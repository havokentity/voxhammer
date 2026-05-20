// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// MagicaVoxel .vox importer (RIFF-like format, versions 150 and 200).
// Pure C++20 / STL – no extra dependencies.
//
// Usage (file path):
//   vox::voxel::VoxScene scene;
//   if (vox::voxel::LoadVox("my.vox", scene)) { /* use scene.world */ }
//
// Usage (in-memory buffer, useful for tests):
//   if (vox::voxel::LoadVoxFromMemory(ptr, len, scene)) { ... }

#include "voxel/VoxelWorld.h"

#include <array>
#include <cstdint>
#include <string>

namespace vox::voxel {

// Default MagicaVoxel palette (index 0 unused; indices 1-255 are colours).
// Each entry is RGBA (R at byte 0, G at 1, B at 2, A at 3).
using VoxPalette = std::array<std::uint32_t, 256>;

struct VoxScene {
    VoxelWorld world;         // populated voxels (material = palette index 1-255)
    VoxPalette palette{};     // 256-entry RGBA palette; entry 0 unused
    unsigned   sizeX = 0;
    unsigned   sizeY = 0;
    unsigned   sizeZ = 0;
    unsigned   voxelCount = 0;
};

// Load a .vox file from disk. Returns true on success.
bool LoadVox(const std::string& path, VoxScene& out);

// Load from a byte buffer already in memory (no filesystem access).
bool LoadVoxFromMemory(const std::uint8_t* data, std::size_t len, VoxScene& out);

}  // namespace vox::voxel
