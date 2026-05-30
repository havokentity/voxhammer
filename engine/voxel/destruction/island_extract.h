// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// ---------------------------------------------------------------------------
// island_extract.h - copy one connected component out of its parent grid into
// a fresh tight local grid (Milestone B, §4 - "Island extraction -> own grid
// (non-AABB-slice)").
//
// When destruction severs a chunk of an object, the spec says each detached
// island becomes its OWN new VoxelObject, with its own tight grid sized to the
// component's bounding box - not a sub-slice of the parent. Concavity comes
// for free because every voxel in the LocalGrid that is *not* part of the
// component stays empty (0), even if the parent had material there.
//
// `extractIsland()` allocates a grid of size component_aabb_extent, copies
// only voxels whose `cf.ids[idx] == componentId` (preserving their original
// material id from `grid`), and records `originInParent = aabb_min` so the
// caller can derive a world transform (parent_transform * translate(origin)).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "voxel/destruction/connectivity.h"

namespace vox::destruction {

// Tight local grid carved out for a single island. `data` is `dims.x*dims.y*dims.z`
// material ids in the same row-major layout as the parent. `originInParent`
// is the integer voxel offset of this grid's (0,0,0) corner inside the parent.
//
// If the requested component has zero voxels, `dims == (0,0,0)`, `data` is
// empty and `originInParent == (0,0,0)`.
struct LocalGrid {
    std::vector<std::uint8_t> data;
    glm::ivec3 dims{0};
    glm::ivec3 originInParent{0};
};

// Extract the voxels of `componentId` from `grid` (parent, dims `parentDims`)
// into a fresh tight LocalGrid. Voxels not part of the component (other
// components OR empty space) stay 0 in the LocalGrid, preserving the
// component's concave shape.
//
// Material ids are copied verbatim from `grid` for participating voxels.
LocalGrid extractIsland(const std::uint8_t* grid,
                        const ComponentField& cf,
                        glm::ivec3 parentDims,
                        std::uint16_t componentId);

}  // namespace vox::destruction
