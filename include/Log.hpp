// Tiny logging facade so C++ output lines up with the Lua mod's
// `[ScannerAssistCXX/lua]` / `[ScannerAssistCXX/lua] [DBG]` format.
//
// Usage:
//   #include <Log.hpp>
//   namespace SAC = ScannerAssistCore;
//   SAC::Log::info(STR("cache reseed v{}"), version);
//   SAC::Log::debug(STR("refetched {} entries"), n);
//
// Mirrors libs/log.lua: info always emits, debug is gated by the
// compile-time kDebug flag. Trailing newline is appended for you.

#pragma once

#include <utility>

#include <DynamicOutput/DynamicOutput.hpp>

namespace ScannerAssistCore::Log
{
    // Flip to false for a quieter "release" build. Defaults to true so
    // dev iterations get the same diagnostic coverage as the Lua side.
    constexpr bool kDebug = true;

    inline constexpr auto kTag      = STR("[ScannerAssistCXX/cpp] ");
    inline constexpr auto kDebugTag = STR("[ScannerAssistCXX/cpp] [DBG] ");
    inline constexpr auto kNewline  = STR("\n");

    template <typename... Args>
    inline auto info(const RC::File::StringType& fmt, Args&&... args) -> void
    {
        RC::Output::send<RC::LogLevel::Default>(
            RC::File::StringType(kTag) + fmt + kNewline,
            std::forward<Args>(args)...);
    }

    template <typename... Args>
    inline auto debug(const RC::File::StringType& fmt, Args&&... args) -> void
    {
        if constexpr (!kDebug) return;
        RC::Output::send<RC::LogLevel::Verbose>(
            RC::File::StringType(kDebugTag) + fmt + kNewline,
            std::forward<Args>(args)...);
    }
} // namespace ScannerAssistCore::Log
