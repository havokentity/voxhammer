// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// ---------------------------------------------------------------------------
// stability.h - integer stability propagation, per the spec's "BFS from
// anchors + integer stability" line (Milestone B, §4):
//
//   "an integer stability value propagates from anchors (cheap, capped -- not
//    a force solve)."
//
// Multi-source BFS over the anchored components of a ComponentField. Every
// anchor seed starts at `anchorStrength`; each face-6 step decrements by
// `falloffPerStep`, saturating at 0. Unanchored components get 0 everywhere
// (they are, by definition, falling islands - no support to propagate).
//
// This is intentionally NOT a stress solver. It is a flood that captures the
// "how far am I from a known anchor" signal cheaply, so the fracture-point
// selector (Milestone C, future) can pick weak spans without simulating
// forces. Per-voxel work is O(1); whole-grid work is O(N).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "voxel/destruction/connectivity.h"

namespace vox::destruction {

// Compute per-voxel integer stability from the anchored components in `cf`.
// `anchorMask` must be the SAME mask passed to labelComponents() so the seeds
// line up; pass nullptr and `out` is left as 0 (no anchors -> no stability).
//
//   anchorStrength  : value assigned at each anchor voxel.
//   falloffPerStep  : amount subtracted per face-6 step (saturating at 0).
//
// Output size == dims.x*dims.y*dims.z. Voxels in unanchored components and
// empty voxels both stay at 0.
std::vector<std::uint16_t> propagateStability(const ComponentField& cf,
                                              glm::ivec3 dims,
                                              const std::uint8_t* anchorMask,
                                              std::uint16_t anchorStrength = 0xFFFF,
                                              std::uint16_t falloffPerStep = 1);

}  // namespace vox::destruction
