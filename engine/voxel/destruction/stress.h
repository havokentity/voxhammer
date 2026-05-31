// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// ---------------------------------------------------------------------------
// stress.h - deterministic load propagation over sparse inclusive AABBs.
//
// This is the cheap "weak spans collapse / loaded pipe snaps at its base"
// layer that sits above component stability. It is not a rigid-body or FEM
// solver: boxes route scalar weight through face-6 connected neighbours toward
// grounded sink boxes, and any box whose routed load exceeds material strength
// fails. Boxes with no graph path to an anchor are floating and fail directly.
// ---------------------------------------------------------------------------

#include <vector>

#include "voxel/destruction/box_decompose.h"

namespace vox::destruction {

struct StressInput {
    std::vector<Box> boxes;
    std::vector<float> weight;
    std::vector<float> strength;
    std::vector<int> anchorBoxIndices;
};

struct StressResult {
    std::vector<float> stress;
    std::vector<bool> failed;
};

StressResult computeStress(const StressInput& in);

} // namespace vox::destruction
