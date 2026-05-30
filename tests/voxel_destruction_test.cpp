// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Doctest unit tests for the Milestone B destruction prototype:
//   connectivity.h, stability.h, box_decompose.h, island_extract.h
//
// Each test builds a small hand-crafted grid and asserts concrete numeric
// outputs. No renderer / PhysX / DX12 deps - this whole stream is CPU-only,
// independent of the in-flight A1/A2/A3 renderer work.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "voxel/destruction/connectivity.h"
#include "voxel/destruction/stability.h"
#include "voxel/destruction/box_decompose.h"
#include "voxel/destruction/island_extract.h"

#include <cstdint>
#include <vector>

namespace {

// -------- tiny grid helpers ------------------------------------------------
//
// All helpers use the same row-major index encoding as the rest of the
// engine: idx = z*dim.x*dim.y + y*dim.x + x.

inline std::int32_t Idx(int x, int y, int z, glm::ivec3 d) {
    return z * d.x * d.y + y * d.x + x;
}

struct Grid {
    glm::ivec3 dims;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> anchor;

    Grid(int dx, int dy, int dz)
        : dims(dx, dy, dz),
          data(static_cast<std::size_t>(dx) * dy * dz, 0u),
          anchor(static_cast<std::size_t>(dx) * dy * dz, 0u) {}

    void set(int x, int y, int z, std::uint8_t mat) {
        data[Idx(x, y, z, dims)] = mat;
    }
    void setAnchor(int x, int y, int z) {
        anchor[Idx(x, y, z, dims)] = 1u;
    }
    void fill(int x0, int y0, int z0, int x1, int y1, int z1, std::uint8_t mat) {
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    set(x, y, z, mat);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Connectivity: anchored solid cube, anchor strip on the floor.
// ---------------------------------------------------------------------------
TEST_CASE("connectivity: solid 4x4x4 cube fully anchored -> one component, 64 voxels labelled 1") {
    using namespace vox::destruction;
    Grid g(4, 4, 4);
    g.fill(0, 0, 0, 3, 3, 3, 1);
    // Anchor the entire bottom slab y=0.
    for (int z = 0; z < 4; ++z)
        for (int x = 0; x < 4; ++x)
            g.setAnchor(x, 0, z);

    const auto cf = labelComponents(g.data.data(), g.dims, g.anchor.data());
    CHECK(cf.anchoredCount == 1);
    CHECK(cf.totalCount == 1);
    int labelled1 = 0;
    for (auto id : cf.ids) if (id == 1u) ++labelled1;
    CHECK(labelled1 == 64);
}

TEST_CASE("connectivity: two disjoint solid cubes, only one anchored -> 1 anchored + 1 unanchored") {
    using namespace vox::destruction;
    Grid g(8, 4, 4);
    // Anchored cube at x=[0,1], y=[0,1], z=[0,1]
    g.fill(0, 0, 0, 1, 1, 1, 1);
    // Unanchored cube at x=[5,6], y=[2,3], z=[2,3] -- not face-connected to the first
    g.fill(5, 2, 2, 6, 3, 3, 1);
    g.setAnchor(0, 0, 0);

    const auto cf = labelComponents(g.data.data(), g.dims, g.anchor.data());
    CHECK(cf.anchoredCount == 1);
    CHECK(cf.totalCount == 2);

    // Anchored cube has id 1; unanchored has id 2.
    CHECK(cf.ids[Idx(0, 0, 0, g.dims)] == 1);
    CHECK(cf.ids[Idx(1, 1, 1, g.dims)] == 1);
    CHECK(cf.ids[Idx(5, 2, 2, g.dims)] == 2);
    CHECK(cf.ids[Idx(6, 3, 3, g.dims)] == 2);
}

TEST_CASE("connectivity: I-beam intact -> one component; sever the column -> top is unanchored") {
    using namespace vox::destruction;
    // I-beam: 4x1x4 bottom mass at y=0, 4x1x4 top mass at y=5, joined by 1x4x1 column
    // at (x=1,z=1) spanning y=1..4. Bottom anchored.
    Grid g(4, 6, 4);
    g.fill(0, 0, 0, 3, 0, 3, 1);                 // bottom slab
    g.fill(0, 5, 0, 3, 5, 3, 1);                 // top slab
    for (int y = 1; y <= 4; ++y) g.set(1, y, 1, 1);  // joining column
    for (int z = 0; z < 4; ++z)
        for (int x = 0; x < 4; ++x)
            g.setAnchor(x, 0, z);

    SUBCASE("intact -> single anchored component") {
        const auto cf = labelComponents(g.data.data(), g.dims, g.anchor.data());
        CHECK(cf.anchoredCount == 1);
        CHECK(cf.totalCount == 1);
        // Top mass got swept up by the anchor flood through the column.
        CHECK(cf.ids[Idx(2, 5, 2, g.dims)] == 1);
    }
    SUBCASE("sever the column -> 1 anchored (bottom) + 1 unanchored (top)") {
        // Knock out the middle voxel of the column.
        g.set(1, 2, 1, 0);
        g.set(1, 3, 1, 0);
        const auto cf = labelComponents(g.data.data(), g.dims, g.anchor.data());
        CHECK(cf.anchoredCount == 1);
        CHECK(cf.totalCount == 2);
        // Top slab is now the unanchored island (id 2).
        CHECK(cf.ids[Idx(2, 5, 2, g.dims)] == 2);
        // Bottom slab is still anchored (id 1).
        CHECK(cf.ids[Idx(2, 0, 2, g.dims)] == 1);
    }
}

// ---------------------------------------------------------------------------
// Stability: 1x1x16 vertical anchored column. Anchor at y=0, falloff=1.
// ---------------------------------------------------------------------------
TEST_CASE("stability: vertical column -- linear falloff from anchor, unanchored stays 0") {
    using namespace vox::destruction;
    Grid g(1, 16, 1);
    for (int y = 0; y < 16; ++y) g.set(0, y, 0, 1);
    g.setAnchor(0, 0, 0);

    const auto cf = labelComponents(g.data.data(), g.dims, g.anchor.data());
    REQUIRE(cf.anchoredCount == 1);

    const std::uint16_t strength = 1000;
    const std::uint16_t falloff = 1;
    const auto s = propagateStability(cf, g.dims, g.anchor.data(), strength, falloff);

    CHECK(s[Idx(0, 0, 0, g.dims)] == strength);
    CHECK(s[Idx(0, 1, 0, g.dims)] == strength - 1);
    CHECK(s[Idx(0, 5, 0, g.dims)] == strength - 5);
    CHECK(s[Idx(0, 15, 0, g.dims)] == strength - 15);
}

TEST_CASE("stability: saturating subtract -- stability clamps at 0 when distance > strength/falloff") {
    using namespace vox::destruction;
    Grid g(1, 16, 1);
    for (int y = 0; y < 16; ++y) g.set(0, y, 0, 1);
    g.setAnchor(0, 0, 0);
    const auto cf = labelComponents(g.data.data(), g.dims, g.anchor.data());

    // strength=3, falloff=1: y=0..3 are 3,2,1,0; everything beyond y=3 stays 0.
    const auto s = propagateStability(cf, g.dims, g.anchor.data(), 3, 1);
    CHECK(s[Idx(0, 0, 0, g.dims)] == 3);
    CHECK(s[Idx(0, 1, 0, g.dims)] == 2);
    CHECK(s[Idx(0, 2, 0, g.dims)] == 1);
    CHECK(s[Idx(0, 3, 0, g.dims)] == 0);
    CHECK(s[Idx(0, 4, 0, g.dims)] == 0);
    CHECK(s[Idx(0, 15, 0, g.dims)] == 0);
}

TEST_CASE("stability: unanchored island voxels have stability 0") {
    using namespace vox::destruction;
    Grid g(4, 4, 4);
    // Anchored single voxel at origin.
    g.set(0, 0, 0, 1);
    g.setAnchor(0, 0, 0);
    // Floating 2x2x2 island at +x, not touching anchor.
    g.fill(2, 1, 1, 3, 2, 2, 1);

    const auto cf = labelComponents(g.data.data(), g.dims, g.anchor.data());
    REQUIRE(cf.anchoredCount == 1);
    REQUIRE(cf.totalCount == 2);

    const auto s = propagateStability(cf, g.dims, g.anchor.data(), 1000, 1);
    CHECK(s[Idx(0, 0, 0, g.dims)] == 1000);
    // Unanchored voxels: all 0.
    CHECK(s[Idx(2, 1, 1, g.dims)] == 0);
    CHECK(s[Idx(3, 2, 2, g.dims)] == 0);
}

// ---------------------------------------------------------------------------
// Box decompose: cover invariants.
// ---------------------------------------------------------------------------

// Validate the two spec-mandated invariants for a decomposition of `compId`:
//   (a) union(boxes) == set of solid voxels of compId (no empty included,
//       none missed)  -- concave-respecting
//   (b) no two boxes overlap
// Returns the total voxel count covered for additional caller checks.
static int CheckDecompInvariants(const vox::destruction::ComponentField& cf,
                                 glm::ivec3 dims, std::uint16_t compId,
                                 const std::vector<vox::destruction::Box>& boxes) {
    const std::int32_t total = dims.x * dims.y * dims.z;
    std::vector<int> hitCount(static_cast<std::size_t>(total), 0);
    for (const auto& b : boxes) {
        for (int z = b.mn.z; z <= b.mx.z; ++z)
            for (int y = b.mn.y; y <= b.mx.y; ++y)
                for (int x = b.mn.x; x <= b.mx.x; ++x)
                    hitCount[Idx(x, y, z, dims)] += 1;
    }
    int covered = 0;
    bool anyOverlap = false;
    bool anyEmptyCovered = false;
    bool anyMissed = false;
    for (std::int32_t i = 0; i < total; ++i) {
        if (hitCount[i] > 1) anyOverlap = true;
        const bool isOurs = cf.ids[i] == compId;
        if (hitCount[i] > 0 && !isOurs) anyEmptyCovered = true;
        if (hitCount[i] == 0 && isOurs) anyMissed = true;
        if (hitCount[i] > 0) ++covered;
    }
    CHECK_FALSE(anyOverlap);
    CHECK_FALSE(anyEmptyCovered);
    CHECK_FALSE(anyMissed);
    return covered;
}

TEST_CASE("box_decompose: solid 4x4x4 cube -> exactly one box covering 64 voxels") {
    using namespace vox::destruction;
    Grid g(4, 4, 4);
    g.fill(0, 0, 0, 3, 3, 3, 1);
    const auto cf = labelComponents(g.data.data(), g.dims, nullptr);
    REQUIRE(cf.totalCount == 1);

    const auto boxes = decomposeComponent(cf, g.dims, 1);
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].mn == glm::ivec3(0, 0, 0));
    CHECK(boxes[0].mx == glm::ivec3(3, 3, 3));
    CHECK(boxes[0].volume() == 64);
    CheckDecompInvariants(cf, g.dims, 1, boxes);
}

TEST_CASE("box_decompose: L-shape -> >=2 boxes, full cover, no overlap, no empty covered") {
    using namespace vox::destruction;
    // L-shape in the XY plane (z=0 only): a 4x1 horizontal arm + a 1x3 vertical
    // arm joined at the corner. Concave: the bounding box has 4*4*1 = 16 cells
    // but only 7 are solid. Greedy box decomp MUST emit >=2 boxes (one box
    // would also cover the empty quadrant, sealing the L's concavity shut).
    //
    //   layout (z=0):
    //     y=0:  X . . .
    //     y=1:  X . . .
    //     y=2:  X . . .
    //     y=3:  X X X X
    //
    Grid g(4, 4, 1);
    // Horizontal arm along the top row (y=3): x=0..3.
    for (int x = 0; x < 4; ++x) g.set(x, 3, 0, 1);
    // Vertical arm down the left column (x=0): y=0..3.
    for (int y = 0; y < 4; ++y) g.set(0, y, 0, 1);

    const auto cf = labelComponents(g.data.data(), g.dims, nullptr);
    REQUIRE(cf.totalCount == 1);

    int solidCount = 0;
    for (auto id : cf.ids) if (id == 1u) ++solidCount;
    REQUIRE(solidCount == 7);  // 4 + 4 - 1 (corner shared)

    const auto boxes = decomposeComponent(cf, g.dims, 1);
    CHECK(boxes.size() >= 2);
    const int covered = CheckDecompInvariants(cf, g.dims, 1, boxes);
    CHECK(covered == solidCount);

    // Spec invariant: every cell of the L's concave quadrant (x>=1 && y<=2)
    // must sit inside ZERO boxes -- otherwise the box compound would seal the
    // concavity and a projectile aimed through it would wrongly collide.
    for (int y = 0; y <= 2; ++y) {
        for (int x = 1; x <= 3; ++x) {
            for (const auto& b : boxes) {
                CHECK_FALSE(b.contains(x, y, 0));
            }
        }
    }
}

TEST_CASE("box_decompose: 6x6x6 with a central 2x2x2 hole -> 208 voxels, multiple boxes, void uncovered") {
    using namespace vox::destruction;
    Grid g(6, 6, 6);
    g.fill(0, 0, 0, 5, 5, 5, 1);
    // Punch the 2x2x2 hole at the center (2..3 on each axis -- 8 voxels).
    g.fill(2, 2, 2, 3, 3, 3, 0);

    const auto cf = labelComponents(g.data.data(), g.dims, nullptr);
    REQUIRE(cf.totalCount == 1);
    int solidCount = 0;
    for (auto id : cf.ids) if (id == 1u) ++solidCount;
    REQUIRE(solidCount == 216 - 8);  // 208

    const auto boxes = decomposeComponent(cf, g.dims, 1);
    CHECK(boxes.size() >= 2);
    const int covered = CheckDecompInvariants(cf, g.dims, 1, boxes);
    CHECK(covered == 208);

    // Spec's "holes for free" guarantee: every voxel of the empty void must
    // sit inside zero boxes (would otherwise seal the hole shut).
    for (int z = 2; z <= 3; ++z)
        for (int y = 2; y <= 3; ++y)
            for (int x = 2; x <= 3; ++x)
                for (const auto& b : boxes)
                    CHECK_FALSE(b.contains(x, y, z));
}

// ---------------------------------------------------------------------------
// Island extract: unanchored 2x2x2 island at (5,5,5) in a 16^3 parent.
// ---------------------------------------------------------------------------
TEST_CASE("island_extract: 2x2x2 unanchored island at (5,5,5) -> dims=(2,2,2), 8 solids, origin=(5,5,5)") {
    using namespace vox::destruction;
    Grid g(16, 16, 16);
    // Anchored seed somewhere far so that the island is the second component.
    g.set(0, 0, 0, 1);
    g.setAnchor(0, 0, 0);
    // The unanchored 2x2x2 island.
    g.fill(5, 5, 5, 6, 6, 6, 7);  // material 7

    const auto cf = labelComponents(g.data.data(), g.dims, g.anchor.data());
    REQUIRE(cf.anchoredCount == 1);
    REQUIRE(cf.totalCount == 2);

    const std::uint16_t islandId = 2;
    const auto lg = extractIsland(g.data.data(), cf, g.dims, islandId);
    CHECK(lg.dims == glm::ivec3(2, 2, 2));
    CHECK(lg.originInParent == glm::ivec3(5, 5, 5));
    REQUIRE(lg.data.size() == 8u);
    int solid = 0;
    for (auto v : lg.data) if (v != 0u) ++solid;
    CHECK(solid == 8);
    // Material is preserved verbatim.
    for (auto v : lg.data) if (v != 0u) CHECK(v == 7u);
}

TEST_CASE("island_extract: concave island preserves holes -- non-component voxels stay 0 in the local grid") {
    using namespace vox::destruction;
    // Build a 4x4x4 island with a 2x2x2 internal void. The whole solid shell
    // is one connected component thanks to face-6 adjacency around the void.
    Grid g(8, 8, 8);
    g.fill(2, 2, 2, 5, 5, 5, 3);             // solid 4x4x4 island at offset (2,2,2)
    // Hollow out a 2x2x2 cube in the middle of the island (parent coords 3..4).
    g.fill(3, 3, 3, 4, 4, 4, 0);

    const auto cf = labelComponents(g.data.data(), g.dims, nullptr);
    REQUIRE(cf.totalCount == 1);

    const auto lg = extractIsland(g.data.data(), cf, g.dims, 1);
    CHECK(lg.dims == glm::ivec3(4, 4, 4));
    CHECK(lg.originInParent == glm::ivec3(2, 2, 2));

    // The hole in local coords is at lx=1..2, ly=1..2, lz=1..2. Those must be 0.
    for (int lz = 1; lz <= 2; ++lz)
        for (int ly = 1; ly <= 2; ++ly)
            for (int lx = 1; lx <= 2; ++lx) {
                const std::size_t li =
                    static_cast<std::size_t>(lz) * 4 * 4 +
                    static_cast<std::size_t>(ly) * 4 +
                    static_cast<std::size_t>(lx);
                CHECK(lg.data[li] == 0u);
            }
    // Outer shell cells are solid (== 3).
    CHECK(lg.data[0] == 3u);  // (0,0,0) local = (2,2,2) parent
}
