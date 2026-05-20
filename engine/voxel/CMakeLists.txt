# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_voxel STATIC
    VoxelWorld.h
    VoxelWorld.cpp
    VoxImport.h
    VoxImport.cpp
)

target_link_libraries(vox_voxel PUBLIC vox_includes fmt::fmt)
vox_configure_target(vox_voxel)
