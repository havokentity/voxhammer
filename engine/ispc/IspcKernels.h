// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

namespace vox::ispc {

// out[i] = a*x[i] + y[i]. Dispatches to the ISPC kernel when VOX_ENABLE_ISPC,
// else a scalar reference. The unit test asserts the result matches an
// independent reference (and, when ISPC is on, cross-checks the two backends).
void Saxpy(float a, const float* x, const float* y, float* out, int n);

}  // namespace vox::ispc
