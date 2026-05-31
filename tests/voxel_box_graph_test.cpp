// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Doctest unit tests for sparse face-6 connectivity over voxel boxes.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "voxel/destruction/box_graph.h"

#include <vector>

namespace {

vox::destruction::Box B(int x0, int y0, int z0, int x1, int y1, int z1) {
    return vox::destruction::Box{glm::ivec3(x0, y0, z0), glm::ivec3(x1, y1, z1)};
}

} // namespace

TEST_CASE("box_graph: empty input -> zero components") {
    using namespace vox::destruction;

    const auto comps = labelBoxComponents({});
    CHECK(comps.count == 0);
    CHECK(comps.labels.empty());
}

TEST_CASE("box_graph: single box -> one component") {
    using namespace vox::destruction;

    const auto comps = labelBoxComponents({B(0, 0, 0, 0, 0, 0)});
    REQUIRE(comps.labels.size() == 1u);
    CHECK(comps.count == 1);
    CHECK(comps.labels[0] == 0);
}

TEST_CASE("box_graph: two face-sharing boxes -> one component") {
    using namespace vox::destruction;

    const auto comps = labelBoxComponents({
        B(0, 0, 0, 0, 1, 1),
        B(1, 0, 0, 1, 1, 1),
    });
    CHECK(comps.count == 1);
    REQUIRE(comps.labels.size() == 2u);
    CHECK(comps.labels[0] == comps.labels[1]);
}

TEST_CASE("box_graph: two boxes separated by a one-cell gap -> two components") {
    using namespace vox::destruction;

    const auto comps = labelBoxComponents({
        B(0, 0, 0, 0, 0, 0),
        B(2, 0, 0, 2, 0, 0),
    });
    CHECK(comps.count == 2);
    REQUIRE(comps.labels.size() == 2u);
    CHECK(comps.labels[0] == 0);
    CHECK(comps.labels[1] == 1);
}

TEST_CASE("box_graph: edge-only contact does not connect") {
    using namespace vox::destruction;

    const auto comps = labelBoxComponents({
        B(0, 0, 0, 0, 0, 0),
        B(1, 1, 0, 1, 1, 0),
    });
    CHECK(comps.count == 2);
    REQUIRE(comps.labels.size() == 2u);
    CHECK(comps.labels[0] == 0);
    CHECK(comps.labels[1] == 1);
}

TEST_CASE("box_graph: L of three boxes -> one component") {
    using namespace vox::destruction;

    const auto comps = labelBoxComponents({
        B(0, 0, 0, 0, 0, 0),
        B(1, 0, 0, 1, 0, 0),
        B(1, 1, 0, 1, 1, 0),
    });
    CHECK(comps.count == 1);
    REQUIRE(comps.labels.size() == 3u);
    CHECK(comps.labels[0] == 0);
    CHECK(comps.labels[1] == 0);
    CHECK(comps.labels[2] == 0);
}

TEST_CASE("box_graph: anchoredComponents marks anchored and floating islands") {
    using namespace vox::destruction;

    const auto comps = labelBoxComponents({
        B(0, 0, 0, 0, 0, 0),
        B(1, 0, 0, 1, 0, 0),
        B(5, 0, 0, 5, 0, 0),
    });
    REQUIRE(comps.count == 2);
    REQUIRE(comps.labels.size() == 3u);
    CHECK(comps.labels[0] == 0);
    CHECK(comps.labels[1] == 0);
    CHECK(comps.labels[2] == 1);

    const auto anchored = anchoredComponents(comps, {1});
    REQUIRE(anchored.size() == 2u);
    CHECK(anchored[0]);
    CHECK_FALSE(anchored[1]);
}
