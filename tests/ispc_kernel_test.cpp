// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ispc/IspcKernels.h"

#include <vector>

// Validates the Saxpy wrapper against an independent reference. With
// VOX_ENABLE_ISPC this cross-checks the ISPC kernel against scalar math; with
// it off it validates the scalar fallback path + the API.
TEST_CASE("saxpy matches scalar reference") {
    const int n = 1024;
    std::vector<float> x(n), y(n), out(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        x[i] = static_cast<float>(i) * 0.5f;
        y[i] = static_cast<float>(n - i);
    }
    const float a = 2.5f;

    vox::ispc::Saxpy(a, x.data(), y.data(), out.data(), n);

    for (int i = 0; i < n; ++i) {
        CHECK(out[i] == doctest::Approx(a * x[i] + y[i]));
    }
}
