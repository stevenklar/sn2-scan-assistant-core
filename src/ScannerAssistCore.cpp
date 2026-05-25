// Mod lifecycle and cache reseed. Lua bridge trampolines live in
// LuaBridge.cpp; pure-string helpers (token, size, scannable) live
// in include/Classification.hpp.

#include <ScannerAssistCore.hpp>
#include <Classification.hpp>
#include <LuaBridge.hpp>
#include <Log.hpp>

#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace ScannerAssistCore
{
    using namespace RC;
    using namespace RC::Unreal;
    using namespace RC::LuaMadeSimple;

    Mod* Mod::s_instance = nullptr;

    // Reseed cadence. Cheap in C++ (no wrapper allocs), so we can be
    // aggressive without the 30ms hitch the Lua version produced.
    static constexpr int kReseedEveryMs = 5000;

    // Native parent classes to sweep via FindAllOf.
    //   UWEWorldPopResourceBaseActor — world resource deposits / nodes
    //   SN2PickupItem                — loose pickups (BP_<Resource>_C)
    //   UWEBaseItem                  — broader item base; catches
    //                                  inventory-style items that don't
    //                                  inherit from SN2PickupItem (e.g.
    //                                  BP_NecroleiCystFruit_C).
    static const StringType kParentClasses[] = {
        STR("UWEWorldPopResourceBaseActor"),
        STR("SN2PickupItem"),
        STR("UWEBaseItem"),
    };

    // ---- helpers ---------------------------------------------------------

    static auto wideToUtf8(const StringType& w) -> std::string
    {
        std::string out;
        out.reserve(w.size());
        for (auto c : w) out.push_back(static_cast<char>(c < 128 ? c : '?'));
        return out;
    }

    // ---- Mod lifecycle ---------------------------------------------------

    Mod::Mod() : CppUserModBase()
    {
        ModName        = kModName;
        ModVersion     = kModVersion;
        ModDescription = kModDescription;
        ModAuthors     = kModAuthors;
        s_instance     = this;
    }

    Mod::~Mod()
    {
        if (s_instance == this) s_instance = nullptr;
    }

    auto Mod::on_unreal_init() -> void
    {
        m_unrealReady = true;
        Log::info(STR("on_unreal_init — Phase 1 cache ready"));
    }

    auto Mod::on_lua_start(StringViewType mod_name,
                           Lua& lua,
                           Lua& main_lua,
                           Lua& async_lua,
                           Lua* hook_lua) -> void
    {
        // UE4SS fires on_lua_start once per loaded Lua mod. Skip any
        // mod whose name isn't in our whitelist — our globals would
        // otherwise pollute every other mod's Lua state.
        bool isOurs = false;
        for (const auto& accepted : kOurLuaModNames)
        {
            if (mod_name == accepted) { isOurs = true; break; }
        }
        if (!isOurs) return;

        registerLuaFunctions(lua);
        registerLuaFunctions(main_lua);
        registerLuaFunctions(async_lua);
        if (hook_lua) registerLuaFunctions(*hook_lua);
        Log::info(STR("Lua bridge functions registered for '{}'"),
                  StringType(mod_name));
    }

    // ---- Reseed (chunked) -----------------------------------------------

    // Number of actors processed per on_update tick. ~50µs/actor on AMD
    // means a chunk of 200 caps each tick's reseed cost at ~10 ms. With
    // ~2400 actors total, a full reseed completes in ~12 ticks (~200 ms
    // wall time at 60 fps) without any single tick blocking the game
    // thread for the old 120 ms hit.
    static constexpr int kReseedChunkSize = 200;
    static constexpr int kParentCount =
        sizeof(kParentClasses) / sizeof(kParentClasses[0]);

    auto Mod::reseedStart() -> void
    {
        m_reseed.next.clear();
        m_reseed.next.reserve(m_cache.empty() ? 256 : m_cache.size());
        m_reseed.rawList.clear();
        m_reseed.rawIdx       = 0;
        m_reseed.parentIdx    = 0;
        m_reseed.totalSeen    = 0;
        m_reseed.kept         = 0;
        m_reseed.skipNoClass  = 0;
        m_reseed.skipNotScan  = 0;
        m_reseed.skipGathered = 0;
        m_reseed.skipNoLoc    = 0;
        m_reseed.startedAt    = std::chrono::steady_clock::now();
        m_reseeding           = true;
    }

    auto Mod::reseedStep() -> void
    {
        if (!m_reseeding) return;

        int processed = 0;
        while (processed < kReseedChunkSize)
        {
            // Fetch next parent class's actor list when we've drained
            // the current one (or on first entry).
            if (m_reseed.rawIdx >= static_cast<int>(m_reseed.rawList.size()))
            {
                if (m_reseed.parentIdx >= kParentCount)
                {
                    // All parents drained — swap in the new cache.
                    m_cache = std::move(m_reseed.next);
                    ++m_cacheVersion;
                    m_cacheStaleRequested = false;
                    m_reseeding = false;

                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - m_reseed.startedAt)
                                        .count();
                    Log::debug(
                        STR("cache reseed v{}: seen={} kept={} (noClass={} notScan={} gathered={} noLoc={}) in {}ms (chunked)"),
                        m_cacheVersion, m_reseed.totalSeen, m_reseed.kept,
                        m_reseed.skipNoClass, m_reseed.skipNotScan,
                        m_reseed.skipGathered, m_reseed.skipNoLoc, ms);
                    return;
                }
                m_reseed.rawList.clear();
                UObjectGlobals::FindAllOf(
                    kParentClasses[m_reseed.parentIdx].c_str(), m_reseed.rawList);
                m_reseed.totalSeen += m_reseed.rawList.size();
                m_reseed.rawIdx     = 0;
                ++m_reseed.parentIdx;
                continue; // re-enter loop; new list may be empty.
            }

            UObject* obj = m_reseed.rawList[m_reseed.rawIdx++];
            ++processed;
            if (!obj) continue;

            UClass* cls = obj->GetClassPrivate();
            if (!cls) { ++m_reseed.skipNoClass; continue; }
            std::string cname = wideToUtf8(cls->GetName());
            if (!Classification::isScannable(cname)) { ++m_reseed.skipNotScan; continue; }

            // bHasBeenGathered: cached per-class offset. The InChain
            // walk used to fire 2400× per reseed; now ~20× (once per
            // unique class) and the per-actor read is a direct
            // pointer + offset deref.
            std::int32_t gOff;
            auto it = m_gatheredOffsets.find(cls);
            if (it == m_gatheredOffsets.end())
            {
                FProperty* p = obj->GetPropertyByNameInChain(STR("bHasBeenGathered"));
                gOff = p ? p->GetOffset_ForInternal() : -1;
                m_gatheredOffsets[cls] = gOff;
            }
            else
            {
                gOff = it->second;
            }
            if (gOff >= 0)
            {
                const bool gathered = *reinterpret_cast<bool*>(
                    reinterpret_cast<std::uint8_t*>(obj) + gOff);
                if (gathered) { ++m_reseed.skipGathered; continue; }
            }

            AActor* actor = static_cast<AActor*>(obj);
            FVector loc = actor->K2_GetActorLocation();

            // Drop actors at exactly (0,0,0). That's the sentinel
            // origin K2_GetActorLocation returns for things that
            // don't have a world position — inventory items, mid-
            // spawn, etc. We deliberately do NOT also filter by
            // bHidden: SN2 world resources are bHidden=true in normal
            // play (probably LOD culling), filtering on it wipes
            // the cache to zero.
            if (loc.X() == 0.0 && loc.Y() == 0.0 && loc.Z() == 0.0)
            {
                ++m_reseed.skipNoLoc;
                continue;
            }

            CacheEntry e;
            e.token = Classification::resourceToken(cname);
            e.size  = Classification::sizeBucket(cname);
            e.cname = std::move(cname);
            e.key   = wideToUtf8(obj->GetFullName());
            e.X = loc.X();
            e.Y = loc.Y();
            e.Z = loc.Z();
            m_reseed.next.push_back(std::move(e));
            ++m_reseed.kept;
        }
    }

    auto Mod::reseedIfDue() -> void
    {
        // If a reseed is in progress, keep stepping. Don't start a new
        // one until the current one finishes (and the cadence timer
        // fires again).
        if (m_reseeding)
        {
            reseedStep();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - m_lastReseedAt).count();
        if (!m_cacheStaleRequested && sinceMs < kReseedEveryMs) return;
        m_lastReseedAt = now;
        reseedStart();
        reseedStep(); // do the first chunk this tick so progress shows immediately.
    }

    // ---- Tick ------------------------------------------------------------

    auto Mod::on_update() -> void
    {
        if (!m_unrealReady) return;

        // Heartbeat ticks regardless of active state — Lua's
        // core_bridge.lua uses it to detect "DLL alive" even when the
        // mod is in idle/menu state.
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastTick).count() >= 1000)
        {
            m_lastTick = now;
            ++m_tickCounter;
            if (m_tickCounter == 1)
                Log::info(STR("first heartbeat tick — on_update is live"));
        }

        // Cache reseed gated on Lua-driven active flag. Lua flips it
        // true when the player is in a world AND the mod is enabled
        // AND a station exists. Skipping when inactive saves the
        // FindAllOf cost during menus / save-load / mod-disabled.
        const bool active = m_active.load(std::memory_order_relaxed);
        if (m_wasActive && !active)
        {
            // Active → inactive transition (world unload / mod off).
            // UClass* pointers in m_gatheredOffsets may be unloaded
            // along with the world; drop them so we re-resolve fresh
            // when the player loads back in. Also abort any in-flight
            // reseed since its rawList might point at freed actors.
            m_gatheredOffsets.clear();
            m_reseeding = false;
            m_reseed.next.clear();
            m_reseed.rawList.clear();
        }
        m_wasActive = active;

        if (!active) return;
        reseedIfDue();
    }
} // namespace ScannerAssistCore
