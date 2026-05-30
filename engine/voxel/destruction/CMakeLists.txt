# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
#
# vox_destruction: CPU-only destruction algorithms (Milestone B prototype).
# Pure header + .cpp; no DX12, no PhysX, no renderer deps. Designed so the
# in-flight A1/A2/A3 renderer stream cannot conflict with this stream.

add_library(vox_destruction STATIC
    connectivity.h
    connectivity.cpp
    stability.h
    stability.cpp
    box_decompose.h
    box_decompose.cpp
    island_extract.h
    island_extract.cpp
)

target_link_libraries(vox_destruction PUBLIC vox_includes glm::glm)
vox_configure_target(vox_destruction)
