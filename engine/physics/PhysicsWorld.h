// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <vector>

namespace vox::physics {

// World-space pose of a live dynamic body, returned by EnumerateBodies(). The
// id is the dense AddDynamicBox() / AddBox() index, stable for the body's
// lifetime; px/py/pz is its current center of mass in world units. Rotation is
// intentionally omitted -- the voxel-debris renderer snaps chunky cubes to the
// grid and ignores orientation.
struct BodyState {
    int   id = -1;
    float px = 0.0f, py = 0.0f, pz = 0.0f;
};

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

    // --- Debris-on-carve helpers (PASS 2) ----------------------------------

    // Add a dynamic cube of half-extent `half` (so edge = 2*half) at (x,y,z)
    // with initial linear velocity (vx,vy,vz). Returns a dense body id (stable
    // for the body's lifetime, reused after RemoveBody compacts the slot list)
    // or -1 in the stub build / on failure. Used to spawn carve debris.
    int AddBox(float x, float y, float z, float half, float vx, float vy, float vz);

    // Append the current world pose of every live dynamic body to |out|
    // (cleared first). Empty in the stub build. The renderer uses each body's
    // position to stamp a voxel cube into the grid every frame.
    void EnumerateBodies(std::vector<BodyState>& out) const;

    // World-space Y of the body whose id is `id` (added via AddBox/AddDynamicBox),
    // or NaN if it is not a live id. Lets the caller cull debris that fell below
    // the ground.
    float BodyPosY(int id) const;

    // Remove (destroy) the dynamic body with id `id`. No-op for an unknown id or
    // in the stub build. Other ids stay valid (the body's slot is tombstoned,
    // not reindexed) so the caller's per-body bookkeeping is not invalidated.
    void RemoveBody(int id);

    // Destroy all dynamic bodies (debris), leaving the scene + ground intact.
    void ClearDynamics();

    // Number of live dynamic bodies (debris). 0 in the stub build.
    unsigned BodyCount() const;

private:
    // Opaque PIMPL so PhysX headers never leak into engine TUs that include
    // this. Allocated only in the VOX_HAVE_PHYSX build; null otherwise.
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace vox::physics
