// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "script/ScriptHost.h"
#include "cvar/Console.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static vox::script::ScriptHost& host() {
    static vox::script::ScriptHost h;
    static bool inited = false;
    if (!inited) {
        h.Init();
        inited = true;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("ScriptHost: 1+2 evaluates to '3'") {
    auto& h = host();
    std::string result = h.Eval("return 1+2");
    CHECK(result == "3");
}

TEST_CASE("ScriptHost: string concatenation") {
    auto& h = host();
    std::string result = h.Eval("return 'hello' .. ' ' .. 'world'");
    CHECK(result == "hello world");
}

TEST_CASE("ScriptHost: cvar round-trip via vox.cvar_set / vox.cvar_get") {
    // Register a test cvar and round-trip through the real Console.
    auto& con = vox::console::Console::Get();
    con.RegisterCVar("script_test_x", "initial", "unit-test cvar for ScriptHost");

    auto& h = host();
    // Set via Lua binding, then get back.
    std::string set_result = h.Eval("vox.cvar_set('script_test_x', '42'); return vox.cvar_get('script_test_x')");
    CHECK(set_result == "42");
}

TEST_CASE("ScriptHost: syntax error returns diagnostic") {
    auto& h = host();
    std::string result = h.Eval("this is not valid lua !!!");
    CHECK(result.substr(0, 12) == "[lua error] ");
}

TEST_CASE("ScriptHost: vox.exec dispatches a console command") {
    // Register a command that sets a flag.
    static bool fired = false;
    vox::console::Console::Get().RegisterCommand(
        "script_test_cmd", "test",
        [](std::span<const std::string_view>, vox::console::Output&) {
            fired = true;
        });

    auto& h = host();
    h.Eval("vox.exec('script_test_cmd')");
    CHECK(fired);
}
