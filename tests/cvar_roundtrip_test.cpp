// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cvar/Console.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace vox::console;

TEST_CASE("cvar set / get, enum + range validation, readonly override") {
    Console& c = Console::Get();

    c.RegisterCVar("test.int", "3", "int cvar",
                   {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 6});
    CVar* iv = c.FindCVar("test.int");
    REQUIRE(iv != nullptr);
    CHECK(iv->GetInt() == 3);

    CHECK(c.Execute("test.int 5").ok);
    CHECK(iv->GetInt() == 5);

    // out-of-range clamps to the declared max
    CHECK(c.Execute("test.int 999").ok);
    CHECK(iv->GetInt() == 6);

    c.RegisterCVar("test.enum", "A", "enum cvar", {.type = CVarType::Enum, .enum_values = {"A", "B"}});
    CHECK_FALSE(c.Execute("test.enum C").ok);      // not a member
    CHECK(c.FindCVar("test.enum")->value == "A");  // unchanged
    CHECK(c.Execute("test.enum B").ok);
    CHECK(c.FindCVar("test.enum")->value == "B");

    c.RegisterCVar("test.ro", "x", "readonly cvar", {.type = CVarType::String, .flags = CVAR_READONLY});
    CHECK_FALSE(c.Execute("test.ro y").ok);         // user/network cannot write
    CHECK(c.SetCVarOverride("test.ro", "y"));        // engine path can
    CHECK(c.FindCVar("test.ro")->value == "y");
}

TEST_CASE("cvars.toml round-trip + schema_version") {
    Console& c = Console::Get();
    c.RegisterCVar("rt.bounces", "3", "archived int",
                   {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 6});
    CHECK(c.Execute("rt.bounces 5").ok);

    const std::string path = "test_cvars_roundtrip.toml";
    CHECK(c.SaveCvarsToml(path) >= 1);

    // schema_version is written (ADR-001)
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string body = ss.str();
    CHECK(body.find("schema_version") != std::string::npos);

    // mutate, then a reload restores the persisted value
    CHECK(c.Execute("rt.bounces 1").ok);
    CHECK(c.FindCVar("rt.bounces")->GetInt() == 1);
    CHECK(c.LoadCvarsToml(path));
    CHECK(c.FindCVar("rt.bounces")->GetInt() == 5);

    std::remove(path.c_str());
}
