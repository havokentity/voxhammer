// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/destruction/box_decompose.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace vox::destruction {

namespace {

inline std::int32_t Idx(int x, int y, int z, glm::ivec3 dims) {
    return z * dims.x * dims.y + y * dims.x + x;
}

// Test whether a candidate axis-aligned box [bMn..bMx] (inclusive) is fully
// solid-of-`componentId` AND fully uncovered. Used by the greedy grower to
// validate each slab BEFORE committing it - so we never produce a box that
// includes an empty voxel (would seal a hole) or overlaps a prior box.
inline bool BoxIsValid(const ComponentField& cf,
                       const std::vector<std::uint8_t>& covered,
                       glm::ivec3 dims,
                       std::uint16_t componentId,
                       glm::ivec3 bMn, glm::ivec3 bMx) {
    if (bMn.x < 0 || bMn.y < 0 || bMn.z < 0) return false;
    if (bMx.x >= dims.x || bMx.y >= dims.y || bMx.z >= dims.z) return false;
    for (int z = bMn.z; z <= bMx.z; ++z) {
        for (int y = bMn.y; y <= bMx.y; ++y) {
            for (int x = bMn.x; x <= bMx.x; ++x) {
                const std::int32_t i = Idx(x, y, z, dims);
                if (cf.ids[i] != componentId) return false;
                if (covered[i] != 0) return false;
            }
        }
    }
    return true;
}

// Validate only the 2D slab that would be added by extending the existing
// box `[bMn..bMx]` outward by one step along `axis` in direction `dir` (+1
// or -1). Faster than re-checking the whole grown box every step.
inline bool SlabIsValid(const ComponentField& cf,
                        const std::vector<std::uint8_t>& covered,
                        glm::ivec3 dims,
                        std::uint16_t componentId,
                        glm::ivec3 bMn, glm::ivec3 bMx,
                        int axis, int dir) {
    glm::ivec3 sMn = bMn;
    glm::ivec3 sMx = bMx;
    if (dir > 0) {
        sMn[axis] = bMx[axis] + 1;
        sMx[axis] = bMx[axis] + 1;
    } else {
        sMn[axis] = bMn[axis] - 1;
        sMx[axis] = bMn[axis] - 1;
    }
    return BoxIsValid(cf, covered, dims, componentId, sMn, sMx);
}

inline void MarkCovered(std::vector<std::uint8_t>& covered,
                        glm::ivec3 dims, const Box& b) {
    for (int z = b.mn.z; z <= b.mx.z; ++z) {
        for (int y = b.mn.y; y <= b.mx.y; ++y) {
            for (int x = b.mn.x; x <= b.mx.x; ++x) {
                covered[Idx(x, y, z, dims)] = 1;
            }
        }
    }
}

}  // namespace

std::vector<Box> decomposeComponent(const ComponentField& cf,
                                    glm::ivec3 dims,
                                    std::uint16_t componentId) {
    std::vector<Box> out;
    if (componentId == 0) return out;
    if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) return out;
    const std::int32_t total = dims.x * dims.y * dims.z;
    if (cf.ids.size() != static_cast<std::size_t>(total)) return out;

    // covered[idx] == 1 once any returned box has claimed voxel idx. The
    // greedy loop is: pick the next uncovered voxel of `componentId` in
    // row-major order, grow it into the biggest valid axis-aligned box, push,
    // mark covered, repeat. This guarantees every solid voxel of the
    // component ends up in exactly one box.
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(total), 0u);

    for (std::int32_t seed = 0; seed < total; ++seed) {
        if (cf.ids[seed] != componentId) continue;
        if (covered[seed] != 0) continue;

        const int sz = seed / (dims.x * dims.y);
        const int sy = (seed / dims.x) % dims.y;
        const int sx = seed % dims.x;

        Box b{glm::ivec3(sx, sy, sz), glm::ivec3(sx, sy, sz)};

        // Round-robin grow on all six faces: keep extending each face by 1
        // as long as the new slab is fully solid-of-this-component AND
        // uncovered. Stop when an entire pass adds nothing.
        //
        // axis 0/1/2 = X/Y/Z; dir +1 / -1.
        bool anyGrown = true;
        while (anyGrown) {
            anyGrown = false;
            for (int axis = 0; axis < 3; ++axis) {
                for (int dir = +1; dir >= -1; dir -= 2) {
                    if (SlabIsValid(cf, covered, dims, componentId, b.mn, b.mx, axis, dir)) {
                        if (dir > 0) b.mx[axis] += 1;
                        else         b.mn[axis] -= 1;
                        anyGrown = true;
                    }
                }
            }
        }

        out.push_back(b);
        MarkCovered(covered, dims, b);
    }

    return out;
}

}  // namespace vox::destruction
