// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <string>
#include <string_view>

namespace vox::console {

// Password hashing for the remote console. Returns an argon2id hash string
// (libsodium crypto_pwhash, interactive params) when the 'console' vcpkg
// feature is built in; otherwise a clearly-labelled, non-cryptographic
// placeholder so the skeleton still never persists plaintext. The hardening
// pass makes argon2id mandatory.
//
// NEVER returns the plaintext.
std::string HashPassword(std::string_view plaintext);

// True iff `plaintext` matches `hash` (whichever scheme produced it).
bool VerifyPassword(std::string_view plaintext, std::string_view hash);

// True when the build links the real argon2id implementation.
bool HashingIsCryptographic();

}  // namespace vox::console
