// Mod lifecycle and cache reseed. Lua bridge trampolines live in
// LuaBridge.cpp; pure-string helpers (token, size, scannable) live
// in include/Classification.hpp.

#include <ScannerAssistCore.hpp>
#include <Classification.hpp>
#include <LuaBridge.hpp>
#include <Log.hpp>

#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
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

    // ---- Reseed ----------------------------------------------------------

    auto Mod::reseed() -> void
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<CacheEntry> next;
        next.reserve(m_cache.empty() ? 256 : m_cache.size());

        std::size_t totalSeen = 0;
        std::size_t kept = 0;
        std::size_t skipNoClass = 0;
        std::size_t skipNotScan = 0;
        std::size_t skipGathered = 0;
        std::size_t skipNoLoc = 0;

        for (const auto& parent : kParentClasses)
        {
            std::vector<UObject*> raw;
            UObjectGlobals::FindAllOf(parent.c_str(), raw);
            totalSeen += raw.size();

            for (UObject* obj : raw)
            {
                if (!obj) continue;
                UClass* cls = obj->GetClassPrivate();
                if (!cls) { ++skipNoClass; continue; }
                std::string cname = wideToUtf8(cls->GetName());
                if (!Classification::isScannable(cname)) { ++skipNotScan; continue; }

                // bHasBeenGathered: walk the class chain — the property
                // lives on a parent (UWEWorldPopResourceBaseActor) not
                // the BP leaf. Leaf-only lookup would return null and
                // we'd (wrongly) count gathered actors as live.
                auto* gathered = obj->GetValuePtrByPropertyNameInChain<bool>(STR("bHasBeenGathered"));
                if (gathered && *gathered) { ++skipGathered; continue; }

                AActor* actor = static_cast<AActor*>(obj);
                FVector loc = actor->K2_GetActorLocation();

                // Drop actors at exactly (0,0,0). That's the sentinel
                // origin K2_GetActorLocation returns for things that
                // don't actually have a world position — inventory
                // items, items mid-spawn, etc. Without this filter, a
                // momentary ghost station at (0,0,0) green-lights every
                // such actor and we get phantom markers at distance
                // ≈ |player - origin|.
                //
                // We deliberately do NOT also filter by bHidden:
                // SN2 world resources are bHidden=true in normal play
                // (probably LOD culling), and filtering on it wipes
                // the cache to zero. The (0,0,0) check on its own
                // is enough for the inventory case.
                if (loc.X() == 0.0 && loc.Y() == 0.0 && loc.Z() == 0.0)
                {
                    ++skipNoLoc;
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
                next.push_back(std::move(e));
                ++kept;
            }
        }

        m_cache = std::move(next);
        ++m_cacheVersion;
        m_cacheStaleRequested = false;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        Log::debug(
            STR("cache reseed v{}: seen={} kept={} (noClass={} notScan={} gathered={} noLoc={}) in {}ms"),
            m_cacheVersion, totalSeen, kept,
            skipNoClass, skipNotScan, skipGathered, skipNoLoc, ms);
    }

    auto Mod::reseedIfDue() -> void
    {
        const auto now = std::chrono::steady_clock::now();
        const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - m_lastReseedAt).count();
        if (!m_cacheStaleRequested && sinceMs < kReseedEveryMs) return;
        m_lastReseedAt = now;
        reseed();
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
        // AND a station exists. Skipping when inactive saves the 20ms
        // FindAllOf in menus / save load / mod-disabled states.
        if (!m_active.load(std::memory_order_relaxed)) return;
        reseedIfDue();
    }
} // namespace ScannerAssistCore
