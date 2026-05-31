// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Doctest unit tests for the CPU-only axis-aligned box slicing primitive.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "voxel/destruction/slice_fracture.h"

#include <cstdint>
#include <vector>

namespace {

vox::destruction::Box B(int x0, int y0, int z0, int x1, int y1, int z1) {
    return vox::destruction::Box{glm::ivec3(x0, y0, z0), glm::ivec3(x1, y1, z1)};
}

std::int64_t TotalVolume(const std::vector<vox::destruction::Box>& boxes) {
    std::int64_t total = 0;
    for (const auto& box : boxes) {
        total += box.volume();
    }
    return total;
}

} // namespace

TEST_CASE("slice_fracture: cut outside box -> original box remains, no inside") {
    using namespace vox::destruction;

    const Box box = B(0, 0, 0, 2, 2, 2);
    const auto slice = sliceBox(box, B(5, 5, 5, 6, 6, 6));

    CHECK_FALSE(slice.hasInside);
    REQUIRE(slice.remainder.size() == 1u);
    CHECK(slice.remainder[0].mn == box.mn);
    CHECK(slice.remainder[0].mx == box.mx);
}

TEST_CASE("slice_fracture: cut equal to box -> no remainder, inside is the box") {
    using namespace vox::destruction;

    const Box box = B(0, 0, 0, 2, 2, 2);
    const auto slice = sliceBox(box, box);

    CHECK(slice.hasInside);
    CHECK(slice.inside.mn == box.mn);
    CHECK(slice.inside.mx == box.mx);
    CHECK(slice.remainder.empty());
}

TEST_CASE("slice_fracture: center voxel cut of 3x3x3 -> 26 remainders and volume conserved") {
    using namespace vox::destruction;

    const auto slice = sliceBox(B(0, 0, 0, 2, 2, 2), B(1, 1, 1, 1, 1, 1));

    REQUIRE(slice.hasInside);
    CHECK(slice.inside.mn == glm::ivec3(1, 1, 1));
    CHECK(slice.inside.mx == glm::ivec3(1, 1, 1));
    CHECK(slice.remainder.size() == 26u);
    CHECK(TotalVolume(slice.remainder) + slice.inside.volume() == 27);
}

TEST_CASE("slice_fracture: negative-x face slab cut -> one remainder box") {
    using namespace vox::destruction;

    const auto slice = sliceBox(B(0, 0, 0, 2, 2, 2), B(0, 0, 0, 0, 2, 2));

    REQUIRE(slice.hasInside);
    CHECK(slice.inside.mn == glm::ivec3(0, 0, 0));
    CHECK(slice.inside.mx == glm::ivec3(0, 2, 2));
    REQUIRE(slice.remainder.size() == 1u);
    CHECK(slice.remainder[0].mn == glm::ivec3(1, 0, 0));
    CHECK(slice.remainder[0].mx == glm::ivec3(2, 2, 2));
}

TEST_CASE("slice_fracture: middle cut of 1D bar -> two remainder boxes") {
    using namespace vox::destruction;

    const auto slice = sliceBox(B(0, 0, 0, 4, 0, 0), B(2, 0, 0, 2, 0, 0));

    REQUIRE(slice.hasInside);
    CHECK(slice.inside.mn == glm::ivec3(2, 0, 0));
    CHECK(slice.inside.mx == glm::ivec3(2, 0, 0));
    REQUIRE(slice.remainder.size() == 2u);
    CHECK(slice.remainder[0].mn == glm::ivec3(0, 0, 0));
    CHECK(slice.remainder[0].mx == glm::ivec3(1, 0, 0));
    CHECK(slice.remainder[1].mn == glm::ivec3(3, 0, 0));
    CHECK(slice.remainder[1].mx == glm::ivec3(4, 0, 0));
}

TEST_CASE("slice_fracture: applyCut preserves source indices for untouched and sliced boxes") {
    using namespace vox::destruction;

    const std::vector<Box> boxes = {
        B(0, 0, 0, 4, 0, 0),
        B(10, 0, 0, 10, 0, 0),
    };

    const auto fracture = applyCut(boxes, B(2, 0, 0, 2, 0, 0));

    REQUIRE(fracture.survivors.size() == 3u);
    REQUIRE(fracture.survivorSource.size() == fracture.survivors.size());
    CHECK(fracture.survivors[0].mn == glm::ivec3(0, 0, 0));
    CHECK(fracture.survivors[0].mx == glm::ivec3(1, 0, 0));
    CHECK(fracture.survivorSource[0] == 0);
    CHECK(fracture.survivors[1].mn == glm::ivec3(3, 0, 0));
    CHECK(fracture.survivors[1].mx == glm::ivec3(4, 0, 0));
    CHECK(fracture.survivorSource[1] == 0);
    CHECK(fracture.survivors[2].mn == boxes[1].mn);
    CHECK(fracture.survivors[2].mx == boxes[1].mx);
    CHECK(fracture.survivorSource[2] == 1);

    REQUIRE(fracture.removed.size() == 1u);
    REQUIRE(fracture.removedSource.size() == 1u);
    CHECK(fracture.removed[0].mn == glm::ivec3(2, 0, 0));
    CHECK(fracture.removed[0].mx == glm::ivec3(2, 0, 0));
    CHECK(fracture.removedSource[0] == 0);
}
