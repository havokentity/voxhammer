// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "voxel/destruction/slice_fracture.h"

#include <algorithm>

namespace vox::destruction {

namespace {

struct Span {
    int mn = 0;
    int mx = 0;
    bool inside = false;
};

inline bool Intersects(const Box& a, const Box& b) {
    return a.mn.x <= b.mx.x && a.mx.x >= b.mn.x && a.mn.y <= b.mx.y && a.mx.y >= b.mn.y && a.mn.z <= b.mx.z &&
           a.mx.z >= b.mn.z;
}

inline Box Intersection(const Box& a, const Box& b) {
    return Box{
        glm::max(a.mn, b.mn),
        glm::min(a.mx, b.mx),
    };
}

inline std::vector<Span> AxisSpans(const Box& box, const Box& cut, int axis) {
    std::vector<Span> spans;
    spans.reserve(3);

    if (cut.mn[axis] > box.mn[axis]) {
        spans.push_back(Span{box.mn[axis], std::min(box.mx[axis], cut.mn[axis] - 1), false});
    }

    spans.push_back(Span{
        std::max(box.mn[axis], cut.mn[axis]),
        std::min(box.mx[axis], cut.mx[axis]),
        true,
    });

    if (cut.mx[axis] < box.mx[axis]) {
        spans.push_back(Span{std::max(box.mn[axis], cut.mx[axis] + 1), box.mx[axis], false});
    }

    return spans;
}

} // namespace

SliceResult sliceBox(const Box& box, const Box& cut) {
    SliceResult result;

    if (!Intersects(box, cut)) {
        result.remainder.push_back(box);
        return result;
    }

    result.hasInside = true;
    result.inside = Intersection(box, cut);

    const auto xs = AxisSpans(box, cut, 0);
    const auto ys = AxisSpans(box, cut, 1);
    const auto zs = AxisSpans(box, cut, 2);
    result.remainder.reserve(26);

    for (const Span& x : xs) {
        for (const Span& y : ys) {
            for (const Span& z : zs) {
                if (x.inside && y.inside && z.inside)
                    continue;
                result.remainder.push_back(Box{
                    glm::ivec3(x.mn, y.mn, z.mn),
                    glm::ivec3(x.mx, y.mx, z.mx),
                });
            }
        }
    }

    return result;
}

FractureResult applyCut(const std::vector<Box>& boxes, const Box& cut) {
    FractureResult result;

    for (int i = 0; i < static_cast<int>(boxes.size()); ++i) {
        const SliceResult slice = sliceBox(boxes[static_cast<std::size_t>(i)], cut);

        for (const Box& survivor : slice.remainder) {
            result.survivors.push_back(survivor);
            result.survivorSource.push_back(i);
        }

        if (slice.hasInside) {
            result.removed.push_back(slice.inside);
            result.removedSource.push_back(i);
        }
    }

    return result;
}

} // namespace vox::destruction
