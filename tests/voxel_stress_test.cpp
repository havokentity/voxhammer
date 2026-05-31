// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Doctest unit tests for the deterministic sparse-box stress propagation pass.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "voxel/destruction/stress.h"

#include <vector>

namespace {

vox::destruction::Box UnitBox(int x, int y, int z) {
    return {glm::ivec3(x, y, z), glm::ivec3(x, y, z)};
}

} // namespace

TEST_CASE("stress: single anchored box carries its own weight and survives") {
    using namespace vox::destruction;
    StressInput in;
    in.boxes = {UnitBox(0, 0, 0)};
    in.weight = {3.0f};
    in.strength = {3.0f};
    in.anchorBoxIndices = {0};

    const auto out = computeStress(in);
    REQUIRE(out.stress.size() == 1u);
    REQUIRE(out.failed.size() == 1u);
    CHECK(out.stress[0] == doctest::Approx(3.0f));
    CHECK_FALSE(out.failed[0]);
}

TEST_CASE("stress: unanchored box is floating and fails") {
    using namespace vox::destruction;
    StressInput in;
    in.boxes = {UnitBox(0, 0, 0)};
    in.weight = {1.0f};
    in.strength = {100.0f};

    const auto out = computeStress(in);
    REQUIRE(out.failed.size() == 1u);
    CHECK(out.failed[0]);
}

TEST_CASE("stress: vertical column routes all load through the anchored base") {
    using namespace vox::destruction;
    constexpr int kCount = 5;
    constexpr float kWeight = 2.0f;

    StressInput in;
    for (int y = 0; y < kCount; ++y) {
        in.boxes.push_back(UnitBox(0, y, 0));
    }
    in.weight.assign(kCount, kWeight);
    in.strength.assign(kCount, 100.0f);
    in.anchorBoxIndices = {0};

    SUBCASE("weak base snaps") {
        in.strength[0] = 9.0f;
        const auto out = computeStress(in);
        CHECK(out.stress[0] == doctest::Approx(static_cast<float>(kCount) * kWeight));
        CHECK(out.failed[0]);
    }

    SUBCASE("strong enough column survives") {
        in.strength.assign(kCount, static_cast<float>(kCount) * kWeight);
        const auto out = computeStress(in);
        CHECK(out.stress[0] == doctest::Approx(static_cast<float>(kCount) * kWeight));
        for (bool failed : out.failed) {
            CHECK_FALSE(failed);
        }
    }
}

TEST_CASE("stress: two anchored supports share a top load") {
    using namespace vox::destruction;
    StressInput in;
    in.boxes = {
        {glm::ivec3(0, 0, 0), glm::ivec3(0, 1, 0)},
        {glm::ivec3(2, 0, 0), glm::ivec3(2, 1, 0)},
        {glm::ivec3(0, 2, 0), glm::ivec3(2, 2, 0)},
    };
    in.weight = {1.0f, 1.0f, 10.0f};
    in.strength = {6.5f, 6.5f, 100.0f};
    in.anchorBoxIndices = {0, 1};

    const auto out = computeStress(in);
    REQUIRE(out.stress.size() == 3u);
    CHECK(out.stress[0] == doctest::Approx(6.0f));
    CHECK(out.stress[1] == doctest::Approx(6.0f));
    CHECK(out.stress[2] == doctest::Approx(10.0f));
    CHECK_FALSE(out.failed[0]);
    CHECK_FALSE(out.failed[1]);
    CHECK_FALSE(out.failed[2]);
}

TEST_CASE("stress: cantilever load increases toward the anchor") {
    using namespace vox::destruction;
    StressInput in;
    for (int x = 0; x < 5; ++x) {
        in.boxes.push_back(UnitBox(x, 0, 0));
    }
    in.weight = {1.0f, 1.0f, 1.0f, 1.0f, 12.0f};
    in.strength.assign(5, 100.0f);
    in.anchorBoxIndices = {0};

    const auto out = computeStress(in);
    REQUIRE(out.stress.size() == 5u);
    CHECK(out.stress[0] > out.stress[3]);
    CHECK(out.stress[1] > out.stress[4]);
    for (bool failed : out.failed) {
        CHECK_FALSE(failed);
    }
}
