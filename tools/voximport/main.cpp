// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
// voximport -- Load a MagicaVoxel .vox file and print diagnostics.
#include "voxel/VoxImport.h"

#include <fmt/core.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fmt::print("Usage: voximport <file.vox>\n");
        return 1;
    }

    const std::string path = argv[1];
    vox::voxel::VoxScene scene;
    if (!vox::voxel::LoadVox(path, scene)) {
        fmt::print("voximport: failed to load '{}'\n", path);
        return 1;
    }

    fmt::print("File       : {}\n", path);
    fmt::print("Dimensions : {}x{}x{}\n", scene.sizeX, scene.sizeY, scene.sizeZ);
    fmt::print("Voxels     : {}\n", scene.voxelCount);
    fmt::print("Chunks     : {}\n", scene.world.ResidentChunks());
    return 0;
}
