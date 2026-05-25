// Pure-string classification helpers for resource class names. No
// engine or UE4SS dependencies — these only operate on the cname
// strings the cache stores. Header-only so the inline expansion is
// guaranteed in the hot reseed/scan paths.
//
// Kept in lockstep with libs/cache.lua's shortLabel/sizeOf and
// libs/scanner.lua's resourceToken — if any of those change in Lua,
// mirror the change here.

#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace ScannerAssistCore::Classification
{
    // Class-name prefixes the in-game scanner pings. Order matters:
    // the broader "BP_" catch-all is last so a more specific prefix
    // wins (e.g. BP_ResourceDeposit_Titanium_C tokenises to "Titanium",
    // not "ResourceDeposit_Titanium").
    inline constexpr std::array<std::string_view, 5> kPrefixes = {
        "BP_ResourceDeposit_",
        "BP_ResourceNode_",
        "ABP_ResonateDeposit_",
        "BP_WorldPopSpawned",
        "BP_",
    };

    inline auto startsWith(const std::string& s, std::string_view p) -> bool
    {
        return s.size() >= p.size()
               && std::equal(p.begin(), p.end(), s.begin());
    }

    inline auto endsWith(const std::string& s, std::string_view p) -> bool
    {
        return s.size() >= p.size()
               && std::equal(p.begin(), p.end(), s.end() - p.size());
    }

    // True if cname matches any known resource-class prefix.
    inline auto isScannable(const std::string& cname) -> bool
    {
        for (const auto& p : kPrefixes)
            if (startsWith(cname, p)) return true;
        return false;
    }

    // Bare resource tag for the scan filter to match against. Strips
    // the known prefix and the "_Destructible_Perfect_C" / "_C"
    // suffixes. Returns "" when cname doesn't begin with any prefix.
    inline auto resourceToken(const std::string& cname) -> std::string
    {
        for (const auto& p : kPrefixes)
        {
            if (!startsWith(cname, p)) continue;
            std::string rest = cname.substr(p.size());
            constexpr std::string_view kDestrSuf = "_Destructible_Perfect_C";
            if (endsWith(rest, kDestrSuf))
                rest.erase(rest.size() - kDestrSuf.size());
            if (endsWith(rest, "_C"))
                rest.erase(rest.size() - 2);
            return rest;
        }
        return {};
    }

    // Size bucket for HUD visual differentiation AND the user's
    // small/medium/big toggle in SN2ModSettings:
    //   "big"    : rich deposit vein            — toggleable
    //   "medium" : mineable node / resonate     — toggleable
    //   "small"  : world-pop loose pickup       — toggleable
    //   "other"  : everything else (biological items like cysts,
    //              rotsacs, anemones, salvage, anything not in the
    //              three canonical shapes above) — ALWAYS shown. The
    //              size gate in lua_scanTally only matches the three
    //              named buckets, so "other" falls through.
    //
    // The "other" bucket exists because SN2's naming for organic /
    // miscellaneous items doesn't follow the BP_Resource{Deposit,Node}
    // shape. Hiding them behind SHOW_SMALL would surprise users who
    // expect the scanner filter to find what the in-game scanner finds.
    inline auto sizeBucket(const std::string& cname) -> std::string
    {
        if (startsWith(cname, "BP_ResourceDeposit_"))   return "big";
        if (startsWith(cname, "BP_ResourceNode_"))      return "medium";
        if (startsWith(cname, "ABP_ResonateDeposit_"))  return "medium";
        if (startsWith(cname, "BP_WorldPopSpawned"))    return "small";
        return "other";
    }
} // namespace ScannerAssistCore::Classification
