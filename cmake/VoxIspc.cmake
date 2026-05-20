# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
# ISPC kernel integration. Gated by VOX_ENABLE_ISPC (OFF in the skeleton pass
# until an ispc binary is on PATH or pinned under thirdparty/ispc/). When OFF,
# vox_target_ispc() is a no-op and any C++ that consumes a kernel must itself
# be guarded behind the VOX_ENABLE_ISPC compile definition.

if(VOX_ENABLE_ISPC)
    find_program(VOX_ISPC_EXECUTABLE
        NAMES ispc
        HINTS ${CMAKE_SOURCE_DIR}/thirdparty/ispc/bin
        DOC "Intel ISPC compiler")
    if(NOT VOX_ISPC_EXECUTABLE)
        message(FATAL_ERROR
            "VOX_ENABLE_ISPC=ON but ispc not found. Put ispc on PATH or pin a "
            "binary under thirdparty/ispc/bin and reconfigure.")
    endif()
    message(STATUS "ISPC: ${VOX_ISPC_EXECUTABLE}")
    # AVX2 baseline for the floor GPU's host CPU; AVX-512 dispatch added later.
    set(VOX_ISPC_TARGETS "avx2-i32x8" CACHE STRING "ISPC --target ISA set")
endif()

# vox_target_ispc(<target> SOURCES a.ispc b.ispc ...)
# Compiles each .ispc to an object + a generated C/C++ header, attaches the
# objects to <target>, and exposes the header dir on <target>'s include path.
function(vox_target_ispc target)
    cmake_parse_arguments(ISPC "" "" "SOURCES" ${ARGN})
    if(NOT VOX_ENABLE_ISPC)
        return()  # no-op: kernels excluded from the skeleton build
    endif()

    set(gen_dir ${CMAKE_BINARY_DIR}/ispc_gen/${target})
    file(MAKE_DIRECTORY ${gen_dir})
    set(objs "")
    foreach(src ${ISPC_SOURCES})
        get_filename_component(stem ${src} NAME_WE)
        set(obj ${gen_dir}/${stem}.obj)
        set(hdr ${gen_dir}/${stem}_ispc.h)
        add_custom_command(
            OUTPUT  ${obj} ${hdr}
            COMMAND ${VOX_ISPC_EXECUTABLE}
                    -O2 --arch=x86-64 --target=${VOX_ISPC_TARGETS}
                    --opt=fast-math
                    ${CMAKE_CURRENT_SOURCE_DIR}/${src}
                    -o ${obj} -h ${hdr}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
            COMMENT "ISPC ${src}"
            VERBATIM)
        list(APPEND objs ${obj})
    endforeach()
    target_sources(${target} PRIVATE ${objs})
    target_include_directories(${target} PRIVATE ${gen_dir})
endfunction()
