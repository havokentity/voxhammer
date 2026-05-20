// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <string>
#include <string_view>

// Forward-declare lua_State so callers that just link vox_script don't need
// LuaJIT headers on their include path.
#if VOX_ENABLE_SCRIPT
struct lua_State;
#endif

namespace vox::script {

// LuaJIT scripting host. Owns a single lua_State.
//
// Compile-time guard: when VOX_ENABLE_SCRIPT=0 (scripting feature disabled),
// Init/Shutdown are no-ops and Eval returns a "scripting disabled" diagnostic
// so all call-sites remain unconditional.
//
// Integration (main.cpp):
//   vox::script::ScriptHost scripts;
//   scripts.Init();          // registers the `lua` console command
//   // per-frame: optionally call scripts.Eval on queued strings
//   scripts.Shutdown();
class ScriptHost {
public:
    void Init();
    void Shutdown();

    // Execute `source` as a Lua chunk.  On success returns tostring of the top
    // of stack (or "" if the stack is empty).  On error returns the Lua error
    // message prefixed with "[lua error] ".
    std::string Eval(std::string_view source);

#if VOX_ENABLE_SCRIPT
private:
    lua_State* L_ = nullptr;
#endif
};

}  // namespace vox::script
