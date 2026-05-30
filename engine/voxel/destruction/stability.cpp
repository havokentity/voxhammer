// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/destruction/stability.h"

#include <algorithm>
#include <cstdint>

namespace vox::destruction {

namespace {

constexpr int kNeighOffsets[6][3] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};

inline std::int32_t Idx(int x, int y, int z, glm::ivec3 dims) {
    return z * dims.x * dims.y + y * dims.x + x;
}

}  // namespace

std::vector<std::uint16_t> propagateStability(const ComponentField& cf,
                                              glm::ivec3 dims,
                                              const std::uint8_t* anchorMask,
                                              std::uint16_t anchorStrength,
                                              std::uint16_t falloffPerStep) {
    const std::int32_t total = dims.x * dims.y * dims.z;
    std::vector<std::uint16_t> out(static_cast<std::size_t>(total), 0u);

    if (!anchorMask || cf.ids.size() != static_cast<std::size_t>(total) ||
        cf.anchoredCount == 0) {
        return out;
    }

    // Multi-source BFS. We seed the queue with every anchor voxel that is also
    // inside an anchored component, set its stability to `anchorStrength`, and
    // then relax neighbour stability by `falloffPerStep` per face step,
    // saturating at 0. Because every seed starts at the same value and we walk
    // a uniform-cost grid, the natural BFS layer ordering produces the exact
    // shortest-path stability without needing a priority queue.
    std::vector<std::int32_t> queue;
    queue.reserve(static_cast<std::size_t>(total));

    for (std::int32_t i = 0; i < total; ++i) {
        if (anchorMask[i] == 0) continue;
        const std::uint16_t comp = cf.ids[i];
        if (comp == 0) continue;
        if (static_cast<int>(comp) > cf.anchoredCount) continue;  // unanchored island
        out[i] = anchorStrength;
        queue.push_back(i);
    }

    std::size_t head = 0;
    while (head < queue.size()) {
        const std::int32_t cur = queue[head++];
        const std::uint16_t curS = out[cur];
        // Saturating subtract: if curS <= falloffPerStep, neighbours would be
        // 0, which is also their initial value -- nothing to propagate.
        if (curS <= falloffPerStep) continue;
        const std::uint16_t newS = static_cast<std::uint16_t>(curS - falloffPerStep);

        const int cz = cur / (dims.x * dims.y);
        const int cy = (cur / dims.x) % dims.y;
        const int cx = cur % dims.x;

        for (int n = 0; n < 6; ++n) {
            const int nx = cx + kNeighOffsets[n][0];
            const int ny = cy + kNeighOffsets[n][1];
            const int nz = cz + kNeighOffsets[n][2];
            if (nx < 0 || nx >= dims.x || ny < 0 || ny >= dims.y || nz < 0 || nz >= dims.z) continue;
            const std::int32_t ni = Idx(nx, ny, nz, dims);
            const std::uint16_t neighComp = cf.ids[ni];
            if (neighComp == 0) continue;                                 // empty
            if (static_cast<int>(neighComp) > cf.anchoredCount) continue; // unanchored
            if (out[ni] >= newS) continue;                                // already better
            out[ni] = newS;
            queue.push_back(ni);
        }
    }

    return out;
}

}  // namespace vox::destruction
