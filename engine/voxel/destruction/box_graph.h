// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// ---------------------------------------------------------------------------
// box_graph.h - face-6 connected component labelling over a set of inclusive
// voxel AABBs.
//
// This is the sparse-box sibling of connectivity.h: instead of walking a dense
// voxel grid, it treats each Box as a node and unions nodes whose volumes
// overlap or whose faces touch with overlap on the other two axes. Edge-only
// and corner-only contacts do not connect.
// ---------------------------------------------------------------------------

#include <vector>

#include "voxel/destruction/box_decompose.h"

namespace vox::destruction {

struct BoxComponents {
    std::vector<int> labels; // labels[i] = dense component id of box i, in [0,count)
    int count = 0;
};

// Label face-6 connected components over `boxes` using union-find. Dense
// component ids are deterministic: components are numbered by ascending
// smallest member index.
BoxComponents labelBoxComponents(const std::vector<Box>& boxes);

// A component is anchored iff any of its boxes is listed in `anchorBoxIndices`.
// Invalid indices are ignored.
std::vector<bool> anchoredComponents(const BoxComponents& comps, const std::vector<int>& anchorBoxIndices);

} // namespace vox::destruction
