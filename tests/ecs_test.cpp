// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ecs/EcsWorld.h"
#include "ecs/components.h"

#include <cmath>

using namespace vox::ecs;

// ---------------------------------------------------------------------------
// Basic integration tests.  These exercise the VOX_ENABLE_ECS path; when the
// feature is off the stub satisfies the API and all checks that rely on real
// simulation are skipped via the compile-time guard.
// ---------------------------------------------------------------------------

TEST_CASE("EcsWorld Init/Shutdown is safe to call repeatedly") {
    EcsWorld world;
    world.Init();
    world.Shutdown();
    world.Init();
    world.Shutdown();
}

#if VOX_ENABLE_ECS

TEST_CASE("EntityCount reflects spawned entities") {
    EcsWorld world;
    world.Init();

    CHECK(world.EntityCount() == 0);

    constexpr int N = 10;
    for (int i = 0; i < N; ++i) {
        world.SpawnDebris(
            glm::vec3(static_cast<float>(i), 0.f, 0.f),
            glm::vec3(1.f, 0.f, 0.f),
            /*mass=*/1.f,
            /*life=*/5.f
        );
    }
    CHECK(world.EntityCount() == static_cast<unsigned>(N));

    world.Shutdown();
    CHECK(world.EntityCount() == 0);
}

TEST_CASE("Step: entity stays alive when life has not expired") {
    EcsWorld world;
    world.Init();

    const glm::vec3 vel  = {2.f, 0.f, 0.f};
    const glm::vec3 pos0 = {0.f, 0.f, 0.f};
    const float     life = 100.f;          // won't expire during the test
    const float     dt   = 0.5f;

    world.SpawnDebris(pos0, vel, 1.f, life);
    world.Step(dt);

    // Entity is still alive (no crash, count unchanged)
    CHECK(world.EntityCount() == 1);

    world.Shutdown();
}

TEST_CASE("Step: direct component read-back after integration") {
    // Access registry internals via a subclass for white-box testing.
    struct TestWorld : EcsWorld {
        entt::registry& reg() { return m_registry; }
    };

    TestWorld world;
    world.Init();

    const glm::vec3 vel  = {3.f, 0.f, 0.f};
    const glm::vec3 pos0 = {1.f, 2.f, 3.f};
    const float     dt   = 0.25f;

    auto e = world.SpawnDebris(pos0, vel, 1.f, 99.f);
    world.Step(dt);

    const auto& xf = world.reg().get<Transform>(e);
    CHECK(std::abs(xf.pos.x - (pos0.x + vel.x * dt)) < 1e-5f);
    CHECK(std::abs(xf.pos.y - pos0.y)                 < 1e-5f);
    CHECK(std::abs(xf.pos.z - pos0.z)                 < 1e-5f);

    world.Shutdown();
}

TEST_CASE("Step: debris with expired life is destroyed") {
    EcsWorld world;
    world.Init();

    // Spawn 5 long-lived + 3 short-lived debris
    constexpr int LIVE  = 5;
    constexpr int DYING = 3;

    for (int i = 0; i < LIVE; ++i) {
        world.SpawnDebris({0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 1.f, /*life=*/10.f);
    }
    for (int i = 0; i < DYING; ++i) {
        world.SpawnDebris({0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 1.f, /*life=*/0.1f);
    }

    CHECK(world.EntityCount() == static_cast<unsigned>(LIVE + DYING));

    // A single step with dt > 0.1 expires the short-lived ones
    world.Step(0.5f);

    CHECK(world.EntityCount() == static_cast<unsigned>(LIVE));

    world.Shutdown();
}

TEST_CASE("Multiple Step calls accumulate correctly and cull all expired") {
    EcsWorld world;
    world.Init();

    // One entity with life=1s, stepping by 0.4s three times (total 1.2s)
    world.SpawnDebris({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 1.f, 1.f);
    CHECK(world.EntityCount() == 1);

    world.Step(0.4f);
    CHECK(world.EntityCount() == 1);  // 0.6s remaining

    world.Step(0.4f);
    CHECK(world.EntityCount() == 1);  // 0.2s remaining

    world.Step(0.4f);
    CHECK(world.EntityCount() == 0);  // -0.2s → culled

    world.Shutdown();
}

#else  // !VOX_ENABLE_ECS

TEST_CASE("Stub: EntityCount is always 0") {
    EcsWorld world;
    world.Init();
    world.SpawnDebris({0.f,0.f,0.f}, {1.f,0.f,0.f}, 1.f, 5.f);
    world.Step(1.f);
    CHECK(world.EntityCount() == 0);
    world.Shutdown();
}

#endif  // VOX_ENABLE_ECS
