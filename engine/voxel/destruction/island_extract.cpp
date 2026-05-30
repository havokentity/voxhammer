// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/destruction/island_extract.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace vox::destruction {

namespace {

inline std::int32_t ParentIdx(int x, int y, int z, glm::ivec3 dims) {
    return z * dims.x * dims.y + y * dims.x + x;
}

}  // namespace

LocalGrid extractIsland(const std::uint8_t* grid,
                        const ComponentField& cf,
                        glm::ivec3 parentDims,
                        std::uint16_t componentId) {
    LocalGrid lg;
    if (!grid || componentId == 0) return lg;
    if (parentDims.x <= 0 || parentDims.y <= 0 || parentDims.z <= 0) return lg;
    const std::int32_t total = parentDims.x * parentDims.y * parentDims.z;
    if (cf.ids.size() != static_cast<std::size_t>(total)) return lg;

    // Pass 1: scan for the AABB of `componentId`. We need a tight bounding box
    // so the new VoxelObject's local grid is no larger than the component
    // actually requires.
    glm::ivec3 mn(std::numeric_limits<int>::max());
    glm::ivec3 mx(std::numeric_limits<int>::min());
    bool found = false;
    for (int z = 0; z < parentDims.z; ++z) {
        for (int y = 0; y < parentDims.y; ++y) {
            for (int x = 0; x < parentDims.x; ++x) {
                if (cf.ids[ParentIdx(x, y, z, parentDims)] != componentId) continue;
                mn.x = std::min(mn.x, x); mn.y = std::min(mn.y, y); mn.z = std::min(mn.z, z);
                mx.x = std::max(mx.x, x); mx.y = std::max(mx.y, y); mx.z = std::max(mx.z, z);
                found = true;
            }
        }
    }
    if (!found) return lg;

    const glm::ivec3 ext = mx - mn + glm::ivec3(1);
    lg.dims = ext;
    lg.originInParent = mn;
    lg.data.assign(static_cast<std::size_t>(ext.x) * ext.y * ext.z, 0u);

    // Pass 2: copy participating voxels' material ids into the local grid.
    // Non-component voxels inside the AABB stay 0 -- this is exactly the
    // "holes for free" property: an empty cell in the parent (or a cell that
    // belongs to a different island) appears as empty in the new VoxelObject,
    // so its ray-march sees through and its box decomposition tiles around.
    for (int z = mn.z; z <= mx.z; ++z) {
        for (int y = mn.y; y <= mx.y; ++y) {
            for (int x = mn.x; x <= mx.x; ++x) {
                const std::int32_t pIdx = ParentIdx(x, y, z, parentDims);
                if (cf.ids[pIdx] != componentId) continue;
                const int lx = x - mn.x;
                const int ly = y - mn.y;
                const int lz = z - mn.z;
                const std::size_t lIdx =
                    static_cast<std::size_t>(lz) * ext.x * ext.y +
                    static_cast<std::size_t>(ly) * ext.x +
                    static_cast<std::size_t>(lx);
                lg.data[lIdx] = grid[pIdx];
            }
        }
    }

    return lg;
}

}  // namespace vox::destruction
