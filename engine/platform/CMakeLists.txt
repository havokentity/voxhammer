# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_platform STATIC
    Platform.cpp
    Platform.h
    Log.h
    Keybindings.cpp
    Keybindings.h
    Window.cpp
    Window.h
)

target_link_libraries(vox_platform
    PUBLIC
        vox_includes
        fmt::fmt
    PRIVATE
        tomlplusplus::tomlplusplus
)

vox_configure_target(vox_platform)

if(VOX_ENABLE_WINDOW)
    find_package(glfw3 CONFIG REQUIRED)
    target_link_libraries(vox_platform PRIVATE glfw)
    target_compile_definitions(vox_platform PRIVATE VOX_HAVE_WINDOW=1)
endif()
