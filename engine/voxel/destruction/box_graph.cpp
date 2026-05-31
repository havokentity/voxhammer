// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/destruction/box_graph.h"

#include <cstdint>
#include <vector>

namespace vox::destruction {

namespace {

class UnionFind {
  public:
    explicit UnionFind(int count)
        : parent_(static_cast<std::size_t>(count)), rank_(static_cast<std::size_t>(count), 0) {
        for (int i = 0; i < count; ++i) {
            parent_[static_cast<std::size_t>(i)] = i;
        }
    }

    int find(int x) {
        const std::size_t i = static_cast<std::size_t>(x);
        if (parent_[i] != x) {
            parent_[i] = find(parent_[i]);
        }
        return parent_[i];
    }

    void unite(int a, int b) {
        int ra = find(a);
        int rb = find(b);
        if (ra == rb)
            return;

        if (rank_[static_cast<std::size_t>(ra)] < rank_[static_cast<std::size_t>(rb)]) {
            const int tmp = ra;
            ra = rb;
            rb = tmp;
        }
        parent_[static_cast<std::size_t>(rb)] = ra;
        if (rank_[static_cast<std::size_t>(ra)] == rank_[static_cast<std::size_t>(rb)]) {
            ++rank_[static_cast<std::size_t>(ra)];
        }
    }

  private:
    std::vector<int> parent_;
    std::vector<std::uint8_t> rank_;
};

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

} // namespace

BoxComponents labelBoxComponents(const std::vector<Box>& boxes) {
    BoxComponents comps;
    const int count = static_cast<int>(boxes.size());
    if (count == 0)
        return comps;

    UnionFind uf(count);

    // Brute-force pairwise adjacency is clear for the first version; a spatial
    // grid can accelerate broad-phase candidate gathering later.
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (BoxesConnected(boxes[static_cast<std::size_t>(i)], boxes[static_cast<std::size_t>(j)])) {
                uf.unite(i, j);
            }
        }
    }

    comps.labels.assign(static_cast<std::size_t>(count), -1);
    std::vector<int> rootToLabel(static_cast<std::size_t>(count), -1);
    for (int i = 0; i < count; ++i) {
        const int root = uf.find(i);
        int& label = rootToLabel[static_cast<std::size_t>(root)];
        if (label < 0) {
            label = comps.count++;
        }
        comps.labels[static_cast<std::size_t>(i)] = label;
    }

    return comps;
}

std::vector<bool> anchoredComponents(const BoxComponents& comps, const std::vector<int>& anchorBoxIndices) {
    if (comps.count <= 0)
        return {};

    std::vector<bool> anchored(static_cast<std::size_t>(comps.count), false);
    for (const int boxIndex : anchorBoxIndices) {
        if (boxIndex < 0 || boxIndex >= static_cast<int>(comps.labels.size()))
            continue;
        const int label = comps.labels[static_cast<std::size_t>(boxIndex)];
        if (label < 0 || label >= comps.count)
            continue;
        anchored[static_cast<std::size_t>(label)] = true;
    }
    return anchored;
}

} // namespace vox::destruction
