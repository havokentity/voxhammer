// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// PhysX rigid-body smoke test. Compiled + registered ONLY when
// VOX_ENABLE_PHYSX is ON (see tests/CMakeLists.txt). Proves PhysX is really
// simulating: a dynamic box dropped above a ground plane must fall and settle
// at roughly its half-extent above y = 0.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "physics/PhysicsWorld.h"

#include <cmath>

TEST_CASE("PhysicsWorld: a dynamic box falls and settles on the ground plane") {
    vox::physics::PhysicsWorld world;
    world.Init();

    world.AddGroundPlane();

    const float startY = 20.0f;
    const int box = world.AddDynamicBox(0.0f, startY, 0.0f);
    REQUIRE(box >= 0);  // PhysX present + actor created

    const float y0 = world.BodyY(box);
    REQUIRE(std::isfinite(y0));
    CHECK(y0 == doctest::Approx(startY));

    // Advance ~5s of simulated time at 1/60s. The box starts 20m up, so it
    // needs ~2s to fall (free-fall from 20m) plus margin to come to rest and,
    // eventually, fall asleep.
    for (int i = 0; i < 300; ++i) {
        world.Step(1.0f / 60.0f);
    }

    const float y1 = world.BodyY(box);
    REQUIRE(std::isfinite(y1));

    INFO("box Y: before=" << y0 << " after=" << y1);

    // It fell.
    CHECK(y1 < y0);

    // It came to rest at ~ the box half-extent (0.5) above the plane, not
    // through it. Generous tolerance to absorb solver penetration/restitution.
    CHECK(y1 == doctest::Approx(0.5f).epsilon(0.15));

    // It is genuinely at rest: a few more substeps must not move it further.
    // (More robust than relying on PhysX's exact sleep-timer threshold.)
    for (int i = 0; i < 30; ++i) {
        world.Step(1.0f / 60.0f);
    }
    const float y2 = world.BodyY(box);
    REQUIRE(std::isfinite(y2));
    CHECK(y2 == doctest::Approx(y1).epsilon(0.01));

    // By now the settled box should have gone to sleep -> no active islands.
    CHECK(world.ActiveIslands() == 0u);

    world.Shutdown();
}
