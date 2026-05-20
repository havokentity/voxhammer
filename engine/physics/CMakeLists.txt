# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_physics STATIC
    PhysicsWorld.cpp
    PhysicsWorld.h
)

target_link_libraries(vox_physics PUBLIC vox_includes fmt::fmt)
vox_configure_target(vox_physics)

# --- NVIDIA PhysX 5 (gated, default OFF) ------------------------------------
# When VOX_ENABLE_PHYSX is OFF, vox_physics is the logging stub above and pulls
# in no PhysX dependency -- the default engine build. When ON, link the static
# PhysX SDK built out-of-tree from thirdparty/physx (see thirdparty/README.md
# for the pin + exact build commands) and define VOX_HAVE_PHYSX so
# PhysicsWorld.cpp compiles its real CPU rigid-body implementation.
if(VOX_ENABLE_PHYSX)
    # Root of the cloned/pinned PhysX SDK (the inner "physx" dir that holds
    # include/, source/, bin/). Overridable for a different checkout location.
    set(VOX_PHYSX_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/physx/physx"
        CACHE PATH "Root of the PhysX 5 SDK (the inner physx/ dir).")

    # Which prebuilt PhysX config + platform-bin dir to link against. The PASS 1
    # build produced static libs (dynamic debug CRT, /MDd) here:
    #   <root>/bin/win.x86_64.vc143.md/debug
    # matching the engine's windows-debug preset. Override these to point at a
    # checked/profile/release build or a different toolset bin dir.
    set(VOX_PHYSX_PLATFORM_BIN "win.x86_64.vc143.md"
        CACHE STRING "PhysX bin/<platform> subdir (compiler + CRT flavour).")
    set(VOX_PHYSX_CONFIG "debug"
        CACHE STRING "PhysX build config to link: debug|checked|profile|release.")

    set(_vox_physx_inc "${VOX_PHYSX_ROOT}/include")
    set(_vox_physx_libdir "${VOX_PHYSX_ROOT}/bin/${VOX_PHYSX_PLATFORM_BIN}/${VOX_PHYSX_CONFIG}")

    if(NOT EXISTS "${_vox_physx_inc}/PxPhysicsAPI.h")
        message(FATAL_ERROR
            "VOX_ENABLE_PHYSX=ON but PhysX headers not found at ${_vox_physx_inc}. "
            "Clone + build PhysX first -- see thirdparty/README.md.")
    endif()
    if(NOT EXISTS "${_vox_physx_libdir}")
        message(FATAL_ERROR
            "VOX_ENABLE_PHYSX=ON but PhysX libs not found at ${_vox_physx_libdir}. "
            "Build the '${VOX_PHYSX_CONFIG}' config -- see thirdparty/README.md.")
    endif()

    # Static-lib build: file stems carry a _static_64 suffix on win.x86_64.
    # Link order follows PhysX's own SnippetTemplate.cmake (consumers first,
    # PhysXFoundation last) so MSVC resolves the static archives cleanly.
    set(_vox_physx_libs
        PhysXExtensions
        PhysXPvdSDK
        PhysX
        PhysXVehicle
        PhysXVehicle2
        PhysXCharacterKinematic
        PhysXCooking
        PhysXCommon
        PhysXFoundation)

    set(_vox_physx_resolved "")
    foreach(_lib IN LISTS _vox_physx_libs)
        find_library(VOX_PHYSX_${_lib}_LIB
            NAMES ${_lib}_static_64 ${_lib}_64 ${_lib}
            PATHS "${_vox_physx_libdir}"
            NO_DEFAULT_PATH)
        if(NOT VOX_PHYSX_${_lib}_LIB)
            message(FATAL_ERROR
                "VOX_ENABLE_PHYSX=ON but PhysX lib '${_lib}' not found in "
                "${_vox_physx_libdir}. See thirdparty/README.md.")
        endif()
        list(APPEND _vox_physx_resolved "${VOX_PHYSX_${_lib}_LIB}")
    endforeach()

    target_include_directories(vox_physics PUBLIC "${_vox_physx_inc}")
    target_link_libraries(vox_physics PUBLIC ${_vox_physx_resolved})
    target_compile_definitions(vox_physics PUBLIC VOX_HAVE_PHYSX=1 PX_PHYSX_STATIC_LIB)

    message(STATUS "  PhysX             : ON (${VOX_PHYSX_CONFIG} @ ${_vox_physx_libdir})")
else()
    message(STATUS "  PhysX             : OFF (stub)")
endif()
