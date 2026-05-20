# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_script STATIC
    ScriptHost.cpp
    ScriptHost.h
)

target_link_libraries(vox_script PUBLIC vox_includes fmt::fmt)
vox_configure_target(vox_script)

if(VOX_BUILD_WITH_LUAJIT)
    find_package(luajit CONFIG QUIET)
    if(luajit_FOUND)
        target_link_libraries(vox_script PUBLIC luajit::luajit)
        target_compile_definitions(vox_script PUBLIC VOX_HAVE_LUAJIT=1)
    endif()
endif()
