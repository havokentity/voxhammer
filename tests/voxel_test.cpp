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
// VoxelWorld: CarveSphere destruction
// ---------------------------------------------------------------------------
TEST_CASE("VoxelWorld: CarveSphere removes a spherical region") {
    vox::voxel::VoxelWorld w;
    w.Init();

    // Fill a solid 11^3 block centered on (20,20,20).
    const int cx = 20, cy = 20, cz = 20;
    for (int z = cx - 5; z <= cx + 5; ++z)
        for (int y = cy - 5; y <= cy + 5; ++y)
            for (int x = cz - 5; x <= cz + 5; ++x)
                w.SetVoxel(x, y, z, 1);

    // A voxel just outside the carve radius (radius 3) -- e.g. 5 units away on +X.
    CHECK(w.GetVoxel(cx + 5, cy, cz) == 1);

    const int removed = w.CarveSphere(cx, cy, cz, 3);

    // Something was actually carved.
    CHECK(removed > 0);

    // Center is now empty.
    CHECK(w.GetVoxel(cx, cy, cz) == 0);

    // A voxel within the radius (2 units away) is gone.
    CHECK(w.GetVoxel(cx + 2, cy, cz) == 0);

    // A voxel outside the radius (5 units away, distance 5 > 3) survives.
    CHECK(w.GetVoxel(cx + 5, cy, cz) == 1);

    // Carving the same spot again removes nothing (already empty).
    CHECK(w.CarveSphere(cx, cy, cz, 3) == 0);

    w.Shutdown();
}

TEST_CASE("VoxelWorld: CarveSphere clamps to world bounds") {
    vox::voxel::VoxelWorld w;
    w.Init();

    // Solid voxel at the world origin corner.
    w.SetVoxel(0, 0, 0, 1);
    // A large carve centered at the corner must not crash (negative coords clamped)
    // and must still erase the in-bounds solid voxel.
    const int removed = w.CarveSphere(0, 0, 0, 8);
    CHECK(removed >= 1);
    CHECK(w.GetVoxel(0, 0, 0) == 0);

    w.Shutdown();
}

// ---------------------------------------------------------------------------
// VoxelWorld: RaycastSolid CPU picking
// ---------------------------------------------------------------------------
TEST_CASE("VoxelWorld: RaycastSolid hits a solid voxel along an axis") {
    vox::voxel::VoxelWorld w;
    w.Init();

    // Single solid voxel at (10, 5, 5).
    w.SetVoxel(10, 5, 5, 1);

    // Cast from outside (x=0) straight along +X, aimed at the voxel center band.
    const float origin[3] = {0.5f, 5.5f, 5.5f};
    const float dir[3]    = {1.0f, 0.0f, 0.0f};
    int hx = -1, hy = -1, hz = -1;
    const bool hit = w.RaycastSolid(origin, dir, 128.0f, hx, hy, hz);
    REQUIRE(hit);
    CHECK(hx == 10);
    CHECK(hy == 5);
    CHECK(hz == 5);

    w.Shutdown();
}

TEST_CASE("VoxelWorld: RaycastSolid misses through empty space") {
    vox::voxel::VoxelWorld w;
    w.Init();

    // One solid voxel, but the ray is aimed well clear of it.
    w.SetVoxel(10, 5, 5, 1);

    const float origin[3] = {0.5f, 50.5f, 50.5f};  // far from the voxel
    const float dir[3]    = {1.0f, 0.0f, 0.0f};
    int hx = -1, hy = -1, hz = -1;
    const bool hit = w.RaycastSolid(origin, dir, 128.0f, hx, hy, hz);
    CHECK_FALSE(hit);

    w.Shutdown();
}

TEST_CASE("VoxelWorld: RaycastSolid respects maxDist") {
    vox::voxel::VoxelWorld w;
    w.Init();

    w.SetVoxel(50, 5, 5, 1);
    const float origin[3] = {0.5f, 5.5f, 5.5f};
    const float dir[3]    = {1.0f, 0.0f, 0.0f};
    int hx = -1, hy = -1, hz = -1;

    // Voxel is ~49 units away; a 10-unit ray must not reach it.
    CHECK_FALSE(w.RaycastSolid(origin, dir, 10.0f, hx, hy, hz));
    // A long-enough ray finds it.
    CHECK(w.RaycastSolid(origin, dir, 100.0f, hx, hy, hz));
    CHECK(hx == 50);

    w.Shutdown();
}

TEST_CASE("VoxelWorld: RaycastSolid treats a start inside solid as a hit") {
    vox::voxel::VoxelWorld w;
    w.Init();

    w.SetVoxel(7, 7, 7, 1);
    // Origin sits inside the solid voxel (7,7,7).
    const float origin[3] = {7.5f, 7.5f, 7.5f};
    const float dir[3]    = {1.0f, 0.0f, 0.0f};
    int hx = -1, hy = -1, hz = -1;
    REQUIRE(w.RaycastSolid(origin, dir, 32.0f, hx, hy, hz));
    CHECK(hx == 7);
    CHECK(hy == 7);
    CHECK(hz == 7);

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
    CHECK(scene.world.GetVoxel(1, 3, 2) == 1);  // vox (1,2,3) -> world (1,3,2): MagicaVoxel Z-up -> engine Y-up
    CHECK(scene.world.GetVoxel(0, 0, 0) == 2);
    CHECK(scene.world.GetVoxel(3, 3, 3) == 3);
    CHECK(scene.world.GetVoxel(2, 2, 2) == 0);  // not set

    // BakeFlatGrid round-trip: vox (1,2,3) -> world (1,3,2) at index 2*4*4+3*4+1 = 45
    auto grid = scene.world.BakeFlatGrid(4);
    REQUIRE(grid.size() == 64u);
    CHECK(grid[2u * 4u * 4u + 3u * 4u + 1u] == 1u);  // world (1,3,2)
    CHECK(grid[0u * 4u * 4u + 0u * 4u + 0u] == 2u);  // (0,0,0)
    CHECK(grid[3u * 4u * 4u + 3u * 4u + 3u] == 3u);  // (3,3,3)
}

TEST_CASE("VoxImport: reject bad magic") {
    std::vector<std::uint8_t> buf = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    vox::voxel::VoxScene scene;
    CHECK_FALSE(vox::voxel::LoadVoxFromMemory(buf.data(), buf.size(), scene));
}

TEST_CASE("VoxImport: RGBA palette maps color[i] to index i+1 (no off-by-one)") {
    // SIZE 2x2x2
    std::vector<std::uint8_t> sizeChunk;
    append_tag(sizeChunk, "SIZE");
    append_u32(sizeChunk, 12); append_u32(sizeChunk, 0);
    append_u32(sizeChunk, 2); append_u32(sizeChunk, 2); append_u32(sizeChunk, 2);

    // XYZI: one voxel (0,0,0) material 1
    std::vector<std::uint8_t> xyziChunk;
    append_tag(xyziChunk, "XYZI");
    append_u32(xyziChunk, 4 + 1 * 4); append_u32(xyziChunk, 0);
    append_u32(xyziChunk, 1);
    xyziChunk.push_back(0); xyziChunk.push_back(0); xyziChunk.push_back(0); xyziChunk.push_back(1);

    // RGBA: 256 colors; on-disk color[i] = (R=i, G=i, B=i, A=255).
    std::vector<std::uint8_t> rgbaChunk;
    append_tag(rgbaChunk, "RGBA");
    append_u32(rgbaChunk, 256 * 4); append_u32(rgbaChunk, 0);
    for (int i = 0; i < 256; ++i) {
        rgbaChunk.push_back(static_cast<std::uint8_t>(i));  // R
        rgbaChunk.push_back(static_cast<std::uint8_t>(i));  // G
        rgbaChunk.push_back(static_cast<std::uint8_t>(i));  // B
        rgbaChunk.push_back(0xFF);                          // A
    }

    std::uint32_t childBytes = static_cast<std::uint32_t>(sizeChunk.size() + xyziChunk.size() + rgbaChunk.size());
    std::vector<std::uint8_t> buf;
    append_tag(buf, "VOX "); append_u32(buf, 150);
    append_tag(buf, "MAIN"); append_u32(buf, 0); append_u32(buf, childBytes);
    buf.insert(buf.end(), sizeChunk.begin(), sizeChunk.end());
    buf.insert(buf.end(), xyziChunk.begin(), xyziChunk.end());
    buf.insert(buf.end(), rgbaChunk.begin(), rgbaChunk.end());

    vox::voxel::VoxScene scene;
    REQUIRE(vox::voxel::LoadVoxFromMemory(buf.data(), buf.size(), scene));

    // Compare RGB only: the alpha byte is now repurposed for emission (0 here,
    // since there is no MATL chunk). r.u32() reads R,G,B little-endian.
    auto rgbOf = [](std::uint32_t p) -> std::uint32_t { return p & 0x00FFFFFFu; };
    auto expectedRGB = [](int i) -> std::uint32_t {
        return (static_cast<std::uint32_t>(i) << 16) | (static_cast<std::uint32_t>(i) << 8) | static_cast<std::uint32_t>(i);
    };
    // Spec: palette index m holds on-disk color[m-1]. (The old off-by-one bug
    // stored color[m] at index m -> these would all be shifted by one.)
    CHECK(rgbOf(scene.palette[1])   == expectedRGB(0));    // material 1 -> color 0
    CHECK(rgbOf(scene.palette[5])   == expectedRGB(4));    // material 5 -> color 4
    CHECK(rgbOf(scene.palette[255]) == expectedRGB(254));  // top index -> color 254, NOT the unused 256th
    CHECK(scene.palette[0] == 0x00000000u);                // index 0 reserved/unused
    // No MATL chunk -> nothing is emissive (alpha/emission byte is 0 everywhere).
    CHECK(((scene.palette[1]   >> 24) & 0xFFu) == 0u);
    CHECK(((scene.palette[255] >> 24) & 0xFFu) == 0u);
}

TEST_CASE("VoxImport: MATL _emit material packs emission into palette alpha") {
    // SIZE 2x2x2 + one voxel (material 1) + RGBA + a MATL marking material 1 emissive.
    std::vector<std::uint8_t> sizeChunk;
    append_tag(sizeChunk, "SIZE");
    append_u32(sizeChunk, 12); append_u32(sizeChunk, 0);
    append_u32(sizeChunk, 2); append_u32(sizeChunk, 2); append_u32(sizeChunk, 2);

    std::vector<std::uint8_t> xyziChunk;
    append_tag(xyziChunk, "XYZI");
    append_u32(xyziChunk, 4 + 1 * 4); append_u32(xyziChunk, 0);
    append_u32(xyziChunk, 1);
    xyziChunk.push_back(0); xyziChunk.push_back(0); xyziChunk.push_back(0); xyziChunk.push_back(1);

    std::vector<std::uint8_t> rgbaChunk;
    append_tag(rgbaChunk, "RGBA");
    append_u32(rgbaChunk, 256 * 4); append_u32(rgbaChunk, 0);
    for (int i = 0; i < 256; ++i) { rgbaChunk.push_back(200); rgbaChunk.push_back(100); rgbaChunk.push_back(50); rgbaChunk.push_back(0xFF); }

    // MATL for material 1: _type=_emit, _emit=1.0, _flux=1  (-> emission 1.0 -> alpha ~32)
    auto appendStr = [](std::vector<std::uint8_t>& b, const char* s) {
        std::uint32_t n = static_cast<std::uint32_t>(std::strlen(s));
        append_u32(b, n);
        for (std::uint32_t i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(s[i]));
    };
    std::vector<std::uint8_t> matlBody;
    append_u32(matlBody, 1);       // material id
    append_u32(matlBody, 3);       // 3 key/value pairs
    appendStr(matlBody, "_type"); appendStr(matlBody, "_emit");
    appendStr(matlBody, "_emit"); appendStr(matlBody, "1.0");
    appendStr(matlBody, "_flux"); appendStr(matlBody, "1");
    std::vector<std::uint8_t> matlChunk;
    append_tag(matlChunk, "MATL");
    append_u32(matlChunk, static_cast<std::uint32_t>(matlBody.size())); append_u32(matlChunk, 0);
    matlChunk.insert(matlChunk.end(), matlBody.begin(), matlBody.end());

    std::uint32_t childBytes = static_cast<std::uint32_t>(sizeChunk.size() + xyziChunk.size() + rgbaChunk.size() + matlChunk.size());
    std::vector<std::uint8_t> buf;
    append_tag(buf, "VOX "); append_u32(buf, 200);
    append_tag(buf, "MAIN"); append_u32(buf, 0); append_u32(buf, childBytes);
    buf.insert(buf.end(), sizeChunk.begin(), sizeChunk.end());
    buf.insert(buf.end(), xyziChunk.begin(), xyziChunk.end());
    buf.insert(buf.end(), rgbaChunk.begin(), rgbaChunk.end());
    buf.insert(buf.end(), matlChunk.begin(), matlChunk.end());

    vox::voxel::VoxScene scene;
    REQUIRE(vox::voxel::LoadVoxFromMemory(buf.data(), buf.size(), scene));

    // Material 1 is emissive -> alpha (emission) byte > 0; a non-emissive index stays 0.
    const std::uint32_t emitByte = (scene.palette[1] >> 24) & 0xFFu;
    CHECK(emitByte > 0u);
    CHECK(((scene.palette[2] >> 24) & 0xFFu) == 0u);
    // RGB still loads correctly (color 0 -> index 1).
    CHECK((scene.palette[1] & 0x00FFFFFFu) == ((50u << 16) | (100u << 8) | 200u));
}
