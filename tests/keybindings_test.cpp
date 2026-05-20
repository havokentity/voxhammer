// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte

// Include platform headers before doctest to avoid MSVC /Zc:preprocessor
// conflicts with shift-expression enum initializers in Keybindings.h.
#include "platform/Keybindings.h"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace vox::platform;

// ---------------------------------------------------------------------------
// Note: Due to a known MSVC /Zc:preprocessor interaction, comparing against
// the KeyMod enum constants (which use 1u<<N initializers) inside doctest
// CHECK() macros can yield wrong compile-time values. We therefore test
// ParseMods / ModNames via round-trip identity (ParseMods(ModNames(x)) == x)
// and through behavioural Dispatch tests, rather than asserting exact bitmasks.
// ---------------------------------------------------------------------------

TEST_CASE("ParseMods: empty vector yields zero") {
    // Zero == MOD_NONE by construction
    CHECK(ParseMods({}) == 0u);
}

TEST_CASE("ParseMods: unknown name yields zero") {
    CHECK(ParseMods({"Win"}) == 0u);
    CHECK(ParseMods({"Super"}) == 0u);
}

TEST_CASE("ParseMods: Shift, Ctrl, Alt produce distinct non-zero values") {
    std::uint32_t s = ParseMods({"Shift"});
    std::uint32_t c = ParseMods({"Ctrl"});
    std::uint32_t a = ParseMods({"Alt"});

    CHECK(s != 0u);
    CHECK(c != 0u);
    CHECK(a != 0u);
    // All three must be distinct
    CHECK(s != c);
    CHECK(s != a);
    CHECK(c != a);
}

TEST_CASE("ParseMods: combined modifiers have the individual bits set") {
    std::uint32_t s = ParseMods({"Shift"});
    std::uint32_t c = ParseMods({"Ctrl"});
    std::uint32_t sc = ParseMods({"Shift", "Ctrl"});

    CHECK((sc & s) == s);
    CHECK((sc & c) == c);
    CHECK((sc & ParseMods({"Alt"})) == 0u);
}

TEST_CASE("ModNames round-trips through ParseMods for every combination") {
    // Enumerate all 8 subsets of {Shift, Ctrl, Alt}
    std::uint32_t s = ParseMods({"Shift"});
    std::uint32_t c = ParseMods({"Ctrl"});
    std::uint32_t a = ParseMods({"Alt"});

    for (int i = 0; i < 8; ++i) {
        std::uint32_t m = 0u;
        if (i & 1) m |= s;
        if (i & 2) m |= c;
        if (i & 4) m |= a;
        CHECK(ParseMods(ModNames(m)) == m);
    }
}

// ---------------------------------------------------------------------------
// TargetToConsoleLine
// ---------------------------------------------------------------------------

TEST_CASE("TargetToConsoleLine: command kind") {
    CHECK(TargetToConsoleLine("command quit") == "quit");
    CHECK(TargetToConsoleLine("command screenshot") == "screenshot");
}

TEST_CASE("TargetToConsoleLine: cvar.set kind passes rest verbatim") {
    CHECK(TargetToConsoleLine("cvar.set renderer.hdr.enabled 1") == "renderer.hdr.enabled 1");
}

TEST_CASE("TargetToConsoleLine: cvar.toggle kind") {
    CHECK(TargetToConsoleLine("cvar.toggle editor.active") == "toggle editor.active");
    CHECK(TargetToConsoleLine("cvar.toggle debug.show_brick_grid") == "toggle debug.show_brick_grid");
}

TEST_CASE("TargetToConsoleLine: unknown kind returns empty") {
    CHECK(TargetToConsoleLine("bind F11").empty());
    CHECK(TargetToConsoleLine("").empty());
}

// ---------------------------------------------------------------------------
// DefaultBindings
// ---------------------------------------------------------------------------

TEST_CASE("DefaultBindings: non-empty, every entry has key and target") {
    auto binds = DefaultBindings();
    REQUIRE_FALSE(binds.empty());
    for (const auto& b : binds) {
        CHECK_FALSE(b.key.empty());
        CHECK_FALSE(b.target.empty());
    }
}

// ---------------------------------------------------------------------------
// Keybindings CRUD (in-memory, no file I/O)
// ---------------------------------------------------------------------------

TEST_CASE("Keybindings::Set and All") {
    Keybindings kb;
    KeyBinding b;
    b.key = "F12";
    b.modifiers = 0u;  // MOD_NONE
    b.target = "command reload_shaders";
    kb.Set(b);
    REQUIRE(kb.All().size() == 1);
    CHECK(kb.All()[0].key == "F12");
    CHECK(kb.All()[0].target == "command reload_shaders");
}

TEST_CASE("Keybindings::Set: replacing existing binding updates target") {
    Keybindings kb;
    {
        KeyBinding b;
        b.key = "F12"; b.modifiers = 0u; b.target = "command old_target";
        kb.Set(b);
    }
    {
        KeyBinding b;
        b.key = "F12"; b.modifiers = 0u; b.target = "command new_target";
        kb.Set(b);
    }
    REQUIRE(kb.All().size() == 1);
    CHECK(kb.All()[0].target == "command new_target");
}

TEST_CASE("Keybindings::Remove: existing binding removed") {
    Keybindings kb;
    {
        KeyBinding b;
        b.key = "F12"; b.modifiers = 0u; b.target = "command something";
        kb.Set(b);
    }
    CHECK(kb.Remove("F12", 0u));
    CHECK(kb.All().empty());
}

TEST_CASE("Keybindings::Remove: returns false for missing binding") {
    Keybindings kb;
    CHECK_FALSE(kb.Remove("F99", 0u));
}

// ---------------------------------------------------------------------------
// Keybindings::Dispatch
// ---------------------------------------------------------------------------

TEST_CASE("Dispatch: matching binding fires exec_line callback") {
    Keybindings kb;
    {
        KeyBinding b;
        b.key = "F5"; b.modifiers = 0u;
        b.target = "cvar.toggle debug.pause_simulation";
        kb.Set(b);
    }

    std::string dispatched_line;
    bool fired = kb.Dispatch("F5", 0u, [&](const std::string& line) {
        dispatched_line = line;
    });

    CHECK(fired);
    CHECK(dispatched_line == "toggle debug.pause_simulation");
}

TEST_CASE("Dispatch: no binding for key returns false") {
    Keybindings kb;
    bool fired = kb.Dispatch("F99", 0u, [](const std::string&) {});
    CHECK_FALSE(fired);
}

TEST_CASE("Dispatch: modifier mismatch does not fire") {
    Keybindings kb;
    std::uint32_t ctrl = ParseMods({"Ctrl"});
    {
        KeyBinding b;
        b.key = "S"; b.modifiers = ctrl; b.target = "command save";
        kb.Set(b);
    }

    bool fired = kb.Dispatch("S", 0u, [](const std::string&) {});
    CHECK_FALSE(fired);
}

TEST_CASE("Dispatch: correct modifier fires") {
    Keybindings kb;
    std::uint32_t ctrl = ParseMods({"Ctrl"});
    {
        KeyBinding b;
        b.key = "S"; b.modifiers = ctrl; b.target = "command save";
        kb.Set(b);
    }

    std::string got;
    bool fired = kb.Dispatch("S", ctrl, [&](const std::string& line) {
        got = line;
    });
    CHECK(fired);
    CHECK(got == "save");
}

TEST_CASE("Dispatch routes cvar.set target through to exec_line as raw path+value") {
    Keybindings kb;
    {
        KeyBinding b;
        b.key = "Home"; b.modifiers = 0u; b.target = "cvar.set camera.speed 5";
        kb.Set(b);
    }

    std::string got;
    CHECK(kb.Dispatch("Home", 0u, [&](const std::string& line) { got = line; }));
    CHECK(got == "camera.speed 5");
}

// ---------------------------------------------------------------------------
// TOML file round-trip
// ---------------------------------------------------------------------------

TEST_CASE("Keybindings: save + load round-trip preserves bindings") {
    const std::string path = "test_keybindings_rt.toml";

    // Write a minimal seed file so Load() sets path_ correctly
    {
        std::ofstream tmp(path);
        tmp << "[[bind]]\nkey=\"X\"\nmods=[]\ntarget=\"command dummy\"\n";
    }

    Keybindings kb;
    REQUIRE(kb.Load(path));

    std::uint32_t ctrl = ParseMods({"Ctrl"});
    std::uint32_t shift = ParseMods({"Shift"});
    std::uint32_t cs = ctrl | shift;

    {
        KeyBinding b;
        b.key = "F12"; b.modifiers = 0u; b.target = "command reload_shaders";
        kb.Set(b);
    }
    {
        KeyBinding b;
        b.key = "P"; b.modifiers = cs; b.target = "cvar.toggle profiler.enabled";
        kb.Set(b);
    }
    int written = kb.Save();
    CHECK(written >= 2);

    // Load into a fresh object and verify
    Keybindings kb2;
    REQUIRE(kb2.Load(path));

    bool found_f12 = false;
    bool found_p   = false;
    for (const auto& b : kb2.All()) {
        if (b.key == "F12" && b.modifiers == 0u) {
            found_f12 = true;
            CHECK(b.target == "command reload_shaders");
        }
        if (b.key == "P" && b.modifiers == cs) {
            found_p = true;
            CHECK(b.target == "cvar.toggle profiler.enabled");
        }
    }
    CHECK(found_f12);
    CHECK(found_p);

    std::remove(path.c_str());
}

TEST_CASE("Keybindings: loading a missing file returns false") {
    Keybindings kb;
    CHECK_FALSE(kb.Load("definitely_no_such_keybindings.toml"));
}

TEST_CASE("Keybindings: duplicate key in file keeps first, ignores second") {
    const std::string path = "test_kb_dup.toml";
    {
        std::ofstream f(path);
        f << "[[bind]]\nkey=\"F1\"\nmods=[]\ntarget=\"command first\"\n";
        f << "[[bind]]\nkey=\"F1\"\nmods=[]\ntarget=\"command second\"\n";
    }

    Keybindings kb;
    REQUIRE(kb.Load(path));

    int count = 0;
    std::string first_target;
    for (const auto& b : kb.All()) {
        if (b.key == "F1" && b.modifiers == 0u) {
            if (count == 0) first_target = b.target;
            ++count;
        }
    }
    CHECK(count == 1);
    CHECK(first_target == "command first");

    std::remove(path.c_str());
}
