// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "physics/PhysicsWorld.h"

#include "platform/Log.h"

#if defined(VOX_HAVE_PHYSX)

// ---------------------------------------------------------------------------
// Real NVIDIA PhysX 5 CPU rigid-body implementation (PASS 1 / M3).
//
// CPU-only path: PxDefaultCpuDispatcher, no GPU (so no PhysXGpu DLL needed).
// GPU rigid bodies + the enkiTS-backed dispatcher come in a later pass.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <vector>

#include <PxPhysicsAPI.h>

namespace vox::physics {

using namespace physx;

namespace {
// Fixed substep for deterministic settling in the smoke test. Step(dt) clamps
// to at most a handful of substeps so a large dt can't explode the sim.
constexpr float kFixedStep = 1.0f / 60.0f;
constexpr float kBoxHalfExtent = 0.5f;  // 1m cube
}  // namespace

struct PhysicsWorld::Impl {
    PxDefaultAllocator allocator;
    PxDefaultErrorCallback errorCallback;

    PxFoundation* foundation = nullptr;
    PxPhysics* physics = nullptr;
    PxDefaultCpuDispatcher* dispatcher = nullptr;
    PxScene* scene = nullptr;
    PxMaterial* material = nullptr;

    // Dense list of dynamic bodies in creation order; index == AddDynamicBox() /
    // AddBox() / AcquireBox() return value (the id). Bodies are owned by the
    // scene (or, for pooled+parked actors, by us); we keep raw pointers to read
    // transforms back. RemoveBody()/ReleaseBox() tombstone or park a slot
    // WITHOUT reindexing, so previously handed out ids stay valid for the
    // caller's per-body bookkeeping. `half` is the box half-extent so the voxel
    // renderer can size each debris cube.
    //
    // Pooling: a `pooled` body's actor is pre-created once and recycled. When
    // parked it is removed from the scene (not released) and `inScene` is false;
    // its slot index is pushed onto `freeList`. AcquireBox pops a free slot,
    // resizes its shape, re-poses it, re-adds it to the scene and returns the id.
    struct Body {
        PxRigidDynamic* actor = nullptr;
        float half = kBoxHalfExtent;
        bool  pooled = false;   // created via the pool (recycle, never release)
        bool  inScene = false;  // currently simulated (acquired) vs parked
    };
    std::vector<Body> dynamics;

    // Free list of parked pooled-body slot indices (into `dynamics`) available
    // for the next AcquireBox.
    std::vector<int> freeList;

    float accumulator = 0.0f;

    // Create a fresh pooled actor (1m unit box, parked: not added to the scene)
    // and append it to `dynamics`. Returns the new slot index, or -1 on failure.
    int CreatePooledActor();
    // Resize an existing dynamic body's single box shape to half-extents (hx,hy,hz).
    static void ResizeBox(PxRigidDynamic* body, float hx, float hy, float hz);
};

void PhysicsWorld::Init() {
    if (impl_) {
        return;  // already initialised
    }
    impl_ = new Impl();

    impl_->foundation =
        PxCreateFoundation(PX_PHYSICS_VERSION, impl_->allocator, impl_->errorCallback);
    if (!impl_->foundation) {
        vox::log::Error("physics: PxCreateFoundation failed");
        delete impl_;
        impl_ = nullptr;
        return;
    }

    // No PVD (debugger) connection in PASS 1; nullptr is fine.
    impl_->physics =
        PxCreatePhysics(PX_PHYSICS_VERSION, *impl_->foundation, PxTolerancesScale(), true, nullptr);
    if (!impl_->physics) {
        vox::log::Error("physics: PxCreatePhysics failed");
        Shutdown();
        return;
    }

    PxSceneDesc sceneDesc(impl_->physics->getTolerancesScale());
    sceneDesc.gravity = PxVec3(0.0f, -9.81f, 0.0f);

    impl_->dispatcher = PxDefaultCpuDispatcherCreate(2);
    sceneDesc.cpuDispatcher = impl_->dispatcher;
    sceneDesc.filterShader = PxDefaultSimulationFilterShader;

    impl_->scene = impl_->physics->createScene(sceneDesc);
    if (!impl_->scene) {
        vox::log::Error("physics: createScene failed");
        Shutdown();
        return;
    }

    // Default material (static & dynamic friction 0.5, restitution 0.1).
    impl_->material = impl_->physics->createMaterial(0.5f, 0.5f, 0.1f);

    vox::log::Info("physics: PhysX {} init (CPU rigid bodies)", PX_PHYSICS_VERSION_MAJOR);
}

void PhysicsWorld::Shutdown() {
    if (!impl_) {
        return;
    }
    // Release in reverse construction order. Releasing the scene drops the
    // actors it owns; but PARKED pooled actors are NOT in the scene, so the
    // scene won't free them -- release those explicitly first. Then clear the
    // (now dangling-once-the-scene-goes) raw pointers.
    for (auto& b : impl_->dynamics) {
        if (b.actor && b.pooled && !b.inScene) {
            b.actor->release();  // parked pooled actor: scene doesn't own it
            b.actor = nullptr;
        }
    }
    impl_->dynamics.clear();
    impl_->freeList.clear();
    if (impl_->scene) {
        impl_->scene->release();
        impl_->scene = nullptr;
    }
    if (impl_->dispatcher) {
        impl_->dispatcher->release();
        impl_->dispatcher = nullptr;
    }
    if (impl_->material) {
        impl_->material->release();
        impl_->material = nullptr;
    }
    if (impl_->physics) {
        impl_->physics->release();
        impl_->physics = nullptr;
    }
    if (impl_->foundation) {
        impl_->foundation->release();
        impl_->foundation = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
}

void PhysicsWorld::Step(float dt) {
    if (!impl_ || !impl_->scene || dt <= 0.0f) {
        return;
    }
    // Fixed-timestep accumulator. Cap the number of substeps per call so a huge
    // dt (e.g. after a hitch) cannot spiral into a long simulate loop.
    impl_->accumulator += dt;
    int substeps = 0;
    while (impl_->accumulator >= kFixedStep && substeps < 8) {
        impl_->scene->simulate(kFixedStep);
        impl_->scene->fetchResults(true);
        impl_->accumulator -= kFixedStep;
        ++substeps;
    }
}

unsigned PhysicsWorld::ActiveIslands() const {
    if (!impl_ || !impl_->scene) {
        return 0;
    }
    // Count awake dynamic actors -- a simple proxy for "active islands" that is
    // enough for the smoke test (settled bodies sleep and drop out).
    const PxU32 count =
        impl_->scene->getNbActors(PxActorTypeFlag::eRIGID_DYNAMIC);
    if (count == 0) {
        return 0;
    }
    std::vector<PxActor*> actors(count);
    impl_->scene->getActors(PxActorTypeFlag::eRIGID_DYNAMIC, actors.data(), count);
    unsigned active = 0;
    for (PxActor* a : actors) {
        auto* body = static_cast<PxRigidDynamic*>(a);
        if (body && !body->isSleeping()) {
            ++active;
        }
    }
    return active;
}

void PhysicsWorld::AddGroundPlane() {
    if (!impl_ || !impl_->scene || !impl_->physics || !impl_->material) {
        return;
    }
    // Plane through the origin with normal +Y -> ground at y = 0.
    PxRigidStatic* ground =
        PxCreatePlane(*impl_->physics, PxPlane(0.0f, 1.0f, 0.0f, 0.0f), *impl_->material);
    if (ground) {
        impl_->scene->addActor(*ground);
    }
}

int PhysicsWorld::AddDynamicBox(float x, float y, float z) {
    if (!impl_ || !impl_->scene || !impl_->physics || !impl_->material) {
        return -1;
    }
    PxRigidDynamic* body =
        impl_->physics->createRigidDynamic(PxTransform(PxVec3(x, y, z)));
    if (!body) {
        return -1;
    }
    PxBoxGeometry box(kBoxHalfExtent, kBoxHalfExtent, kBoxHalfExtent);
    PxShape* shape = impl_->physics->createShape(box, *impl_->material);
    if (shape) {
        body->attachShape(*shape);
        shape->release();  // body retains a reference
    }
    PxRigidBodyExt::updateMassAndInertia(*body, 10.0f);  // density 10
    impl_->scene->addActor(*body);

    const int idx = static_cast<int>(impl_->dynamics.size());
    impl_->dynamics.push_back({body, kBoxHalfExtent});
    return idx;
}

float PhysicsWorld::BodyY(int idx) const {
    if (!impl_ || idx < 0 || idx >= static_cast<int>(impl_->dynamics.size())) {
        return std::nanf("");
    }
    const Impl::Body& b = impl_->dynamics[static_cast<std::size_t>(idx)];
    if (!b.actor || (b.pooled && !b.inScene)) {
        return std::nanf("");  // tombstoned or parked pooled actor
    }
    return b.actor->getGlobalPose().p.y;
}

// --- Debris-on-carve helpers (PASS 2) --------------------------------------

int PhysicsWorld::AddBox(float x, float y, float z, float half, float vx, float vy,
                         float vz) {
    if (!impl_ || !impl_->scene || !impl_->physics || !impl_->material) {
        return -1;
    }
    if (half <= 0.0f) {
        half = kBoxHalfExtent;
    }
    PxRigidDynamic* body = impl_->physics->createRigidDynamic(PxTransform(PxVec3(x, y, z)));
    if (!body) {
        return -1;
    }
    PxBoxGeometry box(half, half, half);
    PxShape* shape = impl_->physics->createShape(box, *impl_->material);
    if (shape) {
        body->attachShape(*shape);
        shape->release();  // body retains a reference
    }
    PxRigidBodyExt::updateMassAndInertia(*body, 10.0f);  // density 10
    body->setLinearVelocity(PxVec3(vx, vy, vz));
    // A little spin makes the chunky debris read as tumbling even though the
    // voxel renderer ignores orientation -- cheap and harmless.
    body->setAngularVelocity(PxVec3(vy * 0.5f, vx * 0.5f, vz * 0.5f));
    impl_->scene->addActor(*body);

    const int idx = static_cast<int>(impl_->dynamics.size());
    impl_->dynamics.push_back({body, half, /*pooled=*/false, /*inScene=*/true});
    return idx;
}

// --- Object-pooled bodies (PASS 3) -----------------------------------------

void PhysicsWorld::Impl::ResizeBox(PxRigidDynamic* body, float hx, float hy,
                                   float hz) {
    if (!body || body->getNbShapes() == 0) {
        return;
    }
    PxShape* shape = nullptr;
    body->getShapes(&shape, 1);
    if (shape) {
        shape->setGeometry(PxBoxGeometry(hx, hy, hz));
    }
}

int PhysicsWorld::Impl::CreatePooledActor() {
    if (!scene || !physics || !material) {
        return -1;
    }
    // Parked off-screen far below the world; never simulated until acquired.
    PxRigidDynamic* body = physics->createRigidDynamic(PxTransform(PxVec3(0.0f, -1.0e6f, 0.0f)));
    if (!body) {
        return -1;
    }
    PxBoxGeometry box(kBoxHalfExtent, kBoxHalfExtent, kBoxHalfExtent);
    PxShape* shape = physics->createShape(box, *material);
    if (shape) {
        body->attachShape(*shape);
        shape->release();  // body retains a reference
    }
    PxRigidBodyExt::updateMassAndInertia(*body, 10.0f);  // density 10
    // NOT added to the scene: this is a parked free-list actor.
    const int idx = static_cast<int>(dynamics.size());
    dynamics.push_back({body, kBoxHalfExtent, /*pooled=*/true, /*inScene=*/false});
    freeList.push_back(idx);
    return idx;
}

void PhysicsWorld::ReservePool(int count) {
    if (!impl_ || count <= 0) {
        return;
    }
    // Count actors already owned by the pool (in-use + parked).
    int pooledTotal = 0;
    for (const auto& b : impl_->dynamics) {
        if (b.pooled) ++pooledTotal;
    }
    for (int i = pooledTotal; i < count; ++i) {
        if (impl_->CreatePooledActor() < 0) break;  // creation failed; stop growing
    }
}

int PhysicsWorld::AcquireBox(float x, float y, float z, float hx, float hy, float hz,
                             float vx, float vy, float vz, float wx, float wy, float wz) {
    if (!impl_ || !impl_->scene || !impl_->physics || !impl_->material) {
        return -1;
    }
    if (hx <= 0.0f) hx = kBoxHalfExtent;
    if (hy <= 0.0f) hy = kBoxHalfExtent;
    if (hz <= 0.0f) hz = kBoxHalfExtent;

    // Grow the pool by one if the free list is empty (CreatePooledActor pushes
    // the new slot onto freeList), then pop the parked slot to acquire it.
    if (impl_->freeList.empty()) {
        if (impl_->CreatePooledActor() < 0) return -1;
    }
    const int idx = impl_->freeList.back();
    impl_->freeList.pop_back();

    Impl::Body& b = impl_->dynamics[static_cast<std::size_t>(idx)];
    PxRigidDynamic* body = b.actor;
    if (!body) {
        return -1;
    }
    Impl::ResizeBox(body, hx, hy, hz);
    PxRigidBodyExt::updateMassAndInertia(*body, 10.0f);  // recompute for the new size
    body->setGlobalPose(PxTransform(PxVec3(x, y, z)));   // identity rotation at spawn
    body->setLinearVelocity(PxVec3(vx, vy, vz));
    body->setAngularVelocity(PxVec3(wx, wy, wz));
    // Use the largest half-extent as the renderer's nominal "half" (debris cube
    // sizing path); the chunk renderer reads the real per-axis box separately.
    b.half = std::max(hx, std::max(hy, hz));
    impl_->scene->addActor(*body);
    body->wakeUp();
    b.inScene = true;
    return idx;
}

void PhysicsWorld::ReleaseBox(int id) {
    if (!impl_ || id < 0 || id >= static_cast<int>(impl_->dynamics.size())) {
        return;
    }
    Impl::Body& b = impl_->dynamics[static_cast<std::size_t>(id)];
    if (!b.actor || !b.pooled || !b.inScene) {
        return;  // unknown / not pooled / already parked
    }
    if (impl_->scene) {
        impl_->scene->removeActor(*b.actor);
    }
    // Park it far below the world so a stray query never sees it near the scene.
    b.actor->setLinearVelocity(PxVec3(0.0f));
    b.actor->setAngularVelocity(PxVec3(0.0f));
    b.actor->setGlobalPose(PxTransform(PxVec3(0.0f, -1.0e6f, 0.0f)));
    b.inScene = false;
    impl_->freeList.push_back(id);
}

void PhysicsWorld::EnumerateBodies(std::vector<BodyState>& out) const {
    out.clear();
    if (!impl_) {
        return;
    }
    out.reserve(impl_->dynamics.size());
    for (std::size_t i = 0; i < impl_->dynamics.size(); ++i) {
        const Impl::Body& b = impl_->dynamics[i];
        PxRigidDynamic* body = b.actor;
        if (!body || (b.pooled && !b.inScene)) {
            continue;  // tombstoned slot or parked pooled actor
        }
        const PxTransform t = body->getGlobalPose();
        out.push_back(BodyState{static_cast<int>(i), t.p.x, t.p.y, t.p.z,
                                t.q.x, t.q.y, t.q.z, t.q.w});
    }
}

bool PhysicsWorld::GetBodyState(int id, BodyState& out) const {
    if (!impl_ || id < 0 || id >= static_cast<int>(impl_->dynamics.size())) {
        return false;
    }
    const Impl::Body& b = impl_->dynamics[static_cast<std::size_t>(id)];
    if (!b.actor || (b.pooled && !b.inScene)) {
        return false;  // tombstoned / parked
    }
    const PxTransform t = b.actor->getGlobalPose();
    out = BodyState{id, t.p.x, t.p.y, t.p.z, t.q.x, t.q.y, t.q.z, t.q.w};
    return true;
}

float PhysicsWorld::BodyPosY(int id) const {
    return BodyY(id);  // same dense-index lookup
}

void PhysicsWorld::RemoveBody(int id) {
    if (!impl_ || id < 0 || id >= static_cast<int>(impl_->dynamics.size())) {
        return;
    }
    PxRigidDynamic*& body = impl_->dynamics[static_cast<std::size_t>(id)].actor;
    if (!body) {
        return;  // already removed
    }
    if (impl_->scene) {
        impl_->scene->removeActor(*body);
    }
    body->release();
    body = nullptr;  // tombstone: keep the slot so other ids stay stable
}

void PhysicsWorld::ClearDynamics() {
    if (!impl_) {
        return;
    }
    // Non-pooled bodies are destroyed; POOLED bodies are parked (kept for reuse)
    // so the pool survives a world clear / reload. Rebuild the free list from the
    // parked pooled slots afterwards.
    impl_->freeList.clear();
    for (std::size_t i = 0; i < impl_->dynamics.size(); ++i) {
        Impl::Body& b = impl_->dynamics[i];
        if (!b.actor) {
            continue;
        }
        if (b.pooled) {
            if (b.inScene) {
                if (impl_->scene) impl_->scene->removeActor(*b.actor);
                b.actor->setLinearVelocity(PxVec3(0.0f));
                b.actor->setAngularVelocity(PxVec3(0.0f));
                b.actor->setGlobalPose(PxTransform(PxVec3(0.0f, -1.0e6f, 0.0f)));
                b.inScene = false;
            }
            impl_->freeList.push_back(static_cast<int>(i));  // parked: reusable
        } else {
            if (impl_->scene) impl_->scene->removeActor(*b.actor);
            b.actor->release();
            b.actor = nullptr;  // tombstone (keep slot so ids stay stable)
        }
    }
}

unsigned PhysicsWorld::BodyCount() const {
    if (!impl_) {
        return 0;
    }
    // Live = simulated bodies: non-pooled actors + acquired (in-scene) pooled
    // actors. Parked pooled actors on the free list do not count.
    unsigned n = 0;
    for (const auto& b : impl_->dynamics) {
        if (b.actor && (!b.pooled || b.inScene)) {
            ++n;
        }
    }
    return n;
}

}  // namespace vox::physics

#else  // !VOX_HAVE_PHYSX -------------------------------------------------------

// Logging stub: identical public API, no PhysX dependency. This is the default
// build the engine ships with until VOX_ENABLE_PHYSX is turned on.

#include <cmath>

namespace vox::physics {

struct PhysicsWorld::Impl {};

void PhysicsWorld::Init() { vox::log::Trace("physics: PhysX stub init"); }
void PhysicsWorld::Shutdown() {}
void PhysicsWorld::Step(float dt) { (void)dt; }
unsigned PhysicsWorld::ActiveIslands() const { return 0; }
void PhysicsWorld::AddGroundPlane() {}
int PhysicsWorld::AddDynamicBox(float x, float y, float z) {
    (void)x;
    (void)y;
    (void)z;
    return -1;
}
float PhysicsWorld::BodyY(int idx) const {
    (void)idx;
    return std::nanf("");
}

// Debris-on-carve API (PASS 2) -- stubbed identically so the public surface is
// the same in both builds; a future un-gated caller links cleanly here too.
int PhysicsWorld::AddBox(float x, float y, float z, float half, float vx, float vy,
                         float vz) {
    (void)x; (void)y; (void)z; (void)half; (void)vx; (void)vy; (void)vz;
    return -1;
}
void PhysicsWorld::ReservePool(int count) { (void)count; }
int PhysicsWorld::AcquireBox(float x, float y, float z, float hx, float hy, float hz,
                             float vx, float vy, float vz, float wx, float wy, float wz) {
    (void)x; (void)y; (void)z; (void)hx; (void)hy; (void)hz;
    (void)vx; (void)vy; (void)vz; (void)wx; (void)wy; (void)wz;
    return -1;
}
void PhysicsWorld::ReleaseBox(int id) { (void)id; }
void PhysicsWorld::EnumerateBodies(std::vector<BodyState>& out) const { out.clear(); }
bool PhysicsWorld::GetBodyState(int id, BodyState& out) const {
    (void)id; (void)out;
    return false;
}
float PhysicsWorld::BodyPosY(int id) const {
    (void)id;
    return std::nanf("");
}
void PhysicsWorld::RemoveBody(int id) { (void)id; }
void PhysicsWorld::ClearDynamics() {}
unsigned PhysicsWorld::BodyCount() const { return 0; }

}  // namespace vox::physics

#endif  // VOX_HAVE_PHYSX
