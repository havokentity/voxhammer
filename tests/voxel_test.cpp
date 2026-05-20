// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "voxel/VoxelWorld.h"
#include "voxel/VoxImport.h"

#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// VoxelWorld: sparse set/get and BakeFlatGrid
// ---------------------------------------------------------------------------
TEST_CASE("VoxelWorld: SetVoxel / GetVoxel round-trip") {
    vox::voxel::VoxelWorld w;
    w.Init();

    CHECK(w.GetVoxel(0, 0, 0) == 0);  // empty default

    w.SetVoxel(0, 0, 0, 1);
    CHECK(w.GetVoxel(0, 0, 0) == 1);

    w.SetVoxel(10, 20, 30, 3);
    CHECK(w.GetVoxel(10, 20, 30) == 3);
    CHECK(w.GetVoxel(10, 20, 31) == 0);  // adjacent: empty

    // Overwrite with empty removes voxel
    w.SetVoxel(0, 0, 0, 0);
    CHECK(w.GetVoxel(0, 0, 0) == 0);

    w.Shutdown();
}

TEST_CASE("VoxelWorld: ResidentChunks grows and shrinks") {
    vox::voxel::VoxelWorld w;
    w.Init();

    CHECK(w.ResidentChunks() == 0);

    // Two voxels in the same chunk
    w.SetVoxel(0, 0, 0, 1);
    w.SetVoxel(1, 1, 1, 2);
    CHECK(w.ResidentChunks() == 1);

    // A voxel in a different chunk
    w.SetVoxel(100, 100, 100, 3);
    CHECK(w.ResidentChunks() == 2);

    // Clear one chunk fully
    w.SetVoxel(0, 0, 0, 0);
    w.SetVoxel(1, 1, 1, 0);
    CHECK(w.ResidentChunks() == 1);

    w.Shutdown();
}

TEST_CASE("VoxelWorld: BakeFlatGrid size and encoding") {
    vox::voxel::VoxelWorld w;
    w.Init();

    const unsigned dim = 64;

    // Place a known voxel at (5, 10, 20), mat=2
    w.SetVoxel(5, 10, 20, 2);

    auto grid = w.BakeFlatGrid(dim);

    // Grid must be exactly dim^3 uint32s.
    REQUIRE(grid.size() == static_cast<std::size_t>(dim) * dim * dim);

    // Encoding: index = z * dim * dim + y * dim + x  (matches Renderer.cpp GenerateScene)
    const std::size_t idx = 20u * dim * dim + 10u * dim + 5u;
    CHECK(grid[idx] == 2u);

    // All other set-cells in a fresh world must be 0 at (6,10,20)
    CHECK(grid[20u * dim * dim + 10u * dim + 6u] == 0u);

    // Empty voxel is 0
    CHECK(grid[0] == 0u);

    w.Shutdown();
}

TEST_CASE("VoxelWorld: BakeFlatGrid clamps out-of-range voxels") {
    vox::voxel::VoxelWorld w;
    w.Init();

    const unsigned dim = 8;
    w.SetVoxel(3, 3, 3, 1);    // in range
    w.SetVoxel(100, 0, 0, 2);  // out of range

    auto grid = w.BakeFlatGrid(dim);
    REQUIRE(grid.size() == static_cast<std::size_t>(dim) * dim * dim);

    // In-range voxel present
    CHECK(grid[3u * dim * dim + 3u * dim + 3u] == 1u);

    // Out-of-range voxel not reflected in the grid
    bool any_two = false;
    for (auto v : grid) if (v == 2u) { any_two = true; break; }
    CHECK_FALSE(any_two);

    w.Shutdown();
}

// ---------------------------------------------------------------------------
// VoxImport: parse a minimal hand-crafted .vox byte buffer
// ---------------------------------------------------------------------------
namespace {

// Helpers to build a little-endian binary buffer.
void append_u32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>(v));
    buf.push_back(static_cast<std::uint8_t>(v >> 8));
    buf.push_back(static_cast<std::uint8_t>(v >> 16));
    buf.push_back(static_cast<std::uint8_t>(v >> 24));
}
void append_tag(std::vector<std::uint8_t>& buf, const char t[4]) {
    buf.push_back(static_cast<std::uint8_t>(t[0]));
    buf.push_back(static_cast<std::uint8_t>(t[1]));
    buf.push_back(static_cast<std::uint8_t>(t[2]));
    buf.push_back(static_cast<std::uint8_t>(t[3]));
}

// Build a minimal valid .vox: one 4x4x4 model with 3 voxels.
// Layout: SIZE(4,4,4) + XYZI(3 voxels) all under MAIN.
std::vector<std::uint8_t> BuildMinimalVox() {
    // --- SIZE chunk ---
    std::vector<std::uint8_t> sizeChunk;
    append_tag(sizeChunk, "SIZE");
    append_u32(sizeChunk, 12);   // selfBytes: 3 x uint32
    append_u32(sizeChunk, 0);    // childBytes
    append_u32(sizeChunk, 4);    // sizeX
    append_u32(sizeChunk, 4);    // sizeY
    append_u32(sizeChunk, 4);    // sizeZ

    // --- XYZI chunk: 3 voxels ---
    // voxel bytes: x,y,z,matIndex (1-indexed palette)
    std::vector<std::uint8_t> xyziChunk;
    append_tag(xyziChunk, "XYZI");
    append_u32(xyziChunk, 4 + 3 * 4);  // selfBytes: nVox(4) + 3*4 bytes
    append_u32(xyziChunk, 0);           // childBytes
    append_u32(xyziChunk, 3);           // nVox
    // voxel 1: (1,2,3, mat=1)
    xyziChunk.push_back(1); xyziChunk.push_back(2); xyziChunk.push_back(3); xyziChunk.push_back(1);
    // voxel 2: (0,0,0, mat=2)
    xyziChunk.push_back(0); xyziChunk.push_back(0); xyziChunk.push_back(0); xyziChunk.push_back(2);
    // voxel 3: (3,3,3, mat=3)
    xyziChunk.push_back(3); xyziChunk.push_back(3); xyziChunk.push_back(3); xyziChunk.push_back(3);

    // --- MAIN chunk ---
    std::uint32_t childBytes = static_cast<std::uint32_t>(sizeChunk.size() + xyziChunk.size());

    std::vector<std::uint8_t> buf;
    append_tag(buf, "VOX ");
    append_u32(buf, 150);        // version
    append_tag(buf, "MAIN");
    append_u32(buf, 0);          // MAIN selfBytes
    append_u32(buf, childBytes); // MAIN childBytes
    buf.insert(buf.end(), sizeChunk.begin(), sizeChunk.end());
    buf.insert(buf.end(), xyziChunk.begin(), xyziChunk.end());
    return buf;
}

}  // anonymous namespace

TEST_CASE("VoxImport: parse minimal in-memory .vox") {
    auto buf = BuildMinimalVox();
    vox::voxel::VoxScene scene;
    const bool ok = vox::voxel::LoadVoxFromMemory(buf.data(), buf.size(), scene);
    REQUIRE(ok);

    // Dimensions
    CHECK(scene.sizeX == 4);
    CHECK(scene.sizeY == 4);
    CHECK(scene.sizeZ == 4);

    // Voxel count
    CHECK(scene.voxelCount == 3);

    // At least 1 chunk was created
    CHECK(scene.world.ResidentChunks() >= 1);

    // Materials at expected positions
    CHECK(scene.world.GetVoxel(1, 2, 3) == 1);
    CHECK(scene.world.GetVoxel(0, 0, 0) == 2);
    CHECK(scene.world.GetVoxel(3, 3, 3) == 3);
    CHECK(scene.world.GetVoxel(2, 2, 2) == 0);  // not set

    // BakeFlatGrid round-trip: voxel (1,2,3) at index 3*4*4+2*4+1 = 57
    auto grid = scene.world.BakeFlatGrid(4);
    REQUIRE(grid.size() == 64u);
    CHECK(grid[3u * 4u * 4u + 2u * 4u + 1u] == 1u);  // (1,2,3)
    CHECK(grid[0u * 4u * 4u + 0u * 4u + 0u] == 2u);  // (0,0,0)
    CHECK(grid[3u * 4u * 4u + 3u * 4u + 3u] == 3u);  // (3,3,3)
}

TEST_CASE("VoxImport: reject bad magic") {
    std::vector<std::uint8_t> buf = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    vox::voxel::VoxScene scene;
    CHECK_FALSE(vox::voxel::LoadVoxFromMemory(buf.data(), buf.size(), scene));
}
