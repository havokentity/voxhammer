# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte

# Milestone B prototype: CPU-only destruction algorithms (own static lib so the
# renderer's A1/A2/A3 stream cannot cause merge conflicts here). Added before
# vox_voxel so the StructuralSettle unit (below) can link it.
add_subdirectory(destruction)

add_library(vox_voxel STATIC
    VoxelWorld.h
    VoxelWorld.cpp
    VoxImport.h
    VoxImport.cpp
    StructuralSettle.h       # Milestone B1: connectivity + stability -> live carve
    StructuralSettle.cpp
)

# vox_destruction (PUBLIC) so StructuralSettle's deps flow to the game/tools.
target_link_libraries(vox_voxel PUBLIC vox_includes fmt::fmt vox_destruction)
vox_configure_target(vox_voxel)
