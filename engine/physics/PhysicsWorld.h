// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

namespace vox::physics {

// NVIDIA PhysX 5 wrapper: rigid bodies, articulations, vehicles, FEM soft
// bodies, cloth, fluids. GPU rigid bodies on NVIDIA (10K+ islands), CPU path
// elsewhere. Dispatcher wired into the enkiTS job system.
//
// PASS 1 (M3, this pass): real CPU rigid-body Init/Step/Shutdown behind the
// VOX_HAVE_PHYSX gate (set by the VOX_ENABLE_PHYSX CMake option). When the
// option is OFF the methods compile to logging stubs so the engine links with
// no PhysX dependency. The public API below is identical in both builds so a
// future caller (debris-on-carve) works whether or not PhysX is enabled.
class PhysicsWorld {
public:
    void Init();
    void Shutdown();
    void Step(float dt);

    // Number of active (awake) dynamic rigid bodies in the scene. 0 in the stub
    // build and once everything has come to rest.
    unsigned ActiveIslands() const;

    // --- Smoke-test / minimal scene authoring helpers ----------------------
    // No-ops returning sentinels in the stub build.

    // Add a static ground plane at y = 0 (normal +Y).
    void AddGroundPlane();

    // Add a 1m dynamic box (0.5 half-extent) at (x, y, z). Returns a dense
    // index used by BodyY(), or -1 if PhysX is not present / creation failed.
    int AddDynamicBox(float x, float y, float z);

    // World-space Y of dynamic box `idx` (as returned by AddDynamicBox). NaN in
    // the stub build or for an out-of-range index.
    float BodyY(int idx) const;

private:
    // Opaque PIMPL so PhysX headers never leak into engine TUs that include
    // this. Allocated only in the VOX_HAVE_PHYSX build; null otherwise.
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace vox::physics
