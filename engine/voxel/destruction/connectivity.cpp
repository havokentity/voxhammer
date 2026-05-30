// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/destruction/connectivity.h"

#include <algorithm>
#include <cstdint>

namespace vox::destruction {

namespace {

// Face-6 neighbour offsets (dx,dy,dz). Matches the integer voxel DDA stencil
// used everywhere else in the engine and the per-object collision/fracture
// neighbourhood from the spec (§4: object-LOCAL integer voxel checks).
constexpr int kNeighOffsets[6][3] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};

inline std::int32_t Idx(int x, int y, int z, glm::ivec3 dims) {
    return z * dims.x * dims.y + y * dims.x + x;
}

// BFS flood-fill from a single seed, writing |label| into every reachable solid
// voxel in `out` that is currently 0 (unassigned). Single flat queue shared by
// the caller (re-used across seeds) to avoid per-seed allocations.
void BfsFlood(const std::uint8_t* grid,
              glm::ivec3 dims,
              std::vector<std::uint16_t>& out,
              std::vector<std::int32_t>& queue,
              std::int32_t seedIdx,
              std::uint16_t label) {
    // Caller asserts grid[seedIdx] != 0 && out[seedIdx] == 0.
    queue.clear();
    queue.push_back(seedIdx);
    out[seedIdx] = label;

    // Hand-rolled flat queue: we use an index cursor instead of pop_front so
    // the underlying vector keeps its capacity warm across seeds + we avoid
    // std::deque's per-block alloc cost on hot dense grids.
    std::size_t head = 0;
    while (head < queue.size()) {
        const std::int32_t cur = queue[head++];
        const int cz = cur / (dims.x * dims.y);
        const int cy = (cur / dims.x) % dims.y;
        const int cx = cur % dims.x;

        for (int n = 0; n < 6; ++n) {
            const int nx = cx + kNeighOffsets[n][0];
            const int ny = cy + kNeighOffsets[n][1];
            const int nz = cz + kNeighOffsets[n][2];
            if (nx < 0 || nx >= dims.x || ny < 0 || ny >= dims.y || nz < 0 || nz >= dims.z) continue;
            const std::int32_t ni = Idx(nx, ny, nz, dims);
            if (out[ni] != 0) continue;           // already labelled (visited)
            if (grid[ni] == 0) continue;          // empty -- skipped, never enqueued
            out[ni] = label;
            queue.push_back(ni);
        }
    }
}

}  // namespace

ComponentField labelComponents(const std::uint8_t* grid,
                               glm::ivec3 dims,
                               const std::uint8_t* anchorMask) {
    ComponentField cf;
    if (!grid || dims.x <= 0 || dims.y <= 0 || dims.z <= 0) {
        return cf;
    }

    const std::int32_t total = dims.x * dims.y * dims.z;
    cf.ids.assign(static_cast<std::size_t>(total), 0u);

    // Shared scratch queue, sized once to the worst-case voxel count.
    std::vector<std::int32_t> queue;
    queue.reserve(static_cast<std::size_t>(total));

    std::uint16_t nextLabel = 1;

    // -----------------------------------------------------------------------
    // Phase 1: grow ANCHORED components from every solid anchor voxel.
    // Each connected anchor cluster gets one id; the flood then expands that
    // id outward through every face-6-reachable solid voxel.
    // -----------------------------------------------------------------------
    if (anchorMask) {
        for (std::int32_t i = 0; i < total; ++i) {
            if (grid[i] == 0) continue;        // empty
            if (anchorMask[i] == 0) continue;  // not an anchor
            if (cf.ids[i] != 0) continue;      // already swallowed by an earlier anchor's flood

            // Safety: refuse to assign label > 65535 (uint16 cap).
            // In practice this only matters for absurdly fractured grids
            // post-many-carves; we clamp by leaving the rest unlabeled.
            if (nextLabel == 0) break;

            BfsFlood(grid, dims, cf.ids, queue, i, nextLabel);
            ++nextLabel;
        }
    }
    cf.anchoredCount = static_cast<int>(nextLabel) - 1;

    // -----------------------------------------------------------------------
    // Phase 2: every solid voxel not yet labelled belongs to an UNANCHORED
    // island. Same flood, fresh labels starting at anchoredCount + 1.
    // -----------------------------------------------------------------------
    for (std::int32_t i = 0; i < total; ++i) {
        if (grid[i] == 0) continue;
        if (cf.ids[i] != 0) continue;
        if (nextLabel == 0) break;  // overflow guard, see above
        BfsFlood(grid, dims, cf.ids, queue, i, nextLabel);
        ++nextLabel;
    }

    cf.totalCount = static_cast<int>(nextLabel) - 1;
    return cf;
}

}  // namespace vox::destruction
