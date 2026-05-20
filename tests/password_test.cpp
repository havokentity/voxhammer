// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "console/Password.h"

#include <string>

using namespace vox::console;

TEST_CASE("HashPassword: result is non-empty") {
    std::string h = HashPassword("hunter2");
    CHECK_FALSE(h.empty());
}

TEST_CASE("HashPassword: result is not the plaintext") {
    const std::string pw = "correct-horse-battery-staple";
    std::string h = HashPassword(pw);
    CHECK(h != pw);
    CHECK(h.find(pw) == std::string::npos);
}

TEST_CASE("HashPassword: two calls with the same password yield different hashes") {
    // Argon2id uses a random salt; two hashes must differ. The stub uses a
    // deterministic FNV hash, so this test is advisory for the stub path but
    // mandatory when HashingIsCryptographic() is true.
    std::string h1 = HashPassword("samepassword");
    std::string h2 = HashPassword("samepassword");
    if (HashingIsCryptographic()) {
        CHECK(h1 != h2);  // random salt
    } else {
        // stub is deterministic — just verify both are equal (same FNV result)
        CHECK(h1 == h2);
    }
}

TEST_CASE("VerifyPassword: correct password verifies") {
    const std::string pw = "v0xhamm3r!";
    std::string h = HashPassword(pw);
    REQUIRE_FALSE(h.empty());
    CHECK(VerifyPassword(pw, h));
}

TEST_CASE("VerifyPassword: wrong password fails") {
    std::string h = HashPassword("rightpassword");
    CHECK_FALSE(VerifyPassword("wrongpassword", h));
}

TEST_CASE("VerifyPassword: empty password hashes and verifies correctly") {
    std::string h = HashPassword("");
    REQUIRE_FALSE(h.empty());
    CHECK(VerifyPassword("", h));
    CHECK_FALSE(VerifyPassword("not-empty", h));
}

TEST_CASE("VerifyPassword: password with special characters") {
    const std::string pw = "p@$$w0rd!#%^&*()_+-=[]{}|;':\",./<>?";
    std::string h = HashPassword(pw);
    REQUIRE_FALSE(h.empty());
    CHECK(VerifyPassword(pw, h));
    CHECK_FALSE(VerifyPassword("p@$$w0rd", h));
}

TEST_CASE("VerifyPassword: verify against a garbled hash returns false") {
    // We intentionally corrupt a well-formed hash to ensure the verifier
    // doesn't crash and returns false cleanly.
    std::string h = HashPassword("pass");
    if (!h.empty()) {
        h[0] = 'X';
        h[1] = 'X';
        // Should not throw; may return true or false depending on scheme
        // — we only assert it doesn't crash. With argon2id the garbled
        // PHC header will always fail.
        bool ok = VerifyPassword("pass", h);
        if (HashingIsCryptographic()) {
            CHECK_FALSE(ok);  // garbled PHC string -> reject
        }
        // stub: a garbled hash simply won't match FNV(pw); also false
        else {
            CHECK_FALSE(ok);
        }
    }
}

TEST_CASE("HashingIsCryptographic: returns bool without crashing") {
    // Just exercise the function; value depends on build config.
    bool crypto = HashingIsCryptographic();
    (void)crypto;
    CHECK(true);  // reached without crash
}
