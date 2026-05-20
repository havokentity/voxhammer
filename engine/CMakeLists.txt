# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
# The engine is a set of per-subsystem static libraries ("one subsystem per
# static library") aggregated into the vox_engine interface target that the
# game + tools link against.

# Shared include root: every subsystem includes its siblings as
# "<subsystem>/Header.h" (e.g. "platform/Log.h", "cvar/Console.h").
add_library(vox_includes INTERFACE)
target_include_directories(vox_includes INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}        # "<subsystem>/Header.h"
    ${CMAKE_BINARY_DIR}/generated      # configured VoxVersion.h
)

# Order: vox_includes first (everyone links it), then platform (header-only
# Log.h lives there), then the rest. CMake resolves link order regardless, but
# this reads top-down.
add_subdirectory(platform)
add_subdirectory(cvar)
add_subdirectory(console)
add_subdirectory(jobs)
add_subdirectory(ecs)
add_subdirectory(ispc)
add_subdirectory(voxel)
add_subdirectory(physics)
add_subdirectory(render)
add_subdirectory(audio)
add_subdirectory(script)

# Aggregate. Interface target: linking vox_engine pulls in every subsystem
# static lib (and their transitive deps) onto the consumer's link line.
add_library(vox_engine INTERFACE)
target_link_libraries(vox_engine INTERFACE
    vox_platform
    vox_cvar
    vox_console
    vox_jobs
    vox_ecs
    vox_ispc
    vox_voxel
    vox_physics
    vox_render
    vox_audio
    vox_script
)
