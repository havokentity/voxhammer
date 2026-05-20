# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
# Helper invoked by add_custom_command (see EmbedResource.cmake). Reads
# ${INPUT}, writes a C++ source at ${OUTPUT} exposing two extern symbols:
#   const unsigned char ${SYMBOL}_data[];
#   const unsigned long ${SYMBOL}_size;

file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_len)
math(EXPR byte_len "${hex_len} / 2")

# Convert hex pairs -> "0xAB,0xCD,..." in a single regex pass (O(N)). A
# foreach + string(APPEND) loop is O(N^2) in CMake script and crawls on
# large assets.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")

# Insert a newline every 16 entries so the emitted cpp isn't one multi-MB
# line (some IDEs / source viewers choke on those). CMake's regex {16}
# quantifier is unreliable, so spell out 16 pairs explicitly.
set(_b "0x[0-9a-f][0-9a-f],")
string(REGEX REPLACE "(${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b}${_b})" "\\1\n" bytes "${bytes}")
unset(_b)

# `extern` on the definition prevents the C++ default of giving const
# namespace-scope variables internal linkage -- without it the compiler can
# (with -fvisibility=hidden) elide them entirely.
set(prelude
"// Auto-generated. Do not edit.
extern \"C\" {
extern const unsigned char ${SYMBOL}_data[];
extern const unsigned long ${SYMBOL}_size;
extern const unsigned char ${SYMBOL}_data[] = {
")
set(epilogue
"};
extern const unsigned long ${SYMBOL}_size = ${byte_len};
}
")
file(WRITE "${OUTPUT}" "${prelude}${bytes}\n${epilogue}")
