// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "console/Password.h"

#include <cstdint>
#include <cstdio>

#if defined(VOX_HAVE_CONSOLE_SERVER)
#include <sodium.h>
#endif

namespace vox::console {

bool HashingIsCryptographic() {
#if defined(VOX_HAVE_CONSOLE_SERVER)
    return true;
#else
    return false;
#endif
}

namespace {
constexpr const char* kStubPrefix = "argon2id-stub$";

[[maybe_unused]] std::string StubHash(std::string_view pw) {
    // FNV-1a, hex. NOT secure -- a placeholder until libsodium is linked, just
    // so the skeleton stores a hash-shaped, non-reversible-at-a-glance token
    // instead of the plaintext.
    std::uint64_t h = 1469598103934665603ull;
    for (char c : pw) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(kStubPrefix) + buf;
}
}  // namespace

std::string HashPassword(std::string_view plaintext) {
#if defined(VOX_HAVE_CONSOLE_SERVER)
    if (sodium_init() < 0) {
        return {};
    }
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(out, plaintext.data(), plaintext.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        return {};  // out of memory
    }
    return std::string(out);
#else
    return StubHash(plaintext);
#endif
}

bool VerifyPassword(std::string_view plaintext, std::string_view hash) {
#if defined(VOX_HAVE_CONSOLE_SERVER)
    std::string h(hash);
    return crypto_pwhash_str_verify(h.c_str(), plaintext.data(), plaintext.size()) == 0;
#else
    return StubHash(plaintext) == hash;
#endif
}

}  // namespace vox::console
