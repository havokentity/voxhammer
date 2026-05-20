// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
// Sample ISPC kernel that proves the .ispc -> object + header build path.
// Real kernels (voxel boolean ops, particle physics, broadphase culling,
// island detection) land in M2+. out[i] = a*x[i] + y[i].

export void saxpy(uniform float a,
                  uniform float x[],
                  uniform float y[],
                  uniform float out[],
                  uniform int n) {
    foreach (i = 0 ... n) {
        out[i] = a * x[i] + y[i];
    }
}
