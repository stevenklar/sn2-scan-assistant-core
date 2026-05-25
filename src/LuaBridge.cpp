// Every Lua-callable function the C++ core exposes. Trampolines are
// captureless because `LuaFunction = int(*)(const Lua&)` has no
// capture slot — they reach mod state via Mod::s_instance.
//
// Public surface: registerLuaFunctions(Lua&). Called from
// Mod::on_lua_start for each Lua state we want to expose to.

#include <LuaBridge.hpp>
#include <ScannerAssistCore.hpp>
#include <Log.hpp>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>

#include <lua.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Local copy of the wide→narrow helper (a duplicate of the one in
    // ScannerAssistCore.cpp). Kept file-local so neither TU has to
    // expose it through a header.
    auto wideToUtf8(const RC::StringType& w) -> std::string
    {
        std::string out;
        out.reserve(w.size());
        for (auto c : w) out.push_back(static_cast<char>(c < 128 ? c : '?'));
        return out;
    }
}

namespace ScannerAssistCore
{
    using namespace RC::LuaMadeSimple;
    using namespace RC::Unreal;

    // ---- Heartbeat ------------------------------------------------------

    static auto lua_heartbeat(const Lua& lua) -> int
    {
        const auto tick = Mod::s_instance ? Mod::s_instance->getTickCounter()
                                          : std::uint64_t{0};
        lua.set_integer(static_cast<int64_t>(tick));
        return 1;
    }

    // ---- Active gate ----------------------------------------------------

    // Lua tells us whether to run heavy work. We skip reseed when this
    // is false so the C++ side doesn't churn FindAllOf in menus / on
    // save-load idle. See on_update.
    static auto lua_setActive(const Lua& lua) -> int
    {
        if (Mod::s_instance)
        {
            lua_State* L = lua.get_lua_state();
            Mod::s_instance->setActive(lua_toboolean(L, 1) != 0);
        }
        return 0;
    }

    // ---- Cache -----------------------------------------------------------

    static auto lua_cacheVersion(const Lua& lua) -> int
    {
        const auto v = Mod::s_instance ? Mod::s_instance->getCacheVersion()
                                       : std::uint64_t{0};
        lua.set_integer(static_cast<int64_t>(v));
        return 1;
    }

    // Returns a Lua table keyed by entry.key (the GetFullName string),
    // each value a sub-table {cname, key, X, Y, Z}.
    static auto lua_getCache(const Lua& lua) -> int
    {
        lua_State* L = lua.get_lua_state();
        if (!Mod::s_instance)
        {
            lua_createtable(L, 0, 0);
            return 1;
        }
        const auto& cache = Mod::s_instance->getCache();
        lua_createtable(L, 0, static_cast<int>(cache.size()));
        for (const auto& e : cache)
        {
            lua_createtable(L, 0, 5);
            lua_pushlstring(L, e.cname.data(), e.cname.size()); lua_setfield(L, -2, "cname");
            lua_pushlstring(L, e.key.data(),   e.key.size());   lua_setfield(L, -2, "key");
            lua_pushnumber(L, e.X); lua_setfield(L, -2, "X");
            lua_pushnumber(L, e.Y); lua_setfield(L, -2, "Y");
            lua_pushnumber(L, e.Z); lua_setfield(L, -2, "Z");
            lua_setfield(L, -2, e.key.c_str());
        }
        return 1;
    }

    static auto lua_markCacheStale(const Lua& /*lua*/) -> int
    {
        if (Mod::s_instance) Mod::s_instance->markCacheStale();
        return 0;
    }

    // Diagnostic — returns cname -> count for everything currently in
    // m_cache. Lua's keybinds.lua wires this to Shift+F10. Lets the
    // player see what's been picked up so we can tell whether an
    // unrecognised resource (e.g. BP_NecroleiCystFruit_C) is missing
    // from cache entirely vs only failing the filter-token match.
    static auto lua_dumpCacheByClass(const Lua& lua) -> int
    {
        lua_State* L = lua.get_lua_state();
        if (!Mod::s_instance)
        {
            lua_createtable(L, 0, 0);
            return 1;
        }
        std::unordered_map<std::string, std::size_t> counts;
        for (const auto& e : Mod::s_instance->getCache())
            ++counts[e.cname];
        lua_createtable(L, 0, static_cast<int>(counts.size()));
        for (const auto& kv : counts)
        {
            lua_pushinteger(L, static_cast<lua_Integer>(kv.second));
            lua_setfield(L, -2, kv.first.c_str());
        }
        return 1;
    }

    // ---- ScanTally ------------------------------------------------------

    // Two-return signature:
    //   entries = array of {key,cname,token,size,X,Y,Z,dist}
    //   stats   = {seen, kept, noToken, sizeReject, outOfRange}
    static auto lua_scanTally(const Lua& lua) -> int
    {
        lua_State* L = lua.get_lua_state();
        if (!Mod::s_instance)
        {
            lua_createtable(L, 0, 0);
            lua_createtable(L, 0, 0);
            return 2;
        }

        // arg 1: player {X,Y,Z} or nil
        bool   hasPlayer = false;
        double px = 0, py = 0, pz = 0;
        if (lua_istable(L, 1))
        {
            hasPlayer = true;
            lua_getfield(L, 1, "X"); px = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, 1, "Y"); py = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, 1, "Z"); pz = lua_tonumber(L, -1); lua_pop(L, 1);
        }

        // arg 2: stations array
        struct Station
        {
            double X = 0, Y = 0, Z = 0, range2 = 0;
            std::unordered_set<std::string> allow;
        };
        std::vector<Station> stations;
        if (lua_istable(L, 2))
        {
            const auto n = static_cast<int>(luaL_len(L, 2));
            stations.reserve(static_cast<std::size_t>(n));
            for (int i = 1; i <= n; ++i)
            {
                lua_geti(L, 2, i);
                if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
                Station st;
                lua_getfield(L, -1, "X");      st.X      = lua_tonumber(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "Y");      st.Y      = lua_tonumber(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "Z");      st.Z      = lua_tonumber(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "range2"); st.range2 = lua_tonumber(L, -1); lua_pop(L, 1);

                lua_getfield(L, -1, "allow");
                if (lua_istable(L, -1))
                {
                    lua_pushnil(L);
                    while (lua_next(L, -2) != 0)
                    {
                        std::size_t klen = 0;
                        const char* k = lua_tolstring(L, -2, &klen);
                        if (k) st.allow.emplace(k, klen);
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);  // allow
                lua_pop(L, 1);  // station
                stations.push_back(std::move(st));
            }
        }

        // args 3-5: size visibility flags. Treat explicit `false` as
        // hide; missing/nil defaults to show.
        const bool showSmall  = !lua_isboolean(L, 3) || lua_toboolean(L, 3) != 0;
        const bool showMedium = !lua_isboolean(L, 4) || lua_toboolean(L, 4) != 0;
        const bool showBig    = !lua_isboolean(L, 5) || lua_toboolean(L, 5) != 0;

        // iterate cache and filter
        const auto& cache = Mod::s_instance->getCache();
        lua_createtable(L, static_cast<int>(cache.size()), 0);
        int outIdx = 1;
        std::size_t seen = 0, kept = 0;
        std::size_t noToken = 0, sizeReject = 0, tokenReject = 0, outOfRange = 0;

        for (const auto& e : cache)
        {
            ++seen;
            if (e.token.empty()) { ++noToken; continue; }

            const std::string& sz = e.size;
            if      (sz == "small"  && !showSmall)  { ++sizeReject; continue; }
            else if (sz == "medium" && !showMedium) { ++sizeReject; continue; }
            else if (sz == "big"    && !showBig)    { ++sizeReject; continue; }

            // Token match is a PREFIX check, not an exact-equality
            // check. SN2 names biological leaves like
            // BP_NecroleiCystFruit_C ("NecroleiCystFruit") while their
            // scanner filter is DA_NecroleiCyst_ScannerStationFilter
            // ("NecroleiCyst") — exact-match would miss them. Same
            // pattern for AcidAnemoneFruit, CherimoyaRotsac_Cage, etc.
            // We require the actor token to BEGIN with the full filter
            // token, so "Iron" still wouldn't authorise an unrelated
            // "Ironclad" actor (no such case today, but the asymmetry
            // is the guardrail).
            //
            // Counters distinguish "no station allowed this token" from
            // "a station allowed the token but the actor wasn't in
            // range" — they used to be merged into outOfRange which
            // was misleading.
            bool tokenAllowed = false;
            bool inRange = false;
            for (const auto& st : stations)
            {
                // Defensive: ignore stations at (0,0,0). scanner.lua
                // briefly indexes new stations before their components
                // have read their world location; during that window
                // the station's position is the sentinel origin and
                // would green-light any actor whose cached position is
                // also (0,0,0) (cleaned up in reseed but in case one
                // slips through). The ghost gets compacted by Lua a
                // few seconds later — until then we just skip it.
                if (st.X == 0.0 && st.Y == 0.0 && st.Z == 0.0) continue;
                bool tokenOk = false;
                for (const auto& allowed : st.allow)
                {
                    if (allowed.empty()) continue;
                    if (e.token.size() >= allowed.size()
                        && std::equal(allowed.begin(), allowed.end(), e.token.begin()))
                    {
                        tokenOk = true;
                        break;
                    }
                }
                if (!tokenOk) continue;
                tokenAllowed = true;
                const double dx = e.X - st.X;
                const double dy = e.Y - st.Y;
                const double dz = e.Z - st.Z;
                if (dx * dx + dy * dy + dz * dz <= st.range2)
                {
                    inRange = true;
                    break;
                }
            }
            if (!tokenAllowed) { ++tokenReject; continue; }
            if (!inRange)     { ++outOfRange;  continue; }

            double dist = 0.0;
            if (hasPlayer)
            {
                const double dx = e.X - px;
                const double dy = e.Y - py;
                const double dz = e.Z - pz;
                dist = std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0;
            }

            lua_createtable(L, 0, 8);
            lua_pushlstring(L, e.key.data(),   e.key.size());   lua_setfield(L, -2, "key");
            lua_pushlstring(L, e.cname.data(), e.cname.size()); lua_setfield(L, -2, "cname");
            lua_pushlstring(L, e.token.data(), e.token.size()); lua_setfield(L, -2, "token");
            lua_pushlstring(L, e.size.data(),  e.size.size());  lua_setfield(L, -2, "size");
            lua_pushnumber(L, e.X);    lua_setfield(L, -2, "X");
            lua_pushnumber(L, e.Y);    lua_setfield(L, -2, "Y");
            lua_pushnumber(L, e.Z);    lua_setfield(L, -2, "Z");
            lua_pushnumber(L, dist);   lua_setfield(L, -2, "dist");
            lua_seti(L, -2, outIdx++);
            ++kept;
        }

        // Second return: stats for accurate Lua-side scan log.
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, static_cast<lua_Integer>(seen));        lua_setfield(L, -2, "seen");
        lua_pushinteger(L, static_cast<lua_Integer>(kept));        lua_setfield(L, -2, "kept");
        lua_pushinteger(L, static_cast<lua_Integer>(noToken));     lua_setfield(L, -2, "noToken");
        lua_pushinteger(L, static_cast<lua_Integer>(sizeReject));  lua_setfield(L, -2, "sizeReject");
        lua_pushinteger(L, static_cast<lua_Integer>(tokenReject)); lua_setfield(L, -2, "tokenReject");
        lua_pushinteger(L, static_cast<lua_Integer>(outOfRange));  lua_setfield(L, -2, "outOfRange");
        return 2;
    }

    // ---- OrbsDedupe -----------------------------------------------------

    // Orbs/actorPings are passed as coord-only arrays — Lua keeps the
    // rich data (key/label/icon/stationId) and we just tell it which
    // orbs survived dedupe and what their distance is. Returns array
    // of {idx (1-based into orbs), dist (metres)}.
    static auto lua_orbsDedupe(const Lua& lua) -> int
    {
        lua_State* L = lua.get_lua_state();

        struct Vec3 { double X = 0, Y = 0, Z = 0; };
        auto readVec3Array = [&](int argIdx, std::vector<Vec3>& out) {
            if (!lua_istable(L, argIdx)) return;
            const auto n = static_cast<int>(luaL_len(L, argIdx));
            out.reserve(static_cast<std::size_t>(n));
            for (int i = 1; i <= n; ++i)
            {
                lua_geti(L, argIdx, i);
                if (lua_istable(L, -1))
                {
                    Vec3 v;
                    lua_getfield(L, -1, "X"); v.X = lua_tonumber(L, -1); lua_pop(L, 1);
                    lua_getfield(L, -1, "Y"); v.Y = lua_tonumber(L, -1); lua_pop(L, 1);
                    lua_getfield(L, -1, "Z"); v.Z = lua_tonumber(L, -1); lua_pop(L, 1);
                    out.push_back(v);
                }
                else
                {
                    out.push_back({}); // preserve index alignment
                }
                lua_pop(L, 1);
            }
        };

        std::vector<Vec3> orbs;
        std::vector<Vec3> aps;
        readVec3Array(1, orbs);
        readVec3Array(2, aps);

        const double mergeR2 = lua_tonumber(L, 3);

        bool   hasPlayer = false;
        double px = 0, py = 0, pz = 0;
        if (lua_istable(L, 4))
        {
            hasPlayer = true;
            lua_getfield(L, 4, "X"); px = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, 4, "Y"); py = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, 4, "Z"); pz = lua_tonumber(L, -1); lua_pop(L, 1);
        }

        lua_createtable(L, static_cast<int>(orbs.size()), 0);
        int outIdx = 1;
        for (std::size_t i = 0; i < orbs.size(); ++i)
        {
            const auto& o = orbs[i];
            bool dup = false;
            for (const auto& a : aps)
            {
                const double dx = o.X - a.X;
                const double dy = o.Y - a.Y;
                const double dz = o.Z - a.Z;
                if (dx * dx + dy * dy + dz * dz <= mergeR2) { dup = true; break; }
            }
            if (dup) continue;

            double dist = 0.0;
            if (hasPlayer)
            {
                const double dx = o.X - px;
                const double dy = o.Y - py;
                const double dz = o.Z - pz;
                dist = std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0;
            }

            lua_createtable(L, 0, 2);
            lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
            lua_setfield(L, -2, "idx");
            lua_pushnumber(L, dist);
            lua_setfield(L, -2, "dist");
            lua_seti(L, -2, outIdx++);
        }
        return 1;
    }

    // ---- Project points (batched) --------------------------------------

    // Calling PlayerController.ProjectWorldLocationToScreen via the
    // UE4SS Lua wrapper costs ~80µs per point. With 25 markers at
    // 100Hz that's 200k µs/s = 20% of one CPU thread just for the
    // projection step. AMD CPUs hit it especially hard because the
    // wrapper path is branch-heavy.
    //
    // Doing the same UFunction call from C++ via ProcessEvent bypasses
    // the Lua wrapper entirely. We resolve the parameter offsets on
    // the function once, then just memcpy world location into the
    // parm buffer per call. ~5–10µs per call. Plus only ONE Lua/C++
    // bridge crossing per frame instead of N.

    // Class-level metadata only. We deliberately do NOT cache the
    // PlayerController instance — it gets recycled across world
    // transitions / respawns / possession changes, and the Lua-driven
    // invalidator can't be trusted to run before the next projection
    // call. Calling ProcessEvent on a freed PC crashes the engine
    // inside UObject::ProcessEvent (observed in user crash report).
    //
    // The UFunction + property offsets ARE stable across the session
    // (they live on PlayerController's UClass, not on the instance) so
    // those stay cached.
    struct ProjectionCache
    {
        UFunction* fn = nullptr;
        int32_t worldLocOffset    = -1;
        int32_t screenLocOffset   = -1;
        int32_t viewportRelOffset = -1;
        int32_t returnValueOffset = -1;
        std::size_t parmsSize     = 0;

        auto clear() -> void
        {
            fn = nullptr;
            // Offsets are reset on next resolve.
        }
    };
    static ProjectionCache s_projCache;

    // Resolve a live PlayerController per call. FindFirstOf walks the
    // engine's live UObject table — the returned pointer is guaranteed
    // valid for at least this call.
    static auto resolveLivePC() -> UObject*
    {
        return UObjectGlobals::FindFirstOf(STR("PlayerController"));
    }

    static auto ensureProjectionCache(UObject* pc) -> bool
    {
        if (!pc) return false;
        if (s_projCache.fn) return true;

        UFunction* fn = pc->GetFunctionByNameInChain(STR("ProjectWorldLocationToScreen"));
        if (!fn) return false;

        int32_t wOff = -1, sOff = -1, vOff = -1, rOff = -1;
        for (FProperty* p : TFieldRange<FProperty>(fn))
        {
            if (!p) continue;
            const std::string n = wideToUtf8(p->GetName());
            if      (n == "WorldLocation")            wOff = p->GetOffset_ForInternal();
            else if (n == "ScreenLocation")           sOff = p->GetOffset_ForInternal();
            else if (n == "bPlayerViewportRelative")  vOff = p->GetOffset_ForInternal();
            else if (n == "ReturnValue")              rOff = p->GetOffset_ForInternal();
        }
        if (wOff < 0 || sOff < 0 || vOff < 0 || rOff < 0) return false;

        s_projCache.fn                = fn;
        s_projCache.worldLocOffset    = wOff;
        s_projCache.screenLocOffset   = sOff;
        s_projCache.viewportRelOffset = vOff;
        s_projCache.returnValueOffset = rOff;
        s_projCache.parmsSize         = static_cast<std::size_t>(fn->GetParmsSize());

        Log::debug(
            STR("projection: cached ProjectWorldLocationToScreen (WLoc={}, ScrLoc={}, VRel={}, Ret={}, parms={})"),
            wOff, sOff, vOff, rOff, s_projCache.parmsSize);
        return true;
    }

    // Lua-exposed cache invalidator. Kept callable from Lua for API
    // stability, but the PC pointer is no longer cached — this now
    // only forces re-resolution of the UFunction (defensive, e.g. on
    // mod reload).
    static auto lua_invalidateProjection(const Lua& /*lua*/) -> int
    {
        s_projCache.clear();
        return 0;
    }

    // ---- Station UObject* cache (used by FetchOrbs) --------------------
    //
    // FetchOrbs used to call UObjectGlobals::FindAllOf("SN2BaseScannerStation")
    // on every scan cycle (every 500 ms). FindAllOf walks the global UObject
    // table — O(all_objects) per call. We get the same answer by keying the
    // wanted full-names against a persistent map and only rebuilding when
    // a requested name isn't in the cache.
    //
    // Lifecycle:
    //   * Rebuilt on demand when any wantedName is missing.
    //   * Cleared by lua_invalidateStations on world-gone / mod disable
    //     so we never dereference a freed pointer post world-unload.
    //   * Lua removes station entries on EndPlay, so the wantedNames set
    //     never includes a station that's been destroyed in-world.
    static std::unordered_map<std::string, UObject*> s_stationCache;

    static auto lua_invalidateStations(const Lua& /*lua*/) -> int
    {
        s_stationCache.clear();
        return 0;
    }

    // Lua call:
    //   local results = ScannerAssistCore_ProjectPoints(points)
    // where points = array of {X, Y, Z} world locations. Returns an
    // array of {X, Y, ok} screen positions in RAW PIXEL coordinates
    // — caller divides by viewport scale to get slate units. We don't
    // include the scale here because the caller fetches it once per
    // frame anyway and we'd just be marshaling extra data.
    static auto lua_projectPoints(const Lua& lua) -> int
    {
        lua_State* L = lua.get_lua_state();

        int nPoints = 0;
        if (lua_istable(L, 1)) nPoints = static_cast<int>(luaL_len(L, 1));

        lua_createtable(L, nPoints, 0);
        if (nPoints == 0) return 1;

        // Re-fetch the PlayerController every call. Caching it across
        // frames crashes after world transitions / respawns because the
        // pointer goes stale and ProcessEvent dereferences freed memory.
        UObject* pc = resolveLivePC();
        if (!ensureProjectionCache(pc)) return 1;

        std::vector<uint8_t> buf(s_projCache.parmsSize);

        for (int i = 1; i <= nPoints; ++i)
        {
            lua_geti(L, 1, i);
            double x = 0, y = 0, z = 0;
            bool gotPoint = false;
            if (lua_istable(L, -1))
            {
                lua_getfield(L, -1, "X"); x = lua_tonumber(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "Y"); y = lua_tonumber(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "Z"); z = lua_tonumber(L, -1); lua_pop(L, 1);
                gotPoint = true;
            }
            lua_pop(L, 1);

            double screenX = 0.0, screenY = 0.0;
            bool ok = false;
            if (gotPoint)
            {
                std::memset(buf.data(), 0, s_projCache.parmsSize);
                *reinterpret_cast<FVector*>(buf.data() + s_projCache.worldLocOffset) = FVector(x, y, z);
                *reinterpret_cast<bool*>(buf.data() + s_projCache.viewportRelOffset) = false;

                pc->ProcessEvent(s_projCache.fn, buf.data());

                ok = *reinterpret_cast<bool*>(buf.data() + s_projCache.returnValueOffset);
                FVector2D* sl = reinterpret_cast<FVector2D*>(buf.data() + s_projCache.screenLocOffset);
                screenX = sl->X();
                screenY = sl->Y();
            }

            lua_createtable(L, 0, 3);
            lua_pushnumber(L, screenX); lua_setfield(L, -2, "X");
            lua_pushnumber(L, screenY); lua_setfield(L, -2, "Y");
            lua_pushboolean(L, ok);     lua_setfield(L, -2, "ok");
            lua_seti(L, -2, i);
        }
        return 1;
    }

    // ---- FetchOrbs ------------------------------------------------------

    // Reads each station's ActivePoints array (the scanner hologram
    // cloud) and returns flat orb data. Replaces the per-orb pcall
    // loop in scanner.lua's getStationPings — that did 4 UE4SS Lua
    // wrapper property reads per orb (~80μs each) and hit 50 ms on
    // dense Titanium fields. The C++ version is direct memory reads
    // (one indirection per field) so it's roughly 100× faster.
    //
    // Lua call site:
    //   local raw = ScannerAssistCore_FetchOrbs(stationFullNames)
    //     -> array of {X, Y, Z, count, stationKey, pointIdx}
    // Caller is expected to enrich each entry with label/icon — those
    // are Lua-side state (filter Name FText, soft Thumbnail ref).
    //
    // We use FScriptArrayHelper_InContainer for the TArray walk and
    // cache the WorldLocation/Count offsets across calls. Offsets live
    // on the UScriptStruct, which is shared across instances — one
    // discovery per game run is enough.
    static auto lua_fetchOrbs(const Lua& lua) -> int
    {
        lua_State* L = lua.get_lua_state();

        // arg 1: array of station full-name strings (set)
        std::unordered_set<std::string> wantedNames;
        if (lua_istable(L, 1))
        {
            const auto n = static_cast<int>(luaL_len(L, 1));
            wantedNames.reserve(static_cast<std::size_t>(n));
            for (int i = 1; i <= n; ++i)
            {
                lua_geti(L, 1, i);
                if (lua_isstring(L, -1))
                {
                    std::size_t len = 0;
                    const char* s = lua_tolstring(L, -1, &len);
                    if (s) wantedNames.emplace(s, len);
                }
                lua_pop(L, 1);
            }
        }

        lua_createtable(L, 0, 0);
        int outIdx = 1;

        if (wantedNames.empty()) return 1;

        // Rebuild the station cache only when a wantedName isn't in it.
        // Steady state (same stations as last call) = no FindAllOf.
        bool needRebuild = false;
        for (const auto& name : wantedNames)
        {
            if (s_stationCache.find(name) == s_stationCache.end())
            {
                needRebuild = true;
                break;
            }
        }
        if (needRebuild)
        {
            s_stationCache.clear();
            std::vector<UObject*> stations;
            UObjectGlobals::FindAllOf(STR("SN2BaseScannerStation"), stations);
            for (UObject* st : stations)
            {
                if (!st) continue;
                s_stationCache.emplace(wideToUtf8(st->GetFullName()), st);
            }
        }

        // Struct-field offset cache. The ActivePoint struct layout
        // doesn't change at runtime; one resolution per UScriptStruct
        // is enough.
        static int32_t s_worldLocOffset = -1;
        static int32_t s_countOffset    = -1;
        static const UScriptStruct* s_resolvedStruct = nullptr;

        for (const auto& fullName : wantedNames)
        {
            auto it = s_stationCache.find(fullName);
            if (it == s_stationCache.end()) continue;
            UObject* station = it->second;
            if (!station) continue;

            FProperty* prop = station->GetPropertyByNameInChain(STR("ActivePoints"));
            if (!prop) continue;
            FArrayProperty* arrProp = CastField<FArrayProperty>(prop);
            if (!arrProp) continue;
            FProperty* innerProp = arrProp->GetInner();
            if (!innerProp) continue;
            FStructProperty* structProp = CastField<FStructProperty>(innerProp);
            if (!structProp) continue;

            UScriptStruct* scriptStruct = structProp->GetStruct().Get();
            if (!scriptStruct) continue;

            if (scriptStruct != s_resolvedStruct)
            {
                s_worldLocOffset = -1;
                s_countOffset    = -1;
                for (FProperty* f : TFieldRange<FProperty>(scriptStruct))
                {
                    if (!f) continue;
                    const std::string name = wideToUtf8(f->GetName());
                    if      (name == "WorldLocation") s_worldLocOffset = f->GetOffset_ForInternal();
                    else if (name == "Count")         s_countOffset    = f->GetOffset_ForInternal();
                }
                s_resolvedStruct = scriptStruct;
                Log::debug(STR("orbs: resolved ActivePoint offsets (WorldLocation={}, Count={})"),
                           s_worldLocOffset, s_countOffset);
            }
            if (s_worldLocOffset < 0 || s_countOffset < 0) continue;

            FScriptArrayHelper_InContainer helper(arrProp, station);
            const int32_t num = helper.Num();
            for (int32_t i = 0; i < num; ++i)
            {
                uint8_t* elem = helper.GetRawPtr(i);
                if (!elem) continue;
                FVector* loc = reinterpret_cast<FVector*>(elem + s_worldLocOffset);
                const int32_t count = *reinterpret_cast<int32_t*>(elem + s_countOffset);

                const double x = loc->X();
                const double y = loc->Y();
                const double z = loc->Z();
                if (x == 0.0 && y == 0.0 && z == 0.0) continue;

                lua_createtable(L, 0, 6);
                lua_pushnumber(L, x);    lua_setfield(L, -2, "X");
                lua_pushnumber(L, y);    lua_setfield(L, -2, "Y");
                lua_pushnumber(L, z);    lua_setfield(L, -2, "Z");
                lua_pushinteger(L, count); lua_setfield(L, -2, "count");
                lua_pushlstring(L, fullName.data(), fullName.size());
                lua_setfield(L, -2, "stationKey");
                lua_pushinteger(L, i);   lua_setfield(L, -2, "pointIdx");
                lua_seti(L, -2, outIdx++);
            }
        }

        return 1;
    }

    // ---- Registration ---------------------------------------------------

    auto registerLuaFunctions(Lua& lua) -> void
    {
        lua.register_function("ScannerAssistCore_Heartbeat",        &lua_heartbeat);
        lua.register_function("ScannerAssistCore_SetActive",        &lua_setActive);
        lua.register_function("ScannerAssistCore_CacheVersion",     &lua_cacheVersion);
        lua.register_function("ScannerAssistCore_GetCache",         &lua_getCache);
        lua.register_function("ScannerAssistCore_MarkCacheStale",   &lua_markCacheStale);
        lua.register_function("ScannerAssistCore_DumpCacheByClass", &lua_dumpCacheByClass);
        lua.register_function("ScannerAssistCore_ScanTally",        &lua_scanTally);
        lua.register_function("ScannerAssistCore_OrbsDedupe",       &lua_orbsDedupe);
        lua.register_function("ScannerAssistCore_FetchOrbs",        &lua_fetchOrbs);
        lua.register_function("ScannerAssistCore_ProjectPoints",    &lua_projectPoints);
        lua.register_function("ScannerAssistCore_InvalidateProjection", &lua_invalidateProjection);
        lua.register_function("ScannerAssistCore_InvalidateStations",   &lua_invalidateStations);
    }
} // namespace ScannerAssistCore
