// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// ---------------------------------------------------------------------------
// StructuralSettle.h - Milestone B1: wire the B0 connectivity + integer
// stability algorithms (engine/voxel/destruction/*) into the LIVE carve so the
// world is structurally aware -- "nothing floats".
//
// After a carve, any solid voxel that no longer has a face-6 path to the
// GROUND (an anchor) is, by construction, a falling island. `SettleWorld()`:
//
//   1. Bakes the live VoxelWorld to a flat material grid (BakeFlatGrid).
//   2. Builds an ANCHOR MASK: voxels in the bottom `anchorLayers` rows
//      (y < anchorLayers) are anchors -- the ground the structure stands on.
//   3. Runs vox::destruction::labelComponents() to label connected components,
//      partitioning solids into ANCHORED (reachable from the ground) and
//      UNANCHORED islands.
//   4. For every UNANCHORED island it extractIsland()s a tight local grid,
//      CLEARS those voxels from the live VoxelWorld, and records the island
//      in the returned SettleResult so the caller can spawn it as falling
//      debris and re-upload the world to the renderer.
//
// This routine is deliberately physics/render-AGNOSTIC: it only reads + mutates
// the VoxelWorld and returns plain data. game/main.cpp owns the debris spawn
// (DebrisField) + renderer re-upload, so this stays a clean engine/voxel unit
// the in-flight renderer stream cannot conflict with.
//
// PERFORMANCE: this is a FULL-GRID flood (O(kWorldDim^3)) every call. At 128^3
// (~2M cells) that is fine for a manual command and an opt-in per-carve hook.
// The future optimization (noted in the spec, Milestone B) is an amortized
// DIRTY-REGION re-flood that only re-labels the voxels a carve actually
// touched. Not implemented here on purpose -- correctness first, don't
// over-engineer the prototype.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "voxel/VoxelWorld.h"

namespace vox::voxel {

// One island that detached during a settle: a tight local material grid plus
// the integer voxel offset of its (0,0,0) corner inside the world grid. The
// caller maps this to a falling rigid body (origin + local dims => spawn box).
//
// `data` is `dims.x*dims.y*dims.z` material ids, row-major
// idx = z*dims.x*dims.y + y*dims.x + x (matches the parent / renderer layout);
// 0 = empty so the island's concave shape ("holes for free") is preserved.
struct DetachedIsland {
    std::vector<std::uint8_t> data;
    glm::ivec3 dims{0};
    glm::ivec3 origin{0};      // voxel offset of data's (0,0,0) corner in the world grid
    int        voxelCount = 0; // solid voxels copied (>0)
};

// Result of one SettleWorld() pass.
struct SettleResult {
    std::vector<DetachedIsland> islands;  // unanchored islands detached this pass
    int detachedVoxels = 0;               // total solid voxels removed from the world
};

// Detach + remove every voxel not connected to the ground from `world`.
//
//   dim          : grid edge used for BakeFlatGrid (pass kWorldDim).
//   anchorLayers : voxels with y < anchorLayers are anchors (the ground).
//                  Default 1 = only the y==0 floor layer anchors the world.
//
// On return: every detached island's voxels have been CLEARED from `world`
// (SetVoxel(..,0)), and the islands are returned for the caller to spawn as
// debris + re-bake to the renderer. If nothing detaches, `islands` is empty
// and `world` is unchanged. The caller must re-upload the world to the GPU
// (BakeFlatGrid + SetVoxels) when result.detachedVoxels > 0.
SettleResult SettleWorld(VoxelWorld& world, unsigned dim, int anchorLayers = 1);

}  // namespace vox::voxel
