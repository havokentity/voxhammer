# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_cvar STATIC
    Console.cpp
    Console.h
)

target_link_libraries(vox_cvar
    PUBLIC
        vox_includes
        fmt::fmt
        tomlplusplus::tomlplusplus
)

vox_configure_target(vox_cvar)
