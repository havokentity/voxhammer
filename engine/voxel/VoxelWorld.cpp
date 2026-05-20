// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/VoxelWorld.h"

#include "platform/Log.h"

#include <algorithm>
#include <climits>

namespace vox::voxel {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int FloorDiv(int a, int b) noexcept {
    // Integer floor-division that rounds toward -inf (not toward zero).
    return a / b - (a % b != 0 && (a ^ b) < 0);
}

// static
ChunkCoord VoxelWorld::WorldToChunk(int x, int y, int z) noexcept {
    return {
        FloorDiv(x, static_cast<int>(kChunkSize)),
        FloorDiv(y, static_cast<int>(kChunkSize)),
        FloorDiv(z, static_cast<int>(kChunkSize))
    };
}

// static
void VoxelWorld::ChunkLocal(int x, int y, int z,
                             unsigned& lx, unsigned& ly, unsigned& lz) noexcept {
    auto mod = [](int v, int m) -> unsigned {
        int r = v % m;
        return static_cast<unsigned>(r < 0 ? r + m : r);
    };
    lx = mod(x, static_cast<int>(kChunkSize));
    ly = mod(y, static_cast<int>(kChunkSize));
    lz = mod(z, static_cast<int>(kChunkSize));
}

Chunk& VoxelWorld::GetOrCreateChunk(const ChunkCoord& cc) {
    return chunks_[cc];
}

const Chunk* VoxelWorld::FindChunk(const ChunkCoord& cc) const noexcept {
    auto it = chunks_.find(cc);
    return it != chunks_.end() ? &it->second : nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void VoxelWorld::Init() {
    vox::log::Info("voxel: VoxelWorld init (sparse chunk map, chunk={}^3)", kChunkSize);
}

void VoxelWorld::Shutdown() {
    Clear();
    vox::log::Trace("voxel: VoxelWorld shutdown");
}

void VoxelWorld::Clear() {
    chunks_.clear();
}

void VoxelWorld::SetVoxel(int x, int y, int z, std::uint8_t mat) {
    ChunkCoord cc = WorldToChunk(x, y, z);
    if (mat == 0) {
        // Setting to empty: only touch a chunk if it already exists.
        auto it = chunks_.find(cc);
        if (it != chunks_.end()) {
            unsigned lx, ly, lz;
            ChunkLocal(x, y, z, lx, ly, lz);
            it->second.Set(lx, ly, lz, 0);
            if (it->second.Empty()) {
                chunks_.erase(it);
            }
        }
    } else {
        unsigned lx, ly, lz;
        ChunkLocal(x, y, z, lx, ly, lz);
        GetOrCreateChunk(cc).Set(lx, ly, lz, mat);
    }
}

std::uint8_t VoxelWorld::GetVoxel(int x, int y, int z) const noexcept {
    const Chunk* c = FindChunk(WorldToChunk(x, y, z));
    if (!c) return 0;
    unsigned lx, ly, lz;
    ChunkLocal(x, y, z, lx, ly, lz);
    return c->Get(lx, ly, lz);
}

void VoxelWorld::Bounds(int& xmin, int& ymin, int& zmin,
                         int& xmax, int& ymax, int& zmax) const noexcept {
    xmin = ymin = zmin = INT_MAX;
    xmax = ymax = zmax = INT_MIN;
    for (const auto& [cc, chunk] : chunks_) {
        int bxlo = cc.x * static_cast<int>(kChunkSize);
        int bylo = cc.y * static_cast<int>(kChunkSize);
        int bzlo = cc.z * static_cast<int>(kChunkSize);
        int bxhi = bxlo + static_cast<int>(kChunkSize) - 1;
        int byhi = bylo + static_cast<int>(kChunkSize) - 1;
        int bzhi = bzlo + static_cast<int>(kChunkSize) - 1;
        xmin = std::min(xmin, bxlo); ymin = std::min(ymin, bylo); zmin = std::min(zmin, bzlo);
        xmax = std::max(xmax, bxhi); ymax = std::max(ymax, byhi); zmax = std::max(zmax, bzhi);
    }
    if (xmin == INT_MAX) { xmin = ymin = zmin = xmax = ymax = zmax = 0; }
    else { ++xmax; ++ymax; ++zmax; }  // make exclusive
}

std::vector<std::uint32_t> VoxelWorld::BakeFlatGrid(unsigned dim) const {
    // Encoding matches Renderer.cpp GenerateScene() and HLSL PSMain:
    //   index = z * dim * dim + y * dim + x
    //   value = material id cast to uint32_t (0 = air/empty)
    const std::size_t total = static_cast<std::size_t>(dim) * dim * dim;
    std::vector<std::uint32_t> grid(total, 0u);

    for (const auto& [cc, chunk] : chunks_) {
        int bx = cc.x * static_cast<int>(kChunkSize);
        int by = cc.y * static_cast<int>(kChunkSize);
        int bz = cc.z * static_cast<int>(kChunkSize);

        for (unsigned lz = 0; lz < kChunkSize; ++lz) {
            int wz = bz + static_cast<int>(lz);
            if (wz < 0 || static_cast<unsigned>(wz) >= dim) continue;
            for (unsigned ly = 0; ly < kChunkSize; ++ly) {
                int wy = by + static_cast<int>(ly);
                if (wy < 0 || static_cast<unsigned>(wy) >= dim) continue;
                for (unsigned lx = 0; lx < kChunkSize; ++lx) {
                    int wx = bx + static_cast<int>(lx);
                    if (wx < 0 || static_cast<unsigned>(wx) >= dim) continue;
                    std::uint8_t m = chunk.Get(lx, ly, lz);
                    if (m == 0) continue;
                    const std::size_t idx = static_cast<std::size_t>(wz) * dim * dim
                                          + static_cast<std::size_t>(wy) * dim
                                          + static_cast<std::size_t>(wx);
                    grid[idx] = static_cast<std::uint32_t>(m);
                }
            }
        }
    }

    vox::log::Trace("voxel: BakeFlatGrid dim={} cells={} chunks={}", dim, total, chunks_.size());
    return grid;
}

}  // namespace vox::voxel
