// ScannerAssistCore — C++ "core" library for the ScannerAssist mod.
//
// Purpose: take heavy or thread-unsafe work off the single-threaded
// UE4SS Lua wrapper. The Lua mod kept crashing on:
//   - NotifyOnNewObject firing from async-loading worker threads,
//   - UObject wrappers expiring across tick boundaries.
// This DLL runs on the engine thread (CppUserModBase::on_update) and
// exposes plain-data accessors via the Lua functions registered in
// LuaBridge.cpp. No UObject wrapper ever crosses the bridge.
//
// File layout:
//   ScannerAssistCore.{hpp,cpp}  Mod class, lifecycle, cache reseed.
//   Classification.hpp           Pure-string helpers (prefix/token/size).
//   LuaBridge.{hpp,cpp}          All Lua trampolines + registration.
//   Log.hpp                      Thin logger that mirrors libs/log.lua.
//   dllmain.cpp                  UE4SS DLL entry point.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Mod/CppUserModBase.hpp>

namespace ScannerAssistCore
{
    // Mod metadata. Centralised so version bumps don't require digging
    // through the cpp. ModVersion follows semver-ish.
    inline constexpr auto kModName        = STR("ScannerAssistCore");
    inline constexpr auto kModVersion     = STR("2.2.0");
    inline constexpr auto kModDescription = STR("ScannerAssist core: off-Lua-thread heavy work.");
    inline constexpr auto kModAuthors     = STR("metrix1337");

    // Lua mods we expose our bridge to. The dev build sits at
    // `_ScannerAssistCXX/` (underscore prefix forces UE4SS to load it
    // first); release ships as `ScannerAssist/`. on_lua_start fires
    // once per Lua mod and we skip any name not in this whitelist so
    // unrelated mods don't get our globals in their Lua state.
    inline constexpr std::array<RC::StringViewType, 2> kOurLuaModNames = {
        STR("_ScannerAssistCXX"),
        STR("ScannerAssist"),
    };

    // Plain-data snapshot of one resource actor. We pull every field
    // we need at reseed time and never retain the UObject*.
    //
    // token + size are precomputed at reseed time (pure string ops on
    // cname). Caching them here means the scan-tally hot loop is a flat
    // string compare per station, not a per-iteration prefix re-parse.
    struct CacheEntry
    {
        std::string cname; // class name, e.g. "BP_ResourceDeposit_Titanium_C"
        std::string key;   // GetFullName — unique identity across the world
        std::string token; // bare resource tag, e.g. "Titanium" ("" if unparseable)
        std::string size;  // "big" | "medium" | "small"
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
    };

    class Mod : public RC::CppUserModBase
    {
    public:
        Mod();
        ~Mod() override;

        auto on_unreal_init() -> void override;
        auto on_update() -> void override;

        auto on_lua_start(RC::StringViewType mod_name,
                          RC::LuaMadeSimple::Lua& lua,
                          RC::LuaMadeSimple::Lua& main_lua,
                          RC::LuaMadeSimple::Lua& async_lua,
                          RC::LuaMadeSimple::Lua* hook_lua) -> void override;

        // === Accessors used by the Lua trampolines.
        auto getTickCounter()  const -> std::uint64_t { return m_tickCounter; }
        auto getCacheVersion() const -> std::uint64_t { return m_cacheVersion; }
        auto getCache()        const -> const std::vector<CacheEntry>& { return m_cache; }
        auto isActive()        const -> bool { return m_active.load(std::memory_order_relaxed); }

        // === Mutators used by the Lua trampolines.
        // setActive: Lua tells us whether the player is in a world and
        // the mod is enabled. We skip reseed when false so menus / main
        // menu / save-load idle don't churn FindAllOf for nothing.
        auto setActive(bool v) -> void { m_active.store(v, std::memory_order_relaxed); }
        auto markCacheStale()  -> void { m_cacheStaleRequested = true; }

        // Singleton pointer for the captureless LuaFunction trampolines
        // (LuaFunction = int(*)(const Lua&) has no capture slot, so
        // free functions reach mod state through this).
        static Mod* s_instance;

    private:
        // === Cache reseed ===
        // Game-thread only. Walks the native parent classes, filters
        // by scannable prefix + bHasBeenGathered, snapshots into
        // m_cache, bumps m_cacheVersion. Cheap (no Lua-wrapper alloc)
        // so no chunking needed.
        auto reseedIfDue() -> void;
        auto reseed() -> void;

        // === Heartbeat ===
        std::chrono::steady_clock::time_point m_lastTick{};
        std::uint64_t m_tickCounter = 0;

        // === Cache state ===
        std::vector<CacheEntry> m_cache;
        std::uint64_t m_cacheVersion = 0;
        std::chrono::steady_clock::time_point m_lastReseedAt{};
        bool m_cacheStaleRequested = true; // force first reseed on startup

        // Atomic because the setter might be called from the Lua
        // thread while on_update reads it — UE4SS routes both through
        // the game thread today but we don't want to depend on that.
        std::atomic<bool> m_active{false};

        bool m_unrealReady = false;
    };
} // namespace ScannerAssistCore
