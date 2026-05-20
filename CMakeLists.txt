# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
cmake_minimum_required(VERSION 3.27)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "Configure out of source. Use a preset, e.g. `cmake --preset windows-debug`.")
endif()

project(voxhammer
    VERSION 0.1.0
    DESCRIPTION "Voxhammer -- standalone Windows DX12 voxel-destruction engine + sandbox."
    LANGUAGES C CXX
)

# --- Language baseline (C++20, per spec) -----------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Generated version header (reliable string literal across MSVC/clang-cl/Ninja,
# unlike a -D quoted-string definition). Exposed on vox_includes' include path.
configure_file(
    ${CMAKE_SOURCE_DIR}/engine/VoxVersion.h.in
    ${CMAKE_BINARY_DIR}/generated/VoxVersion.h
    @ONLY)

# --- Project options --------------------------------------------------------
# This is the M0 "skeleton" pass. Heavy subsystems are gated OFF so the core
# tree configures + builds against only the always-on vcpkg deps (fmt,
# tomlplusplus, glm, doctest). Flip a feature ON together with its vcpkg
# manifest feature, e.g.:
#   cmake --preset windows-debug -DVCPKG_MANIFEST_FEATURES=console;tls \
#         -DVOX_ENABLE_CONSOLE_SERVER=ON -DVOX_ENABLE_TLS=ON
option(VOX_BUILD_TESTS          "Build doctest unit tests"                 ON)
option(VOX_BUILD_TOOLS          "Build developer tools under tools/"       ON)
option(VOX_ENABLE_CONSOLE_SERVER "Wire civetweb network ConsoleServer (needs vcpkg feature 'console')" ON)
option(VOX_ENABLE_TLS           "HTTPS/WSS + argon2id (needs vcpkg features 'console;tls')" ON)
option(VOX_ENABLE_WINDOW        "GLFW window + input + minimal DX12 swapchain (needs vcpkg feature 'window')" ON)
option(VOX_ENABLE_ISPC          "Compile ISPC kernels (needs ispc on PATH / pinned in thirdparty)" OFF)
option(VOX_ENABLE_TRACY         "Enable Tracy profiler client (needs vcpkg feature 'profiling')" OFF)
option(VOX_ENABLE_JOBS          "enkiTS job system (needs vcpkg feature 'jobs')" OFF)
option(VOX_ENABLE_ECS           "EnTT ECS (needs vcpkg feature 'ecs')"     OFF)

# --- Always-on dependencies (vcpkg core) -----------------------------------
find_package(fmt CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

# --- Helper modules ---------------------------------------------------------
include(cmake/VoxCompilerOptions.cmake)
include(cmake/EmbedResource.cmake)
include(cmake/VoxIspc.cmake)

# --- Sources ----------------------------------------------------------------
add_subdirectory(engine)
add_subdirectory(game)

if(VOX_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(VOX_BUILD_TESTS)
    enable_testing()
    find_package(doctest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()

# --- Configure summary ------------------------------------------------------
message(STATUS "Voxhammer ${PROJECT_VERSION} configured:")
message(STATUS "  C++ standard      : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler          : ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER})")
message(STATUS "  Tests / Tools     : ${VOX_BUILD_TESTS} / ${VOX_BUILD_TOOLS}")
message(STATUS "  ConsoleServer/TLS : ${VOX_ENABLE_CONSOLE_SERVER} / ${VOX_ENABLE_TLS}")
message(STATUS "  ISPC / Tracy      : ${VOX_ENABLE_ISPC} / ${VOX_ENABLE_TRACY}")
message(STATUS "  Jobs / ECS        : ${VOX_ENABLE_JOBS} / ${VOX_ENABLE_ECS}")
