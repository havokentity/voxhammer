// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

namespace vox::voxel {

// Sparse 32^3 chunk world (chunk -> 8^3 brick hierarchy), 0.1 m voxels,
// 8-bit material + 8-bit palette + 4-bit state. DirectStorage + GDeflate
// streaming, .vox import, and ISPC destruction land in M1/M2. Stub this pass.
class VoxelWorld {
public:
    void Init();
    void Shutdown();
    unsigned ResidentChunks() const { return 0; }
};

}  // namespace vox::voxel
