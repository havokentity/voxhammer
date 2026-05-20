# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_render STATIC
    Renderer.cpp
    Renderer.h
)

target_link_libraries(vox_render PUBLIC vox_includes fmt::fmt glm::glm)
vox_configure_target(vox_render)

# Minimal DX12 presenter targets the GLFW window's HWND, so it is gated on the
# window feature. Full Agility/DirectStorage/DXR/DLSS land in M1/M4.
if(VOX_ENABLE_WINDOW)
    target_compile_definitions(vox_render PRIVATE VOX_HAVE_DX12=1)
    target_link_libraries(vox_render PRIVATE d3d12 dxgi dxguid d3dcompiler)
endif()
