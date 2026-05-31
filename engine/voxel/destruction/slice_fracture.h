// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// ---------------------------------------------------------------------------
// slice_fracture.h - axis-aligned integer-AABB fracture primitive.
//
// Splits inclusive voxel boxes by an arbitrary inclusive cut box. This is the
// CPU-only "fracture at any point" box primitive: no renderer, PhysX, DX12 or
// Unity dependencies. The output is deterministic and preserves source box
// attribution so callers can copy material / gameplay metadata after carving.
// ---------------------------------------------------------------------------

#include <vector>

#include "voxel/destruction/box_decompose.h"

namespace vox::destruction {

struct SliceResult {
    std::vector<Box> remainder;
    bool hasInside = false;
    Box inside{};
};

SliceResult sliceBox(const Box& box, const Box& cut);

struct FractureResult {
    std::vector<Box> survivors;
    std::vector<int> survivorSource;
    std::vector<Box> removed;
    std::vector<int> removedSource;
};

FractureResult applyCut(const std::vector<Box>& boxes, const Box& cut);

} // namespace vox::destruction
