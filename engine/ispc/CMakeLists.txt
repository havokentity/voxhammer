# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_ispc STATIC
    IspcKernels.cpp
    IspcKernels.h
)

target_link_libraries(vox_ispc PUBLIC vox_includes)
vox_configure_target(vox_ispc)

# Compile the .ispc kernels only when ISPC is enabled (cmake/VoxIspc.cmake).
# Otherwise IspcKernels.cpp uses its scalar reference path.
if(VOX_ENABLE_ISPC)
    target_compile_definitions(vox_ispc PUBLIC VOX_ENABLE_ISPC=1)
    vox_target_ispc(vox_ispc SOURCES saxpy.ispc)
endif()
