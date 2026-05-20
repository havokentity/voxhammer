# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
# Per-target compiler option + warning helpers. Windows-only engine: MSVC and
# clang-cl are the only supported front-ends, so the flag sets assume the
# cl-style driver.

# Baseline options: standard already set globally; this adds the Windows
# hygiene defines + sane parsing flags.
function(vox_target_options target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
    )
    if(MSVC)  # true for both cl and clang-cl
        target_compile_options(${target} PRIVATE
            /utf-8
            /permissive-
            /EHsc
            /Zc:preprocessor
            $<$<CONFIG:Debug>:/Od>
            $<$<NOT:$<CONFIG:Debug>>:/O2>
        )
    endif()
endfunction()

# Warning policy. Kept separate so test / third-party-glue targets can opt out.
# Not warnings-as-errors yet (skeleton pass); tighten in a later pass.
function(vox_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /wd4100 /wd4201)
    endif()
endfunction()

# Convenience: apply both. Most subsystem libs call this.
function(vox_configure_target target)
    vox_target_options(${target})
    vox_target_warnings(${target})
endfunction()
