// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vox::voxel {

// ---------------------------------------------------------------------------
// Palette type (shared between VoxelWorld and VoxImport)
// ---------------------------------------------------------------------------
// 256-entry RGBA8 palette (R=byte0, G=byte1, B=byte2, A=byte3).
// Index 0 is reserved (empty/air); valid material indices are 1..255.
using VoxPalette = std::array<std::uint32_t, 256>;

// ---------------------------------------------------------------------------
// Chunk constants
// ---------------------------------------------------------------------------
inline constexpr unsigned kChunkSize = 32;  // voxels per side
inline constexpr unsigned kChunkVoxels = kChunkSize * kChunkSize * kChunkSize;

// ---------------------------------------------------------------------------
// World grid dimension  (single shared constant; renderer + world use this)
// ---------------------------------------------------------------------------
inline constexpr unsigned kWorldDim = 128;  // 128^3 voxel grid

// ---------------------------------------------------------------------------
// ChunkCoord  (integer grid coordinates of a chunk)
// ---------------------------------------------------------------------------
struct ChunkCoord {
    int x = 0, y = 0, z = 0;
    bool operator==(const ChunkCoord& o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct ChunkCoordHash {
    std::size_t operator()(const ChunkCoord& c) const noexcept {
        // FNV-style mix of three ints
        std::size_t h = 2166136261ULL;
        auto mix = [&](int v) { h ^= static_cast<std::size_t>(v + 0x80000000U); h *= 16777619ULL; };
        mix(c.x); mix(c.y); mix(c.z);
        return h;
    }
};

// ---------------------------------------------------------------------------
// Chunk  – flat 32^3 array of 8-bit material IDs; 0 = empty
// ---------------------------------------------------------------------------
struct Chunk {
    std::array<std::uint8_t, kChunkVoxels> mats{};

    std::uint8_t  Get(unsigned lx, unsigned ly, unsigned lz) const noexcept {
        return mats[lz * kChunkSize * kChunkSize + ly * kChunkSize + lx];
    }
    void Set(unsigned lx, unsigned ly, unsigned lz, std::uint8_t m) noexcept {
        mats[lz * kChunkSize * kChunkSize + ly * kChunkSize + lx] = m;
    }
    bool Empty() const noexcept {
        for (auto v : mats) if (v) return false;
        return true;
    }
};

// ---------------------------------------------------------------------------
// VoxelWorld  – sparse chunk map; voxels addressed by world-space integer coords
// ---------------------------------------------------------------------------
class VoxelWorld {
public:
    void Init();
    void Shutdown();

    // Set/get a voxel by absolute world-space integer coordinate.
    void SetVoxel(int x, int y, int z, std::uint8_t mat);
    std::uint8_t GetVoxel(int x, int y, int z) const noexcept;

    // Destruction: erase every SOLID voxel whose center lies within Euclidean
    // |radius| of (cx,cy,cz). Coordinates are clamped to [0,kWorldDim) per axis.
    // Returns the number of voxels actually removed (i.e. solid -> empty).
    int CarveSphere(int cx, int cy, int cz, int radius);

    // CPU voxel ray-cast that matches the renderer's grid so picking is accurate.
    // Walks an integer-voxel DDA from |origin| along |dir| (normalized internally)
    // up to |maxDist| voxel-units. On the first SOLID voxel (GetVoxel != 0) found
    // inside [0,kWorldDim) it returns true and writes the voxel coord to hitX/Y/Z.
    // A ray that starts inside a solid voxel reports that start cell as the hit.
    // Returns false if nothing solid is encountered within range.
    bool RaycastSolid(const float origin[3], const float dir[3], float maxDist,
                      int& hitX, int& hitY, int& hitZ) const noexcept;

    // Number of live (non-evicted) chunks in the resident map.
    unsigned ResidentChunks() const { return static_cast<unsigned>(chunks_.size()); }

    // Clear all voxel data and reset the merged palette.
    void Clear();

    // Stamp a source VoxScene's voxels into this world at offset (ox,oy,oz).
    // Additive: existing voxels are NOT erased.  Source palette entries are merged
    // into the world's global palette (palette_); indices are remapped accordingly.
    // Coordinates outside [0,kWorldDim) are clipped (matches the renderer's grid).
    void StampVox(const VoxelWorld& srcWorld, const VoxPalette& srcPalette,
                  int ox, int oy, int oz);

    // Access the merged global palette (256 RGBA8 entries; index 0 = empty).
    const VoxPalette& Palette() const { return palette_; }

    // Register an RGBA8 color in the merged palette and return its material
    // index (1..255). Returns the existing index if the exact color is already
    // present; allocates a new slot otherwise. If the palette is full it returns
    // the nearest existing color's index. Used to reserve a stable material id
    // for overlay content (e.g. physics debris cubes) without stamping voxels.
    std::uint8_t AddPaletteColor(std::uint32_t rgba);

    // Write |rgba| into palette slot |idx| (1..255) directly, bumping the
    // next-free watermark past it so later StampVox/AddPaletteColor allocations
    // do not reuse the slot. Used to pin an overlay material (debris) at a slot
    // chosen to be free in the live render palette.
    void SetPaletteColor(std::uint8_t idx, std::uint32_t rgba);

    // Produce the flat uint32 grid that the DX12 renderer reads via StructuredBuffer<uint>.
    // Encoding (matches Renderer.cpp GenerateScene / HLSL PSMain):
    //   index = z * dim * dim + y * dim + x
    //   value = material id as uint32_t (0 = empty)
    // dim is clamped to the bounding box of populated voxels; pass an explicit dim
    // to force a specific cube size (e.g. kWorldDim to match the renderer's kGrid).
    std::vector<std::uint32_t> BakeFlatGrid(unsigned dim) const;

    // World-space bounds of all set voxels (min inclusive, max exclusive).
    void Bounds(int& xmin, int& ymin, int& zmin,
                int& xmax, int& ymax, int& zmax) const noexcept;

private:
    using ChunkMap = std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash>;
    ChunkMap chunks_;

    // Merged global palette: index 0 = empty/air (reserved).
    // Palette entries are accumulated as models are stamped.
    VoxPalette palette_{};
    unsigned   paletteUsed_ = 1;  // next free index (starts at 1; 0 is reserved)

    static ChunkCoord WorldToChunk(int x, int y, int z) noexcept;
    static void       ChunkLocal(int x, int y, int z,
                                 unsigned& lx, unsigned& ly, unsigned& lz) noexcept;

    Chunk& GetOrCreateChunk(const ChunkCoord& cc);
    const Chunk* FindChunk(const ChunkCoord& cc) const noexcept;
};

}  // namespace vox::voxel
