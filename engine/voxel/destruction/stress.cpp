// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/destruction/stress.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace vox::destruction {

namespace {

constexpr int kUnreachable = std::numeric_limits<int>::max();

inline bool OverlapAxis(const Box& a, const Box& b, int axis) {
    return a.mn[axis] <= b.mx[axis] && b.mn[axis] <= a.mx[axis];
}

inline bool TouchAxis(const Box& a, const Box& b, int axis) {
    const std::int64_t aMax = a.mx[axis];
    const std::int64_t bMax = b.mx[axis];
    return aMax + 1 == b.mn[axis] || bMax + 1 == a.mn[axis];
}

inline bool BoxesConnected(const Box& a, const Box& b) {
    const bool overlapX = OverlapAxis(a, b, 0);
    const bool overlapY = OverlapAxis(a, b, 1);
    const bool overlapZ = OverlapAxis(a, b, 2);
    const bool touchX = TouchAxis(a, b, 0);
    const bool touchY = TouchAxis(a, b, 1);
    const bool touchZ = TouchAxis(a, b, 2);

    return (overlapX && overlapY && overlapZ) || (touchX && overlapY && overlapZ) || (overlapX && touchY && overlapZ) ||
           (overlapX && overlapY && touchZ);
}

std::vector<std::vector<int>> BuildAdjacency(const std::vector<Box>& boxes) {
    const int count = static_cast<int>(boxes.size());
    std::vector<std::vector<int>> adjacency(static_cast<std::size_t>(count));

    // Brute-force pairwise adjacency is sufficient for the first stress pass;
    // a spatial grid can accelerate broad-phase candidate gathering later.
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (!BoxesConnected(boxes[static_cast<std::size_t>(i)], boxes[static_cast<std::size_t>(j)]))
                continue;
            adjacency[static_cast<std::size_t>(i)].push_back(j);
            adjacency[static_cast<std::size_t>(j)].push_back(i);
        }
    }

    return adjacency;
}

} // namespace

StressResult computeStress(const StressInput& in) {
    const int count = static_cast<int>(in.boxes.size());
    StressResult result;
    result.stress.assign(static_cast<std::size_t>(count), 0.0f);
    result.failed.assign(static_cast<std::size_t>(count), false);

    if (count == 0)
        return result;

    if (in.weight.size() != static_cast<std::size_t>(count) || in.strength.size() != static_cast<std::size_t>(count)) {
        result.failed.assign(static_cast<std::size_t>(count), true);
        return result;
    }

    const auto adjacency = BuildAdjacency(in.boxes);

    std::vector<int> dist(static_cast<std::size_t>(count), kUnreachable);
    std::vector<int> queue;
    queue.reserve(static_cast<std::size_t>(count));

    for (const int boxIndex : in.anchorBoxIndices) {
        if (boxIndex < 0 || boxIndex >= count)
            continue;
        int& d = dist[static_cast<std::size_t>(boxIndex)];
        if (d == 0)
            continue;
        d = 0;
        queue.push_back(boxIndex);
    }

    std::size_t head = 0;
    while (head < queue.size()) {
        const int cur = queue[head++];
        const int nextDist = dist[static_cast<std::size_t>(cur)] + 1;
        for (const int neigh : adjacency[static_cast<std::size_t>(cur)]) {
            int& neighDist = dist[static_cast<std::size_t>(neigh)];
            if (neighDist <= nextDist)
                continue;
            neighDist = nextDist;
            queue.push_back(neigh);
        }
    }

    std::vector<int> order(static_cast<std::size_t>(count), 0);
    for (int i = 0; i < count; ++i) {
        order[static_cast<std::size_t>(i)] = i;
    }
    std::sort(order.begin(), order.end(), [&dist](int a, int b) {
        const int da = dist[static_cast<std::size_t>(a)];
        const int db = dist[static_cast<std::size_t>(b)];
        if (da != db)
            return da > db;
        return a < b;
    });

    std::vector<float> accumulated(static_cast<std::size_t>(count), 0.0f);
    std::vector<int> supports;
    for (const int boxIndex : order) {
        accumulated[static_cast<std::size_t>(boxIndex)] += in.weight[static_cast<std::size_t>(boxIndex)];
        result.stress[static_cast<std::size_t>(boxIndex)] = accumulated[static_cast<std::size_t>(boxIndex)];

        supports.clear();
        const int curDist = dist[static_cast<std::size_t>(boxIndex)];
        if (curDist != kUnreachable) {
            for (const int neigh : adjacency[static_cast<std::size_t>(boxIndex)]) {
                if (dist[static_cast<std::size_t>(neigh)] < curDist) {
                    supports.push_back(neigh);
                }
            }
        }

        if (supports.empty())
            continue;

        const float share = accumulated[static_cast<std::size_t>(boxIndex)] / static_cast<float>(supports.size());
        for (const int support : supports) {
            accumulated[static_cast<std::size_t>(support)] += share;
        }
    }

    for (int i = 0; i < count; ++i) {
        const std::size_t index = static_cast<std::size_t>(i);
        result.failed[index] = (dist[index] == kUnreachable) || (result.stress[index] > in.strength[index]);
    }

    return result;
}

} // namespace vox::destruction
