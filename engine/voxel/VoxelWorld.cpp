// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/VoxelWorld.h"

#include "platform/Log.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>

namespace vox::voxel {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int FloorDiv(int a, int b) noexcept {
    // Integer floor-division that rounds toward -inf (not toward zero).
    return a / b - (a % b != 0 && (a ^ b) < 0);
}

// static
ChunkCoord VoxelWorld::WorldToChunk(int x, int y, int z) noexcept {
    return {
        FloorDiv(x, static_cast<int>(kChunkSize)),
        FloorDiv(y, static_cast<int>(kChunkSize)),
        FloorDiv(z, static_cast<int>(kChunkSize))
    };
}

// static
void VoxelWorld::ChunkLocal(int x, int y, int z,
                             unsigned& lx, unsigned& ly, unsigned& lz) noexcept {
    auto mod = [](int v, int m) -> unsigned {
        int r = v % m;
        return static_cast<unsigned>(r < 0 ? r + m : r);
    };
    lx = mod(x, static_cast<int>(kChunkSize));
    ly = mod(y, static_cast<int>(kChunkSize));
    lz = mod(z, static_cast<int>(kChunkSize));
}

Chunk& VoxelWorld::GetOrCreateChunk(const ChunkCoord& cc) {
    return chunks_[cc];
}

const Chunk* VoxelWorld::FindChunk(const ChunkCoord& cc) const noexcept {
    auto it = chunks_.find(cc);
    return it != chunks_.end() ? &it->second : nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void VoxelWorld::Init() {
    vox::log::Info("voxel: VoxelWorld init (sparse chunk map, chunk={}^3)", kChunkSize);
}

void VoxelWorld::Shutdown() {
    Clear();
    vox::log::Trace("voxel: VoxelWorld shutdown");
}

void VoxelWorld::Clear() {
    chunks_.clear();
    palette_ = {};
    paletteUsed_ = 1;  // index 0 is reserved empty/air
}

std::uint8_t VoxelWorld::AddPaletteColor(std::uint32_t rgba) {
    // Reuse an exact match in [1 .. paletteUsed_-1].
    for (unsigned i = 1; i < paletteUsed_; ++i) {
        if (palette_[i] == rgba) {
            return static_cast<std::uint8_t>(i);
        }
    }
    if (paletteUsed_ < 256) {
        const std::uint8_t idx = static_cast<std::uint8_t>(paletteUsed_);
        palette_[paletteUsed_++] = rgba;
        return idx;
    }
    // Palette full: nearest existing color (Euclidean in RGB), matching StampVox.
    auto r8 = [](std::uint32_t c) { return static_cast<int>( c        & 0xFF); };
    auto g8 = [](std::uint32_t c) { return static_cast<int>((c >>  8) & 0xFF); };
    auto b8 = [](std::uint32_t c) { return static_cast<int>((c >> 16) & 0xFF); };
    const int rr = r8(rgba), rg = g8(rgba), rb = b8(rgba);
    unsigned bestDist = UINT_MAX, bestIdx = 1;
    for (unsigned i = 1; i < 256; ++i) {
        const int dr = r8(palette_[i]) - rr, dg = g8(palette_[i]) - rg, db = b8(palette_[i]) - rb;
        const unsigned d = static_cast<unsigned>(dr * dr + dg * dg + db * db);
        if (d < bestDist) { bestDist = d; bestIdx = i; }
    }
    return static_cast<std::uint8_t>(bestIdx);
}

void VoxelWorld::SetPaletteColor(std::uint8_t idx, std::uint32_t rgba) {
    if (idx == 0) return;  // index 0 is reserved air
    palette_[idx] = rgba;
    if (idx >= paletteUsed_) paletteUsed_ = idx + 1u;  // don't reuse this slot
}

void VoxelWorld::StampVox(const VoxelWorld& srcWorld, const VoxPalette& srcPalette,
                           int ox, int oy, int oz) {
    const int kMaxCoord = static_cast<int>(vox::voxel::kWorldDim);  // renderer grid; clip outside [0,kWorldDim)

    int xmin, ymin, zmin, xmax, ymax, zmax;
    srcWorld.Bounds(xmin, ymin, zmin, xmax, ymax, zmax);

    // Clamp the placement offset so the WHOLE model fits in [0,kMaxCoord) instead of
    // clipping to a thin slice when the cursor sits near an edge (max bounds are
    // exclusive). If the model is larger than the grid on an axis, align its min to 0
    // (the far side still clips -- the kWorldDim^3 world is the hard limit).
    auto fitOffset = [&](int off, int lo, int hiExcl) -> int {
        const int minOff = -lo;                 // so dest-min (off+lo) >= 0
        const int maxOff = kMaxCoord - hiExcl;  // so dest-max (off+hiExcl-1) <= kMaxCoord-1
        if (minOff > maxOff) return minOff;      // model bigger than the grid on this axis
        return off < minOff ? minOff : (off > maxOff ? maxOff : off);
    };
    ox = fitOffset(ox, xmin, xmax);
    oy = fitOffset(oy, ymin, ymax);
    oz = fitOffset(oz, zmin, zmax);

    for (int wz = zmin; wz < zmax; ++wz) {
        for (int wy = ymin; wy < ymax; ++wy) {
            for (int wx = xmin; wx < xmax; ++wx) {
                const std::uint8_t srcMat = srcWorld.GetVoxel(wx, wy, wz);
                if (srcMat == 0) continue;

                // Destination world coordinate.
                const int dx = ox + wx;
                const int dy = oy + wy;
                const int dz = oz + wz;
                if (dx < 0 || dy < 0 || dz < 0 ||
                    dx >= kMaxCoord || dy >= kMaxCoord || dz >= kMaxCoord) continue;

                // Look up RGBA in the source palette.
                const std::uint32_t rgba = srcPalette[srcMat];

                // Find or insert this colour in the global merged palette.
                std::uint8_t globalIdx = 0;

                // Search existing entries [1 .. paletteUsed_-1] for an exact match.
                for (unsigned i = 1; i < paletteUsed_; ++i) {
                    if (palette_[i] == rgba) {
                        globalIdx = static_cast<std::uint8_t>(i);
                        break;
                    }
                }

                if (globalIdx == 0) {
                    if (paletteUsed_ < 256) {
                        // Add new colour.
                        globalIdx = static_cast<std::uint8_t>(paletteUsed_);
                        palette_[paletteUsed_++] = rgba;
                    } else {
                        // Palette full: pick the nearest existing colour (Euclidean in RGB).
                        auto r8 = [](std::uint32_t c) { return static_cast<int>( c        & 0xFF); };
                        auto g8 = [](std::uint32_t c) { return static_cast<int>((c >>  8) & 0xFF); };
                        auto b8 = [](std::uint32_t c) { return static_cast<int>((c >> 16) & 0xFF); };
                        int rr = r8(rgba), rg = g8(rgba), rb = b8(rgba);
                        unsigned bestDist = UINT_MAX;
                        unsigned bestIdx  = 1;
                        for (unsigned i = 1; i < 256; ++i) {
                            int dr = r8(palette_[i]) - rr;
                            int dg = g8(palette_[i]) - rg;
                            int db = b8(palette_[i]) - rb;
                            unsigned d = static_cast<unsigned>(dr*dr + dg*dg + db*db);
                            if (d < bestDist) { bestDist = d; bestIdx = i; }
                        }
                        globalIdx = static_cast<std::uint8_t>(bestIdx);
                    }
                }

                SetVoxel(dx, dy, dz, globalIdx);
            }
        }
    }
}

void VoxelWorld::SetVoxel(int x, int y, int z, std::uint8_t mat) {
    ChunkCoord cc = WorldToChunk(x, y, z);
    if (mat == 0) {
        // Setting to empty: only touch a chunk if it already exists.
        auto it = chunks_.find(cc);
        if (it != chunks_.end()) {
            unsigned lx, ly, lz;
            ChunkLocal(x, y, z, lx, ly, lz);
            it->second.Set(lx, ly, lz, 0);
            if (it->second.Empty()) {
                chunks_.erase(it);
            }
        }
    } else {
        unsigned lx, ly, lz;
        ChunkLocal(x, y, z, lx, ly, lz);
        GetOrCreateChunk(cc).Set(lx, ly, lz, mat);
    }
}

std::uint8_t VoxelWorld::GetVoxel(int x, int y, int z) const noexcept {
    const Chunk* c = FindChunk(WorldToChunk(x, y, z));
    if (!c) return 0;
    unsigned lx, ly, lz;
    ChunkLocal(x, y, z, lx, ly, lz);
    return c->Get(lx, ly, lz);
}

int VoxelWorld::CarveSphere(int cx, int cy, int cz, int radius) {
    if (radius < 0) return 0;

    constexpr int kDim = static_cast<int>(kWorldDim);
    // Bounding box of the sphere, clamped to the valid voxel range [0,kDim).
    const int x0 = std::max(0, cx - radius);
    const int y0 = std::max(0, cy - radius);
    const int z0 = std::max(0, cz - radius);
    const int x1 = std::min(kDim - 1, cx + radius);
    const int y1 = std::min(kDim - 1, cy + radius);
    const int z1 = std::min(kDim - 1, cz + radius);

    const long long r2 = static_cast<long long>(radius) * radius;
    int removed = 0;
    for (int z = z0; z <= z1; ++z) {
        const long long dz = z - cz;
        for (int y = y0; y <= y1; ++y) {
            const long long dy = y - cy;
            for (int x = x0; x <= x1; ++x) {
                const long long dx = x - cx;
                if (dx * dx + dy * dy + dz * dz > r2) continue;  // outside the sphere
                if (GetVoxel(x, y, z) == 0) continue;            // already empty
                SetVoxel(x, y, z, 0);                            // erase (auto-evicts empty chunks)
                ++removed;
            }
        }
    }
    return removed;
}

bool VoxelWorld::RaycastSolid(const float origin[3], const float dir[3], float maxDist,
                              int& hitX, int& hitY, int& hitZ) const noexcept {
    // Normalize the direction; a degenerate (zero-length) ray cannot hit anything.
    float dx = dir[0], dy = dir[1], dz = dir[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-12f) return false;
    dx /= len; dy /= len; dz /= len;

    constexpr int kDim = static_cast<int>(kWorldDim);

    // Current voxel cell (floor of the origin).
    int vx = static_cast<int>(std::floor(origin[0]));
    int vy = static_cast<int>(std::floor(origin[1]));
    int vz = static_cast<int>(std::floor(origin[2]));

    // If the ray starts inside a solid, in-bounds voxel, that cell is the hit.
    auto inBounds = [](int x, int y, int z) {
        return x >= 0 && y >= 0 && z >= 0 && x < kDim && y < kDim && z < kDim;
    };
    if (inBounds(vx, vy, vz) && GetVoxel(vx, vy, vz) != 0) {
        hitX = vx; hitY = vy; hitZ = vz;
        return true;
    }

    // Amanatides & Woo voxel DDA setup.
    const int stepX = (dx > 0.0f) ? 1 : (dx < 0.0f ? -1 : 0);
    const int stepY = (dy > 0.0f) ? 1 : (dy < 0.0f ? -1 : 0);
    const int stepZ = (dz > 0.0f) ? 1 : (dz < 0.0f ? -1 : 0);

    const float kInf = std::numeric_limits<float>::infinity();
    // tDelta: distance (in t, i.e. voxel-units along the normalized ray) to cross
    // one full voxel on each axis. tMax: distance to the next voxel boundary.
    const float tDeltaX = (stepX != 0) ? std::fabs(1.0f / dx) : kInf;
    const float tDeltaY = (stepY != 0) ? std::fabs(1.0f / dy) : kInf;
    const float tDeltaZ = (stepZ != 0) ? std::fabs(1.0f / dz) : kInf;

    auto firstBoundary = [](float o, int v, int step, float invAbs) -> float {
        if (step == 0) return std::numeric_limits<float>::infinity();
        // Next integer boundary in the travel direction.
        const float next = (step > 0) ? (static_cast<float>(v) + 1.0f) : static_cast<float>(v);
        return (next - o) * (step > 0 ? invAbs : -invAbs);
    };
    float tMaxX = firstBoundary(origin[0], vx, stepX, tDeltaX);
    float tMaxY = firstBoundary(origin[1], vy, stepY, tDeltaY);
    float tMaxZ = firstBoundary(origin[2], vz, stepZ, tDeltaZ);

    // March until we exceed maxDist or leave the grid for good.
    float t = 0.0f;
    // Safety cap on iterations (3 axes worth of cells across the grid + slack).
    const int maxSteps = 3 * kDim + 3;
    for (int i = 0; i < maxSteps; ++i) {
        // Advance to the nearest axis boundary.
        if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
            vx += stepX; t = tMaxX; tMaxX += tDeltaX;
        } else if (tMaxY <= tMaxZ) {
            vy += stepY; t = tMaxY; tMaxY += tDeltaY;
        } else {
            vz += stepZ; t = tMaxZ; tMaxZ += tDeltaZ;
        }

        if (t > maxDist) return false;

        if (inBounds(vx, vy, vz)) {
            if (GetVoxel(vx, vy, vz) != 0) {
                hitX = vx; hitY = vy; hitZ = vz;
                return true;
            }
        } else {
            // Once we've moved past the far side of the grid in the travel
            // direction on any axis, no in-bounds voxel can ever be hit again.
            if ((stepX > 0 && vx >= kDim) || (stepX < 0 && vx < 0) ||
                (stepY > 0 && vy >= kDim) || (stepY < 0 && vy < 0) ||
                (stepZ > 0 && vz >= kDim) || (stepZ < 0 && vz < 0)) {
                return false;
            }
        }
    }
    return false;
}

void VoxelWorld::Bounds(int& xmin, int& ymin, int& zmin,
                         int& xmax, int& ymax, int& zmax) const noexcept {
    xmin = ymin = zmin = INT_MAX;
    xmax = ymax = zmax = INT_MIN;
    for (const auto& [cc, chunk] : chunks_) {
        int bxlo = cc.x * static_cast<int>(kChunkSize);
        int bylo = cc.y * static_cast<int>(kChunkSize);
        int bzlo = cc.z * static_cast<int>(kChunkSize);
        int bxhi = bxlo + static_cast<int>(kChunkSize) - 1;
        int byhi = bylo + static_cast<int>(kChunkSize) - 1;
        int bzhi = bzlo + static_cast<int>(kChunkSize) - 1;
        xmin = std::min(xmin, bxlo); ymin = std::min(ymin, bylo); zmin = std::min(zmin, bzlo);
        xmax = std::max(xmax, bxhi); ymax = std::max(ymax, byhi); zmax = std::max(zmax, bzhi);
    }
    if (xmin == INT_MAX) { xmin = ymin = zmin = xmax = ymax = zmax = 0; }
    else { ++xmax; ++ymax; ++zmax; }  // make exclusive
}

std::vector<std::uint32_t> VoxelWorld::BakeFlatGrid(unsigned dim) const {
    // Encoding matches Renderer.cpp GenerateScene() and HLSL PSMain:
    //   index = z * dim * dim + y * dim + x
    //   value = material id cast to uint32_t (0 = air/empty)
    const std::size_t total = static_cast<std::size_t>(dim) * dim * dim;
    std::vector<std::uint32_t> grid(total, 0u);

    for (const auto& [cc, chunk] : chunks_) {
        int bx = cc.x * static_cast<int>(kChunkSize);
        int by = cc.y * static_cast<int>(kChunkSize);
        int bz = cc.z * static_cast<int>(kChunkSize);

        for (unsigned lz = 0; lz < kChunkSize; ++lz) {
            int wz = bz + static_cast<int>(lz);
            if (wz < 0 || static_cast<unsigned>(wz) >= dim) continue;
            for (unsigned ly = 0; ly < kChunkSize; ++ly) {
                int wy = by + static_cast<int>(ly);
                if (wy < 0 || static_cast<unsigned>(wy) >= dim) continue;
                for (unsigned lx = 0; lx < kChunkSize; ++lx) {
                    int wx = bx + static_cast<int>(lx);
                    if (wx < 0 || static_cast<unsigned>(wx) >= dim) continue;
                    std::uint8_t m = chunk.Get(lx, ly, lz);
                    if (m == 0) continue;
                    const std::size_t idx = static_cast<std::size_t>(wz) * dim * dim
                                          + static_cast<std::size_t>(wy) * dim
                                          + static_cast<std::size_t>(wx);
                    grid[idx] = static_cast<std::uint32_t>(m);
                }
            }
        }
    }

    vox::log::Trace("voxel: BakeFlatGrid dim={} cells={} chunks={}", dim, total, chunks_.size());
    return grid;
}

}  // namespace vox::voxel
