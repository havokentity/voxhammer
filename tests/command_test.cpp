// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cvar/Console.h"

#include <string>
#include <vector>

using namespace vox::console;

// ---------------------------------------------------------------------------
// TokenizeLine unit tests
// ---------------------------------------------------------------------------

TEST_CASE("TokenizeLine: empty / whitespace-only / comment-only") {
    std::string storage;
    CHECK(TokenizeLine("", storage).empty());
    CHECK(TokenizeLine("   ", storage).empty());
    CHECK(TokenizeLine("// entire line is a comment", storage).empty());
    CHECK(TokenizeLine("  // leading spaces then comment", storage).empty());
}

TEST_CASE("TokenizeLine: bare tokens") {
    std::string storage;
    auto t = TokenizeLine("foo bar baz", storage);
    REQUIRE(t.size() == 3);
    CHECK(t[0] == "foo");
    CHECK(t[1] == "bar");
    CHECK(t[2] == "baz");
}

TEST_CASE("TokenizeLine: quoted string stays one token") {
    std::string storage;
    auto t = TokenizeLine(R"(cmd "hello world" extra)", storage);
    REQUIRE(t.size() == 3);
    CHECK(t[0] == "cmd");
    CHECK(t[1] == "hello world");
    CHECK(t[2] == "extra");
}

TEST_CASE("TokenizeLine: inline comment truncates remaining tokens") {
    std::string storage;
    auto t = TokenizeLine("cmd arg1 // this is ignored arg2", storage);
    REQUIRE(t.size() == 2);
    CHECK(t[0] == "cmd");
    CHECK(t[1] == "arg1");
}

TEST_CASE("TokenizeLine: multiple quoted tokens") {
    std::string storage;
    auto t = TokenizeLine(R"("first token" "second token")", storage);
    REQUIRE(t.size() == 2);
    CHECK(t[0] == "first token");
    CHECK(t[1] == "second token");
}

// ---------------------------------------------------------------------------
// Command registration and Execute dispatch
// ---------------------------------------------------------------------------

TEST_CASE("RegisterCommand + Execute: callback fires with correct args") {
    Console& c = Console::Get();

    std::vector<std::string> captured;
    c.RegisterCommand("cmd.test_dispatch", "test dispatch",
        [&captured](std::span<const std::string_view> args, Output& out) {
            for (auto a : args) {
                captured.emplace_back(a);
            }
            out.PrintLine("dispatched");
        });

    captured.clear();
    auto r = c.Execute("cmd.test_dispatch alpha beta gamma");
    REQUIRE(r.ok);
    REQUIRE(captured.size() == 3);
    CHECK(captured[0] == "alpha");
    CHECK(captured[1] == "beta");
    CHECK(captured[2] == "gamma");
    CHECK(r.output.find("dispatched") != std::string::npos);
}

TEST_CASE("Execute: quoted arg passed as single token to command") {
    Console& c = Console::Get();

    std::string got;
    c.RegisterCommand("cmd.test_quoted", "test quoted arg",
        [&got](std::span<const std::string_view> args, Output&) {
            if (!args.empty()) got = std::string(args[0]);
        });

    got.clear();
    auto r = c.Execute(R"(cmd.test_quoted "hello world")");
    REQUIRE(r.ok);
    CHECK(got == "hello world");
}

TEST_CASE("Execute: unknown command returns not-ok with error") {
    Console& c = Console::Get();
    auto r = c.Execute("no_such_command_xyz");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("no_such_command_xyz") != std::string::npos);
}

TEST_CASE("Execute: empty line is a no-op (ok, no output)") {
    Console& c = Console::Get();
    auto r = c.Execute("   ");
    CHECK(r.ok);
    CHECK(r.output.empty());
    CHECK(r.error.empty());
}

TEST_CASE("Execute: comment-only line is a no-op") {
    Console& c = Console::Get();
    auto r = c.Execute("// set renderer.hdr 1");
    CHECK(r.ok);
    CHECK(r.output.empty());
}

// ---------------------------------------------------------------------------
// ExecuteScript
// ---------------------------------------------------------------------------

TEST_CASE("ExecuteScript: newline and semicolon as statement separators") {
    Console& c = Console::Get();

    c.RegisterCVar("script.a", "0", "script test a",
                   {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 100});
    c.RegisterCVar("script.b", "0", "script test b",
                   {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 100});

    auto r = c.ExecuteScript("script.a 7\nscript.b 13");
    CHECK(r.ok);
    CHECK(c.FindCVar("script.a")->GetInt() == 7);
    CHECK(c.FindCVar("script.b")->GetInt() == 13);

    // Reset and try semicolon separator
    r = c.ExecuteScript("script.a 1;script.b 2");
    CHECK(r.ok);
    CHECK(c.FindCVar("script.a")->GetInt() == 1);
    CHECK(c.FindCVar("script.b")->GetInt() == 2);
}

TEST_CASE("ExecuteScript: one bad statement sets ok=false but others still apply") {
    Console& c = Console::Get();

    c.RegisterCVar("script.good", "0", "good cvar",
                   {.type = CVarType::Int, .flags = CVAR_NONE, .range_min = 0, .range_max = 100});

    auto r = c.ExecuteScript("script.good 42\nnonsense_cvar_xyz 1");
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK(c.FindCVar("script.good")->GetInt() == 42);
}

// ---------------------------------------------------------------------------
// CVAR TOML edge cases (supplement cvar_roundtrip_test.cpp)
// ---------------------------------------------------------------------------

TEST_CASE("cvar TOML: float round-trip") {
    Console& c = Console::Get();

    c.RegisterCVar("toml.float_rt", "1.5", "float toml test",
                   {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 10.0f});
    CHECK(c.Execute("toml.float_rt 3.14").ok);

    const std::string path = "test_float_rt.toml";
    CHECK(c.SaveCvarsToml(path) >= 1);
    CHECK(c.Execute("toml.float_rt 0.0").ok);
    CHECK(c.LoadCvarsToml(path));

    auto* cv = c.FindCVar("toml.float_rt");
    REQUIRE(cv != nullptr);
    // After reload the float should be ~3.14 (not exactly 0)
    CHECK(cv->GetFloat() > 3.0f);
    CHECK(cv->GetFloat() < 3.2f);

    std::remove(path.c_str());
}

TEST_CASE("cvar TOML: bool round-trip") {
    Console& c = Console::Get();

    c.RegisterCVar("toml.bool_rt", "0", "bool toml test",
                   {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    CHECK(c.Execute("toml.bool_rt true").ok);
    CHECK(c.FindCVar("toml.bool_rt")->GetBool());

    const std::string path = "test_bool_rt.toml";
    CHECK(c.SaveCvarsToml(path) >= 1);
    CHECK(c.Execute("toml.bool_rt false").ok);
    CHECK_FALSE(c.FindCVar("toml.bool_rt")->GetBool());
    CHECK(c.LoadCvarsToml(path));
    CHECK(c.FindCVar("toml.bool_rt")->GetBool());

    std::remove(path.c_str());
}

TEST_CASE("cvar TOML: non-archived cvar is not persisted") {
    Console& c = Console::Get();

    c.RegisterCVar("toml.transient", "42", "not archived", {.type = CVarType::Int});
    CHECK(c.Execute("toml.transient 99").ok);

    const std::string path = "test_transient.toml";
    c.SaveCvarsToml(path);

    // Mutate and reload — transient cvar should stay at 99 (not restored)
    CHECK(c.Execute("toml.transient 0").ok);
    CHECK(c.LoadCvarsToml(path));
    CHECK(c.FindCVar("toml.transient")->GetInt() == 0);

    std::remove(path.c_str());
}

TEST_CASE("cvar TOML: missing file is not an error") {
    Console& c = Console::Get();
    CHECK(c.LoadCvarsToml("definitely_does_not_exist_voxhammer.toml"));
}

TEST_CASE("cvar TOML: schema_version=1 in saved file") {
    Console& c = Console::Get();

    c.RegisterCVar("toml.schema_check", "5", "schema version check",
                   {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 10});
    CHECK(c.Execute("toml.schema_check 7").ok);

    const std::string path = "test_schema_ver.toml";
    CHECK(c.SaveCvarsToml(path) >= 1);

    std::ifstream f(path);
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(body.find("schema_version") != std::string::npos);
    CHECK(body.find('1') != std::string::npos);

    std::remove(path.c_str());
}
