# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_script STATIC
    ScriptHost.cpp
    ScriptHost.h
)

target_link_libraries(vox_script PUBLIC vox_includes fmt::fmt)
vox_configure_target(vox_script)

if(VOX_ENABLE_SCRIPT)
    find_path(LUAJIT_INCLUDE_DIR luajit/lua.h)
    find_library(LUAJIT_LIBRARY NAMES lua51)
    if(LUAJIT_INCLUDE_DIR AND LUAJIT_LIBRARY)
        target_include_directories(vox_script PUBLIC ${LUAJIT_INCLUDE_DIR})
        target_link_libraries(vox_script PUBLIC ${LUAJIT_LIBRARY})
        target_compile_definitions(vox_script PUBLIC VOX_ENABLE_SCRIPT=1)
        message(STATUS "  Script / LuaJIT   : ON (${LUAJIT_LIBRARY})")
    else()
        message(FATAL_ERROR "VOX_ENABLE_SCRIPT=ON but LuaJIT not found. "
            "Pass -DVCPKG_MANIFEST_FEATURES=\"...;script\" to install it via vcpkg.")
    endif()
else()
    message(STATUS "  Script / LuaJIT   : OFF (stub)")
endif()
