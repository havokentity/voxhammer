// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// ---------------------------------------------------------------------------
// ECS component definitions for Voxhammer gameplay entities.
// These are plain-old-data aggregates; no virtual functions, no allocations.
// ---------------------------------------------------------------------------

namespace vox::ecs {

/// World-space transform.
struct Transform {
    glm::vec3 pos   = {0.f, 0.f, 0.f};
    glm::quat rot   = {1.f, 0.f, 0.f, 0.f};  // w, x, y, z (identity)
    glm::vec3 scale = {1.f, 1.f, 1.f};
};

/// Linear + angular velocity (world space).
struct Velocity {
    glm::vec3 linear  = {0.f, 0.f, 0.f};
    glm::vec3 angular = {0.f, 0.f, 0.f};
};

/// Debris fragment: physical mass and remaining lifetime (seconds).
struct DebrisBody {
    float mass = 1.f;   ///< kg
    float life = 5.f;   ///< seconds remaining before the entity is destroyed
};

/// Point / spot light attached to an entity.
struct LightSource {
    glm::vec3 color     = {1.f, 1.f, 1.f};
    float     intensity = 1.f;  ///< candela / arbitrary unit
};

/// Reference into the voxel mass-data subsystem (vox_voxel).
struct VoxelChunkRef {
    uint32_t chunk_id = 0;
};

}  // namespace vox::ecs
