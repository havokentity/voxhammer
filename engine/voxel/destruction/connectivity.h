// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// ---------------------------------------------------------------------------
// connectivity.h - face-6 connected component labelling for the per-object
// voxel grid, with anchor-priority growth (Milestone B, §4).
//
// `labelComponents()` walks a flat `dims.x * dims.y * dims.z` material grid
// (0 = empty, non-zero = solid) and produces a parallel `ComponentField` where
// every solid voxel carries a 16-bit component id:
//
//   id == 0          : empty voxel.
//   1 .. anchoredCount         : ANCHORED components, multi-source BFS from
//                                anchor voxels (anchorMask != 0 AND grid != 0).
//   anchoredCount+1 .. N       : UNANCHORED islands, grown from any remaining
//                                solid voxel not reached by the anchor flood.
//
// Both phases use a SINGLE flat queue (std::vector<int32_t>) and mark "visited"
// by writing the output id; no separate visited bitmap. O(N) work, where N is
// the voxel count (face-6 stencil => 6 neighbour probes per voxel).
//
// This is the multi-source flood-fill the spec calls "BFS from ANCHORS" - the
// algorithmic substrate for stability propagation (stability.h) and detachment
// detection (an unanchored component is, by construction, a falling island).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace vox::destruction {

// Per-voxel component labelling produced by labelComponents().
// `ids.size() == dims.x * dims.y * dims.z`; id == 0 means empty voxel.
// `anchoredCount` is the number of components in [1, anchoredCount] that are
// reachable (face-6) from at least one anchor voxel; ids beyond that are the
// unanchored "loose" islands which would, in the full pipeline, detach into
// new VoxelObjects with PhysX dynamic bodies.
struct ComponentField {
    std::vector<std::uint16_t> ids;
    int anchoredCount = 0;       // number of anchored components (1..anchoredCount)
    int totalCount    = 0;       // total components (anchored + unanchored), == max id
};

// Label face-6 connected components in `grid` (size dims.x*dims.y*dims.z,
// row-major idx = z*dims.x*dims.y + y*dims.x + x; 0 = empty).
//
// `anchorMask` (same size + layout) marks anchor seeds: a voxel participates
// as an anchor iff anchorMask[idx] != 0 AND grid[idx] != 0. Pass nullptr to
// skip the anchor phase (every component is then "unanchored").
//
// Returns a fully-populated ComponentField. Determinism: the seed order for
// the anchor BFS is the natural row-major scan, and ids are assigned in the
// order seeds are encountered, so two identical inputs always produce the
// same output.
ComponentField labelComponents(const std::uint8_t* grid,
                               glm::ivec3 dims,
                               const std::uint8_t* anchorMask);

}  // namespace vox::destruction
