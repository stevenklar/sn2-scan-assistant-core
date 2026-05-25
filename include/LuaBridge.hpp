// Lua bridge surface — every function we expose to Lua mods is
// registered here. The trampolines themselves are file-local in
// LuaBridge.cpp; only the registration entry point is public.

#pragma once

#include <LuaMadeSimple/LuaMadeSimple.hpp>

namespace ScannerAssistCore
{
    // Registers ScannerAssistCore_* globals into the given Lua state.
    // Called once per Lua state we want to expose to (the four states
    // UE4SS hands us in on_lua_start: lua, main, async, hook).
    auto registerLuaFunctions(RC::LuaMadeSimple::Lua& lua) -> void;
} // namespace ScannerAssistCore
