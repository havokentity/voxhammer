// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Forward-declare entt::entity so callers can name the type without pulling in
// the full EnTT headers when VOX_ENABLE_ECS is off.
#if VOX_ENABLE_ECS
#include <entt/entt.hpp>
#endif

namespace vox::ecs {

// ---------------------------------------------------------------------------
// EcsWorld
//
// Holds the EnTT registry and runs the two canonical systems each Step():
//   - KinematicSystem  : pos += vel * dt
//   - LifetimeSystem   : life -= dt, destroy entities whose life <= 0
//
// When VOX_ENABLE_ECS is 0 a minimal stub satisfies the API and returns
// safe no-op values so callers do not need conditional compilation.
// ---------------------------------------------------------------------------

class EcsWorld {
public:
    void Init();
    void Shutdown();

    // Spawn a debris fragment at the given position with the given velocity.
    // Returns entt::null (or a cast-0 placeholder) when ECS is disabled.
#if VOX_ENABLE_ECS
    entt::entity SpawnDebris(glm::vec3 pos, glm::vec3 vel, float mass, float life);
#else
    uint32_t     SpawnDebris(glm::vec3 pos, glm::vec3 vel, float mass, float life);
#endif

    // Advance all systems by dt seconds.
    void Step(float dt);

    // Number of live entities currently in the registry (0 when ECS is off).
    unsigned EntityCount() const;

#if VOX_ENABLE_ECS
protected:
    entt::registry m_registry;
#endif
};

}  // namespace vox::ecs
