// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "ecs/EcsWorld.h"

#include "ecs/components.h"
#include "platform/Log.h"

namespace vox::ecs {

// ---------------------------------------------------------------------------
// EcsWorld — real EnTT-backed implementation (VOX_ENABLE_ECS == 1)
// ---------------------------------------------------------------------------

#if VOX_ENABLE_ECS

void EcsWorld::Init() {
    vox::log::Info("ecs: world init (EnTT {})", ENTT_VERSION);
}

void EcsWorld::Shutdown() {
    vox::log::Info("ecs: world shutdown ({} entities remaining)", EntityCount());
    m_registry.clear();
}

entt::entity EcsWorld::SpawnDebris(glm::vec3 pos, glm::vec3 vel, float mass, float life) {
    entt::entity e = m_registry.create();
    m_registry.emplace<Transform>(e, pos, glm::quat{1.f, 0.f, 0.f, 0.f}, glm::vec3{1.f});
    m_registry.emplace<Velocity>(e, vel, glm::vec3{0.f});
    m_registry.emplace<DebrisBody>(e, mass, life);
    return e;
}

void EcsWorld::Step(float dt) {
    // --- KinematicSystem: integrate position ---
    auto kin_view = m_registry.view<Transform, Velocity>();
    for (auto [entity, xf, vel] : kin_view.each()) {
        xf.pos += vel.linear * dt;
    }

    // --- LifetimeSystem: decrement life, destroy dead debris ---
    std::vector<entt::entity> dead;
    auto debris_view = m_registry.view<DebrisBody>();
    for (auto [entity, body] : debris_view.each()) {
        body.life -= dt;
        if (body.life <= 0.f) {
            dead.push_back(entity);
        }
    }
    for (entt::entity e : dead) {
        m_registry.destroy(e);
    }
}

unsigned EcsWorld::EntityCount() const {
    // For the entity storage (swap_only deletion policy), free_list() equals
    // the number of alive (non-destroyed) entity slots.
    // The const overload of registry::storage<T>() returns a pointer.
    const auto* es = m_registry.storage<entt::entity>();
    return es ? static_cast<unsigned>(es->free_list()) : 0u;
}

// ---------------------------------------------------------------------------
// Stub implementation — VOX_ENABLE_ECS == 0
// ---------------------------------------------------------------------------

#else  // !VOX_ENABLE_ECS

void EcsWorld::Init() {
    vox::log::Trace("ecs: stub init (VOX_ENABLE_ECS=0)");
}

void EcsWorld::Shutdown() {}

uint32_t EcsWorld::SpawnDebris(glm::vec3 /*pos*/, glm::vec3 /*vel*/,
                                float /*mass*/, float /*life*/) {
    return 0;
}

void EcsWorld::Step(float /*dt*/) {}

unsigned EcsWorld::EntityCount() const { return 0; }

#endif  // VOX_ENABLE_ECS

}  // namespace vox::ecs
