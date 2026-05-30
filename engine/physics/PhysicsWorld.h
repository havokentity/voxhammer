// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <vector>

namespace vox::physics {

// World-space pose of a live dynamic body, returned by EnumerateBodies(). The
// id is the dense AddDynamicBox() / AddBox() / AcquireBox() index, stable for
// the body's lifetime; px/py/pz is its current center of mass in world units.
// (qx,qy,qz,qw) is the body's orientation quaternion (identity in the stub
// build). The voxel-chunk renderer uses BOTH position and rotation to stamp
// each chunk's real voxels at the body's full transform so they tumble.
struct BodyState {
    int   id = -1;
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;  // orientation (identity)
};

// One axis-aligned box of the STATIC world collider, in WORLD units (the same
// space the dynamic debris bodies live in: world == voxel grid coords, voxel
// size 1). [min..max] are the box's world-space corners; SetWorldCollider()
// turns each into a PxBoxGeometry shape (half-extent = (max-min)/2, local pose
// = box center) on a single shared PxRigidStatic. Passing the boxes already in
// world units (decomposed caller-side) keeps the physics lib free of any voxel
// / glm dependency. min < max per axis is assumed (a single voxel is a 1-unit
// box, e.g. min={5,0,5} max={6,1,6}).
struct ColliderBox {
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
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

    // --- World collider (Milestone C): a static box-compound that matches the
    // voxel terrain so debris + detached islands LAND on it instead of falling
    // through to the y=0 plane / kill plane. -----------------------------------

    // Replace the (single, legacy) world-collider static actor with a fresh
    // PxRigidStatic carrying ONE PxBoxGeometry shape per `boxes` entry (boxes in
    // WORLD units; see ColliderBox). Releases any previous single-actor collider
    // first (so repeated rebuilds after each carve do not leak). Additive to the
    // y=0 ground plane. Empty `boxes` clears it. No-op stub when PhysX is off.
    //
    // NOTE: the dirty-region rebuild path uses SetColliderRegion() instead -- it
    // partitions the world into a fixed REGION grid and rebuilds only the regions
    // a carve/settle touched. SetWorldCollider remains for callers that want a
    // single monolithic actor (it lives in its own slot, independent of the
    // region grid; ClearWorldCollider clears both).
    void SetWorldCollider(const std::vector<ColliderBox>& boxes);

    // --- Dirty-region world collider (only re-decompose + rebuild the carved
    // area, not the whole 128^3 world) -----------------------------------------
    //
    // The world is partitioned into a fixed grid of cubic REGIONS; each region
    // owns its OWN static box-compound PxRigidStatic. SetColliderRegion releases
    // that region's previous actor (no leak) and builds a fresh one from `boxes`
    // (already in WORLD units, offset by the region origin caller-side). An empty
    // `boxes` clears just that region's actor. `regionIndex` is the dense region
    // grid index (rz*RY*RX + ry*RX + rx); the caller owns the region<->index math
    // and the grid dimensions, so this stays free of any voxel/glm dependency.
    // `regionCount` is the total number of regions (sizes the actor array on
    // first use). Out-of-range indices are ignored. No-op stub when PhysX is off.
    void SetColliderRegion(int regionIndex, int regionCount,
                           const std::vector<ColliderBox>& boxes);

    // Release the world-collider actors: the single legacy actor (SetWorldCollider)
    // AND every per-region actor (SetColliderRegion). No-op stub when off.
    void ClearWorldCollider();

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

    // --- Object-pooled bodies (PASS 3: chunk debris at scale) --------------
    //
    // The carve/chunk path creates and destroys MANY short-lived rigid bodies.
    // AcquireBox/ReleaseBox recycle a pre-allocated free-list of PxRigidDynamic
    // actors so per-carve churn never hits PxPhysics::create*/release. Reserve()
    // sizes the pool (== the live-debris cap); Acquire RESETS one parked actor
    // (resizes its box shape to the requested half-extents, sets pose +
    // linear/angular velocity, wakes it, adds it to the scene) and returns its
    // stable id; Release PARKS the actor (removes it from the scene, puts it on
    // the free list) WITHOUT releasing it. Ids are dense + stable for the
    // acquire->release lifetime and reused on the next Acquire.

    // Pre-allocate up to `count` pooled dynamic actors so later AcquireBox calls
    // recycle instead of allocating. Safe to call repeatedly (grows the pool to
    // `count`; never shrinks below the in-use high-water mark). No-op stub.
    void ReservePool(int count);

    // Acquire a pooled dynamic box with per-axis half-extents (hx,hy,hz) at
    // (x,y,z), initial linear velocity (vx,vy,vz) and angular velocity
    // (wx,wy,wz). Returns a stable body id, or -1 if the pool is exhausted /
    // stub build. Pair every AcquireBox with a ReleaseBox.
    int AcquireBox(float x, float y, float z, float hx, float hy, float hz,
                   float vx, float vy, float vz, float wx, float wy, float wz);

    // Park a pooled body (acquired via AcquireBox) back onto the free list.
    // No-op for an unknown id or in the stub build. The id is invalid until a
    // subsequent AcquireBox hands it back out.
    void ReleaseBox(int id);

    // Append the current world pose (position + orientation) of every live
    // dynamic body to |out| (cleared first). Empty in the stub build. The
    // chunk renderer uses each body's full transform to stamp its voxels.
    void EnumerateBodies(std::vector<BodyState>& out) const;

    // Look up one body's pose by id (added/acquired). Returns false (leaving
    // |out| untouched) for an unknown/parked id or in the stub build. Lets the
    // chunk renderer fetch a single chunk's transform without scanning.
    bool GetBodyState(int id, BodyState& out) const;

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
