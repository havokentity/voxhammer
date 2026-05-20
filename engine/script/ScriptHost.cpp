// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "script/ScriptHost.h"

#include "cvar/Console.h"
#include "platform/Log.h"

#if VOX_ENABLE_SCRIPT
// LuaJIT headers live under <luajit/...> after vcpkg install.
#include <luajit/lua.hpp>
#endif

namespace vox::script {

// ---------------------------------------------------------------------------
// Lua C-function helpers (only compiled when LuaJIT is present)
// ---------------------------------------------------------------------------
#if VOX_ENABLE_SCRIPT

// vox.cvar_get(name) -> string|nil
static int l_cvar_get(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto* cv = vox::console::Console::Get().FindCVar(name);
    if (!cv) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, cv->value.c_str());
    }
    return 1;
}

// vox.cvar_set(name, value) -> nil
static int l_cvar_set(lua_State* L) {
    const char* name  = luaL_checkstring(L, 1);
    const char* value = luaL_checkstring(L, 2);
    auto& con = vox::console::Console::Get();
    auto* cv = con.FindCVar(name);
    if (!cv) {
        lua_pushstring(L, ("cvar not found: " + std::string(name)).c_str());
        lua_error(L);  // noreturn in LuaJIT
    }
    // Use Execute("name value") so the full validation path runs.
    std::string line = std::string(name) + " " + std::string(value);
    auto res = con.Execute(line);
    if (!res.ok) {
        lua_pushstring(L, res.error.c_str());
        lua_error(L);
    }
    return 0;
}

// vox.exec(line) -> output_string
static int l_exec(lua_State* L) {
    const char* line = luaL_checkstring(L, 1);
    auto res = vox::console::Console::Get().Execute(line);
    // Return the combined output (or the error) as a string.
    const std::string& ret = res.ok ? res.output : res.error;
    lua_pushstring(L, ret.c_str());
    return 1;
}

// Register the `vox` table of engine-callable functions.
static void register_vox_lib(lua_State* L) {
    static const luaL_Reg vox_lib[] = {
        {"cvar_get", l_cvar_get},
        {"cvar_set", l_cvar_set},
        {"exec",     l_exec},
        {nullptr, nullptr}
    };
    lua_newtable(L);
    luaL_register(L, nullptr, vox_lib);  // LuaJIT uses luaL_register, not luaL_newlib
    lua_setglobal(L, "vox");
}

#endif  // VOX_ENABLE_SCRIPT

// ---------------------------------------------------------------------------
// ScriptHost
// ---------------------------------------------------------------------------

void ScriptHost::Init() {
#if VOX_ENABLE_SCRIPT
    L_ = luaL_newstate();
    if (!L_) {
        vox::log::Error("script: failed to create lua_State");
        return;
    }
    luaL_openlibs(L_);
    register_vox_lib(L_);

    // Register the `lua` console command so the web console can run Lua.
    vox::console::Console::Get().RegisterCommand(
        "lua",
        "Execute a Lua expression or statement. e.g.  lua \"return 1+2\"",
        [this](std::span<const std::string_view> args, vox::console::Output& out) {
            if (args.empty()) {
                out.PrintLine("usage: lua <expression>");
                return;
            }
            // Concatenate all args (handles quoted strings passed as one arg,
            // or multiple tokens the tokenizer split).
            std::string src;
            for (auto& a : args) {
                if (!src.empty()) src += ' ';
                src += a;
            }
            out.PrintLine(Eval(src));
        });

    vox::log::Info("script: LuaJIT {} ready", LUAJIT_VERSION);
#else
    vox::log::Trace("script: scripting disabled at compile time");
#endif
}

void ScriptHost::Shutdown() {
#if VOX_ENABLE_SCRIPT
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
#endif
}

std::string ScriptHost::Eval(std::string_view source) {
#if VOX_ENABLE_SCRIPT
    if (!L_) return "[lua error] not initialised";

    // Wrap bare expressions in `return` so `Eval("1+2")` yields "3".
    std::string chunk = "return ";
    chunk += source;

    int rc = luaL_loadstring(L_, chunk.c_str());
    if (rc != 0) {
        // Not a valid expression; try as a statement.
        lua_pop(L_, 1);
        rc = luaL_loadbuffer(L_, source.data(), source.size(), "=(eval)");
    }
    if (rc != 0) {
        std::string err = "[lua error] ";
        err += lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return err;
    }

    // Call chunk with 0 args, leaving return values on the stack.
    rc = lua_pcall(L_, 0, LUA_MULTRET, 0);
    if (rc != 0) {
        std::string err = "[lua error] ";
        err += lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return err;
    }

    // Convert the top value to a string (or return "" for void chunks).
    if (lua_gettop(L_) == 0) return "";

    // Use tostring() to coerce any type.
    lua_getglobal(L_, "tostring");
    lua_pushvalue(L_, -2);
    lua_call(L_, 1, 1);
    const char* result = lua_tostring(L_, -1);
    std::string ret = result ? result : "";
    lua_settop(L_, 0);
    return ret;
#else
    (void)source;
    return "scripting disabled (recompile with VOX_ENABLE_SCRIPT=ON)";
#endif
}

}  // namespace vox::script
