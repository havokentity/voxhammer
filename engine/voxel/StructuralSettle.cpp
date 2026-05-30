// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/StructuralSettle.h"

#include <cstdint>
#include <vector>

#include "platform/Log.h"
#include "voxel/destruction/connectivity.h"
#include "voxel/destruction/island_extract.h"

namespace vox::voxel {

namespace {

// Row-major flat index, matching BakeFlatGrid / the renderer / B0 layout:
//   idx = z*dim*dim + y*dim + x.
inline std::size_t FlatIdx(int x, int y, int z, int dim) {
    return static_cast<std::size_t>(z) * dim * dim
         + static_cast<std::size_t>(y) * dim
         + static_cast<std::size_t>(x);
}

}  // namespace

SettleResult SettleWorld(VoxelWorld& world, unsigned dim, int anchorLayers) {
    SettleResult result;
    if (dim == 0) return result;
    const int D = static_cast<int>(dim);
    if (anchorLayers < 0) anchorLayers = 0;

    // 1) Bake the live world to a flat material grid (uint32, 0 = empty). The
    //    B0 connectivity API wants a uint8 solid/material grid, so downcast:
    //    every world material id is 1..255, which fits in a uint8 exactly.
    const std::vector<std::uint32_t> baked = world.BakeFlatGrid(dim);
    const std::size_t total = static_cast<std::size_t>(D) * D * D;
    if (baked.size() != total) {
        vox::log::Warn("settle: BakeFlatGrid size mismatch ({} != {})", baked.size(), total);
        return result;
    }

    std::vector<std::uint8_t> grid(total, 0u);
    for (std::size_t i = 0; i < total; ++i) {
        grid[i] = static_cast<std::uint8_t>(baked[i] & 0xFFu);
    }

    // 2) Anchor mask: the bottom `anchorLayers` rows (y < anchorLayers) are the
    //    GROUND the structure stands on. A solid voxel down there anchors its
    //    whole face-6-connected component. Simple, documented heuristic -- a
    //    future pass could anchor against arbitrary static colliders instead.
    std::vector<std::uint8_t> anchorMask(total, 0u);
    const int anchorY = (anchorLayers > D) ? D : anchorLayers;
    for (int z = 0; z < D; ++z)
        for (int y = 0; y < anchorY; ++y)
            for (int x = 0; x < D; ++x)
                anchorMask[FlatIdx(x, y, z, D)] = 1u;

    // 3) Label connected components with anchor-priority growth. Ids
    //    [1, anchoredCount] are reachable from the ground; ids beyond that are
    //    UNANCHORED islands -- by construction, things that float.
    const glm::ivec3 dims(D, D, D);
    const vox::destruction::ComponentField cf =
        vox::destruction::labelComponents(grid.data(), dims, anchorMask.data());

    if (cf.totalCount <= cf.anchoredCount) {
        // Nothing floats: every component touches the ground. World unchanged.
        return result;
    }

    // 4) For each UNANCHORED island: extract its tight local grid, clear those
    //    voxels from the live world, and record it for the caller to spawn as
    //    falling debris.
    for (int id = cf.anchoredCount + 1; id <= cf.totalCount; ++id) {
        const auto componentId = static_cast<std::uint16_t>(id);
        vox::destruction::LocalGrid lg =
            vox::destruction::extractIsland(grid.data(), cf, dims, componentId);
        if (lg.dims.x <= 0 || lg.dims.y <= 0 || lg.dims.z <= 0 || lg.data.empty()) {
            continue;  // empty component (shouldn't happen for a valid id)
        }

        DetachedIsland island;
        island.dims = lg.dims;
        island.origin = lg.originInParent;
        island.data = std::move(lg.data);

        // Clear the island's solid voxels from the live VoxelWorld (SetVoxel 0
        // auto-evicts now-empty chunks). Count them so the caller knows whether
        // a re-bake / GPU re-upload is needed.
        int islandVox = 0;
        for (int lz = 0; lz < island.dims.z; ++lz)
            for (int ly = 0; ly < island.dims.y; ++ly)
                for (int lx = 0; lx < island.dims.x; ++lx) {
                    const std::size_t li =
                        static_cast<std::size_t>(lz) * island.dims.x * island.dims.y
                        + static_cast<std::size_t>(ly) * island.dims.x
                        + static_cast<std::size_t>(lx);
                    if (island.data[li] == 0u) continue;  // not part of this island
                    const int wx = island.origin.x + lx;
                    const int wy = island.origin.y + ly;
                    const int wz = island.origin.z + lz;
                    world.SetVoxel(wx, wy, wz, 0u);
                    ++islandVox;
                }

        if (islandVox == 0) continue;  // nothing actually removed -> skip
        island.voxelCount = islandVox;
        result.detachedVoxels += islandVox;
        result.islands.push_back(std::move(island));
    }

    if (result.detachedVoxels > 0) {
        vox::log::Info("settle: detached {} island(s), {} voxels (anchor y<{})",
                       result.islands.size(), result.detachedVoxels, anchorY);
    }
    return result;
}

}  // namespace vox::voxel
