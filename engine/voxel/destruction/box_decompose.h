// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// ---------------------------------------------------------------------------
// box_decompose.h - greedy AABB decomposition of one connected component
// (Milestone B, §4 - "Collision shape = a greedy BOX DECOMPOSITION of the
// object's solid voxels").
//
// The spec's PhysX collider is a compound of `PxBoxGeometry`s, *not* a convex
// hull and *not* per-voxel - because:
//
//   * a convex hull would seal up the spec's "holes for free" (a hole in a
//     wall must stay a hole, not get shrink-wrapped over),
//   * per-voxel would explode actor counts for big anchored objects.
//
// So we build a union of axis-aligned boxes that tiles ONLY the solid voxels
// of one connected component, preserving its concave shape exactly.
//
// `decomposeComponent()` guarantees:
//   (a) the union of the returned boxes is exactly the component's voxel set
//       (no empty voxel is covered, no solid voxel of the component is
//       missed) -- concave-respecting,
//   (b) no two boxes overlap.
//
// Algorithm: pick the lowest-row-major uncovered solid voxel; grow a slab in
// +X while every new row is fully solid-of-this-component AND uncovered, then
// +Y, then +Z (and the analogous - directions). Mark all covered voxels;
// repeat. Greedy, not optimal in box count, but cheap, deterministic and
// concave-respecting. The renderer side of the spec doesn't care - physics
// just needs a valid concave cover.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "voxel/destruction/connectivity.h"

namespace vox::destruction {

// Inclusive integer AABB in voxel coordinates: covers voxels [mn..mx] on each
// axis (so a single-voxel box has mn == mx). Volume = (mx-mn+1) component-wise
// product.
struct Box {
    glm::ivec3 mn{0};
    glm::ivec3 mx{0};

    // Convenience accessors -- inclusive volume / containment.
    std::int64_t volume() const noexcept {
        const glm::ivec3 e = mx - mn + glm::ivec3(1);
        return static_cast<std::int64_t>(e.x) * e.y * e.z;
    }
    bool contains(int x, int y, int z) const noexcept {
        return x >= mn.x && x <= mx.x && y >= mn.y && y <= mx.y && z >= mn.z && z <= mx.z;
    }
};

// Greedy box decomposition of `componentId`'s solid voxels inside `cf`.
//
// Returns a list of non-overlapping inclusive AABBs whose union equals exactly
// the set of voxels with cf.ids[idx] == componentId. Order matches the greedy
// scan (row-major seed order).
//
// Passing a component id that does not exist returns an empty vector.
std::vector<Box> decomposeComponent(const ComponentField& cf,
                                    glm::ivec3 dims,
                                    std::uint16_t componentId);

}  // namespace vox::destruction
