// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
//
// Voxhammer sandbox entry point. Boots the control plane (cvar registry,
// hardened web ConsoleServer), opens a window with a minimal DX12 clear, runs
// the keybindings system, and drives the engine loop: drain queued console
// work, render, broadcast telemetry -- until `quit` (or the window closes).

#include "VoxVersion.h"
#include "console/ConsoleServer.h"
#include "console/Password.h"
#include "cvar/Console.h"
#include "platform/Keybindings.h"
#include "platform/Log.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "ecs/EcsWorld.h"
#include "jobs/JobScheduler.h"
#include "physics/PhysicsWorld.h"
#include "render/Renderer.h"
#include "script/ScriptHost.h"
#include "voxel/VoxImport.h"
#include "voxel/VoxelWorld.h"

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Windows Shell API for bluenoise.open_folder — included last to avoid macro
// collisions (winuser.h defines MOD_SHIFT/MOD_ALT/MOD_CTRL which clash with
// vox::platform::MOD_* constants used above).
#include <windows.h>
#include <shellapi.h>
// Undefine the conflicting keyboard-modifier macros from winuser.h.
#ifdef MOD_SHIFT
#undef MOD_SHIFT
#endif
#ifdef MOD_CTRL
#undef MOD_CTRL
#endif
#ifdef MOD_ALT
#undef MOD_ALT
#endif

#ifndef VOX_VERSION_STRING
#define VOX_VERSION_STRING "0.0.0-dev"
#endif

namespace {
using namespace vox::console;
namespace pf = vox::platform;

std::atomic<bool> g_running{true};

void ParseRGB(const std::string& s, float& r, float& g, float& b) {
    r = g = b = 0.0f;
    std::sscanf(s.c_str(), "%f %f %f", &r, &g, &b);
}

// Build the default DESTRUCTIBLE demo scene into a VoxScene: a terrain
// heightfield + a floating sphere (mirrors the renderer's procedural
// GenerateScene), but as real VoxelWorld content so voxel.break / the X key
// can carve it. Materials: 1=grass 2=dirt 3=stone 4=sphere. The caller stamps
// this into the live world (which merges the palette so carving keeps colors).
void GenerateDemoScene(vox::voxel::VoxScene& vs) {
    vs.world.Init();
    vs.palette.fill(0u);
    // alpha byte = emission (0 = not emissive); RGB are sRGB bytes (shader decodes pow(c,2.2)).
    auto rgb = [](int r, int g, int b) -> std::uint32_t {
        return (static_cast<std::uint32_t>(b) << 16)
             | (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(r);
    };
    vs.palette[1] = rgb(173, 211, 148);  // grass
    vs.palette[2] = rgb(183, 160, 139);  // dirt
    vs.palette[3] = rgb(194, 196, 202);  // stone
    // Floating orb is EMISSIVE (alpha=56 -> emission ~1.75 at the default
    // gi.emissive=1): a glowing light that bleeds warm color onto the terrain
    // below via GI -- showcases emissive voxels. Set at a SANE value (the demo
    // is not a .vox, so the vox emissive boost does not touch it). Crank
    // renderer.gi.emissive for a blown-out sun, down to 0 to make it matte.
    vs.palette[4] = rgb(255, 150, 95) | (56u << 24);  // warm glowing orb

    const int   dim   = static_cast<int>(vox::voxel::kWorldDim);
    const float g     = static_cast<float>(dim);
    const float baseH = g * 0.34f, ampH = g * 0.125f;
    const float scx = g * 0.69f, scy = g * 0.66f, scz = g * 0.625f;
    const float sr2 = (g * 0.125f) * (g * 0.125f);
    std::uint32_t count = 0;
    for (int z = 0; z < dim; ++z)
        for (int x = 0; x < dim; ++x) {
            float h = baseH + ampH * std::sin(x * 0.20f) * std::cos(z * 0.18f);
            for (int y = 0; y < dim; ++y) {
                std::uint8_t m = 0;
                if (static_cast<float>(y) < h)
                    m = (static_cast<float>(y) > h - 1.5f) ? 1 : (static_cast<float>(y) > h - 5.0f ? 2 : 3);
                const float dx = static_cast<float>(x) - scx, dy = static_cast<float>(y) - scy, dz = static_cast<float>(z) - scz;
                if (dx * dx + dy * dy + dz * dz < sr2) m = 4;
                if (m) { vs.world.SetVoxel(x, y, z, m); ++count; }
            }
        }
    vs.sizeX = vs.sizeY = vs.sizeZ = static_cast<decltype(vs.sizeX)>(dim);
    vs.voxelCount = count;
}

#if defined(VOX_HAVE_PHYSX)
// ---------------------------------------------------------------------------
// Voxel-chunk fracture (Teardown-style). When the player carves, the REAL
// voxels removed from that volume are captured (their grid positions + ORIGINAL
// materials), grouped into a few rigid CHUNKS, and each chunk is spawned as a
// pooled PhysX box. Every frame each live chunk is re-stamped into the live GPU
// voxel grid at the body's FULL transform (position + rotation) so the actual
// shape + colors fall out and TUMBLE -- not generic cubes.
//
// Render invariant (unchanged from the placeholder): debris voxels live ONLY in
// the GPU grid, never in the VoxelWorld. The authoritative terrain at any cell
// is always world.GetVoxel(). To draw a chunk we overwrite the cells inside its
// rotated bounding box with either the chunk's voxel (if a cell maps inside the
// chunk's local solid) or the terrain (if not). To erase last frame's footprint
// we rewrite its previous bounding box with terrain. One EditVoxels per chunk.
//
// Rotated render uses INVERSE-SAMPLING: for every WORLD cell in the chunk's
// rotated AABB we inverse-transform its center into chunk-local space and sample
// the local solid array -- so a rotated chunk is gap-free (forward-rasterizing
// local->world leaves holes once rotated).
//
// PASS 2 scale-up: bodies are object-pooled (PhysicsWorld Acquire/ReleaseBox);
// the per-frame footprint clear+repaint is built in parallel across chunks via
// JobScheduler::ParallelFor, then the EditVoxels uploads are applied serially on
// the main thread (chunk AABBs overlap -> the shared mapped GPU buffer races).
// ---------------------------------------------------------------------------
class DebrisField {
public:
    // The debris material is no longer a single color: each chunk stores its
    // voxels' ORIGINAL materials. `debrisMaterial` is kept only as a fallback
    // for any captured cell whose material reads back as 0 (shouldn't happen).
    void Init(std::uint8_t debrisMaterial) { debrisMat_ = debrisMaterial; }

    // Live-chunk cap (voxel.debris.max) -- also sizes the PhysX body pool.
    void SetMaxLive(int maxLive) { maxLive_ = maxLive > 0 ? maxLive : 0; }
    // Chunk-size multiplier (voxel.debris.scale) -- dials how finely the carve
    // volume is partitioned into chunks (>1 = fewer/bigger chunks).
    void SetScale(float scale) { scale_ = scale > 0.0f ? scale : 0.0f; }

    // Pre-size the PhysX body pool to the live cap so spawns recycle actors.
    void ReservePool(vox::physics::PhysicsWorld& physics) {
        physics.ReservePool(maxLive_);
    }

    // Capture the solid voxels in the carve sphere (centered at the hit voxel,
    // Euclidean |radius|) BEFORE they are carved, partition them into a few
    // chunks, and spawn a pooled rigid body per chunk. `force`>0 + radial=true
    // gives an explosion (velocity points outward from the blast center scaled
    // by force); otherwise a gentle outward+upward scatter. MUST be called
    // before world.CarveSphere so world.GetVoxel still returns the solids.
    void SpawnFromCarve(vox::voxel::VoxelWorld& world, vox::physics::PhysicsWorld& physics,
                        int hx, int hy, int hz, int radius, float force, bool radial) {
        if (maxLive_ <= 0 || radius <= 0) return;
        const int G = static_cast<int>(vox::voxel::kWorldDim);
        const int x0 = std::max(0, hx - radius), y0 = std::max(0, hy - radius), z0 = std::max(0, hz - radius);
        const int x1 = std::min(G, hx + radius + 1), y1 = std::min(G, hy + radius + 1), z1 = std::min(G, hz + radius + 1);
        if (x0 >= x1 || y0 >= y1 || z0 >= z1) return;
        const float r2 = static_cast<float>(radius) * static_cast<float>(radius);

        // How many chunks along each axis: scale the carve box down by the chunk
        // multiplier so bigger scale -> fewer, chunkier pieces. At least 1 split,
        // capped so a huge crater doesn't make hundreds of chunks.
        const float targetChunkVox = std::max(2.0f, 4.0f * scale_);  // ~edge of one chunk
        auto splits = [&](int extent) {
            int n = static_cast<int>(std::lround(static_cast<float>(extent) / targetChunkVox));
            return std::clamp(n, 1, 4);
        };
        const int nx = splits(x1 - x0), ny = splits(y1 - y0), nz = splits(z1 - z0);

        // Blast center in world units (cell-centered).
        const float bcx = static_cast<float>(hx) + 0.5f;
        const float bcy = static_cast<float>(hy) + 0.5f;
        const float bcz = static_cast<float>(hz) + 0.5f;

        // For each sub-box (octant-like cell of the nx*ny*nz partition) gather the
        // solid voxels that fall inside the carve sphere into a local material grid.
        for (int sz = 0; sz < nz; ++sz)
        for (int sy = 0; sy < ny; ++sy)
        for (int sx = 0; sx < nx; ++sx) {
            if (static_cast<int>(live_.size()) >= maxLive_) return;  // cap reached
            const int sbx0 = x0 + (x1 - x0) * sx / nx, sbx1 = x0 + (x1 - x0) * (sx + 1) / nx;
            const int sby0 = y0 + (y1 - y0) * sy / ny, sby1 = y0 + (y1 - y0) * (sy + 1) / ny;
            const int sbz0 = z0 + (z1 - z0) * sz / nz, sbz1 = z0 + (z1 - z0) * (sz + 1) / nz;
            // Tight bounds of the SOLID-and-in-sphere voxels in this sub-box.
            int mnx = sbx1, mny = sby1, mnz = sbz1, mxx = sbx0 - 1, mxy = sby0 - 1, mxz = sbz0 - 1;
            for (int z = sbz0; z < sbz1; ++z)
            for (int y = sby0; y < sby1; ++y)
            for (int x = sbx0; x < sbx1; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - bcx;
                const float dy = static_cast<float>(y) + 0.5f - bcy;
                const float dz = static_cast<float>(z) + 0.5f - bcz;
                if (dx * dx + dy * dy + dz * dz > r2) continue;     // outside carve sphere
                if (world.GetVoxel(x, y, z) == 0) continue;          // not solid
                mnx = std::min(mnx, x); mny = std::min(mny, y); mnz = std::min(mnz, z);
                mxx = std::max(mxx, x); mxy = std::max(mxy, y); mxz = std::max(mxz, z);
            }
            if (mxx < mnx || mxy < mny || mxz < mnz) continue;  // no solids in this cell

            Chunk c;
            c.dx = mxx - mnx + 1; c.dy = mxy - mny + 1; c.dz = mxz - mnz + 1;
            c.mats.assign(static_cast<std::size_t>(c.dx) * c.dy * c.dz, 0u);
            c.solidCount = 0;
            for (int z = mnz; z <= mxz; ++z)
            for (int y = mny; y <= mxy; ++y)
            for (int x = mnx; x <= mxx; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - bcx;
                const float dy = static_cast<float>(y) + 0.5f - bcy;
                const float dz = static_cast<float>(z) + 0.5f - bcz;
                if (dx * dx + dy * dy + dz * dz > r2) continue;
                const std::uint8_t m = world.GetVoxel(x, y, z);
                if (!m) continue;
                const int lx = x - mnx, ly = y - mny, lz = z - mnz;
                c.mats[Idx(c, lx, ly, lz)] = m ? m : debrisMat_;
                ++c.solidCount;
            }
            if (c.solidCount == 0) continue;

            // Local box half-extents (world units == voxels). The body's center
            // of mass coincides with the chunk's local geometric center
            // localCenter = (dx/2, dy/2, dz/2); at spawn that maps to spawnCenter.
            c.localCx = 0.5f * static_cast<float>(c.dx);
            c.localCy = 0.5f * static_cast<float>(c.dy);
            c.localCz = 0.5f * static_cast<float>(c.dz);
            // Spawn the body so the chunk's voxels initially coincide with the
            // grid cells they were captured from: world center of the local box
            // = (min corner) + localCenter.
            const float scx = static_cast<float>(mnx) + c.localCx;
            const float scy = static_cast<float>(mny) + c.localCy;
            const float scz = static_cast<float>(mnz) + c.localCz;

            // Velocity: radial blast or gentle scatter, computed from the chunk's
            // offset from the blast center.
            float ox = scx - bcx, oy = scy - bcy, oz = scz - bcz;
            float vx, vy, vz;
            if (radial && force > 0.0f) {
                float dx = ox, dy = oy + 0.5f, dz = oz;  // bias up a touch
                const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (len > 1e-4f) { dx /= len; dy /= len; dz /= len; }
                else { dx = Frand(-1.0f, 1.0f); dy = 1.0f; dz = Frand(-1.0f, 1.0f); }
                const float mag = force * Frand(0.6f, 1.0f);
                vx = dx * mag;
                vy = dy * mag + Frand(1.0f, 4.0f);
                vz = dz * mag;
            } else {
                vx = ox * 1.5f + Frand(-2.0f, 2.0f);
                vy = Frand(3.0f, 8.0f);
                vz = oz * 1.5f + Frand(-2.0f, 2.0f);
            }
            // A healthy random spin so chunks tumble visibly.
            const float wx = Frand(-4.0f, 4.0f), wy = Frand(-4.0f, 4.0f), wz = Frand(-4.0f, 4.0f);

            const int id = physics.AcquireBox(scx, scy, scz, c.localCx, c.localCy, c.localCz,
                                              vx, vy, vz, wx, wy, wz);
            if (id < 0) return;  // pool exhausted
            c.id = id;
            live_.push_back(std::move(c));
        }
    }

    // Per-frame: cull dead chunks (clearing their last footprint), then re-stamp
    // each live chunk at its body's full transform. `jobs` parallelizes the
    // per-chunk region build; uploads are serialized on the calling (main) thread.
    void Update(vox::voxel::VoxelWorld& world, vox::physics::PhysicsWorld& physics,
                vox::render::Renderer& renderer, vox::jobs::JobScheduler& jobs, float dt) {
        // 1) Cull chunks that fell below the kill plane or aged out. Build their
        //    "clear previous footprint to terrain" work, then release the body.
        for (auto it = live_.begin(); it != live_.end();) {
            it->age += dt;
            const float y = physics.BodyPosY(it->id);
            const bool dead = !std::isfinite(y) || y < kKillY || it->age > kTtlSeconds;
            if (dead) {
                if (it->hasPrev) ClearBoxToTerrain(world, renderer, it->prev);
                physics.ReleaseBox(it->id);
                it = live_.erase(it);
            } else {
                ++it;
            }
        }
        if (live_.empty()) return;

        // 2) Read live poses; compute each chunk's current world AABB.
        physics.EnumerateBodies(states_);
        jobItems_.clear();
        jobItems_.reserve(live_.size());
        for (std::size_t i = 0; i < live_.size(); ++i) {
            Chunk& t = live_[i];
            vox::physics::BodyState st;
            if (!FindState(states_, t.id, st)) continue;  // not live this frame
            t.pose = st;
            t.cur = WorldAabb(t, st);
            jobItems_.push_back(&t);
        }
        if (jobItems_.empty()) return;

        // 3) PARALLEL: build each chunk's region buffer (the rotated inverse-sample
        //    rasterization over its current world AABB) off the main thread. Each
        //    job writes only its own chunk's buffer, so there is no data race here.
        jobs.ParallelFor(static_cast<std::uint32_t>(jobItems_.size()),
            [this, &world](std::uint32_t begin, std::uint32_t end) {
                for (std::uint32_t k = begin; k < end; ++k) {
                    Chunk& t = *jobItems_[k];
                    BuildChunkRegion(world, t);
                }
            });

        // 4) SERIAL (main thread): apply uploads. Chunk world-AABBs can overlap,
        //    so writing the shared persistently-mapped GPU buffer must not race.
        //    First clear each chunk's PREVIOUS footprint that its new AABB no
        //    longer covers (restore terrain), then paint the freshly built region.
        for (Chunk* tp : jobItems_) {
            Chunk& t = *tp;
            if (t.hasPrev) ClearBoxExcept(world, renderer, t.prev, t.cur);
            if (!EmptyBox(t.cur)) {
                renderer.EditVoxels(t.cur.x0, t.cur.y0, t.cur.z0, t.cur.x1, t.cur.y1, t.cur.z1,
                                    t.region.data(), nullptr);
            }
            t.prev = t.cur;
            t.hasPrev = true;
        }
    }

    // Clear every chunk's cells (e.g. on world clear/reload) and forget chunks.
    void ClearAll(vox::voxel::VoxelWorld& world, vox::physics::PhysicsWorld& physics,
                  vox::render::Renderer& renderer) {
        for (auto& t : live_)
            if (t.hasPrev) ClearBoxToTerrain(world, renderer, t.prev);
        physics.ClearDynamics();  // parks pooled bodies (keeps the pool)
        live_.clear();
    }

    bool Empty() const { return live_.empty(); }

private:
    static constexpr float kKillY      = -2.0f;   // cull below this world Y
    static constexpr float kTtlSeconds = 12.0f;   // max chunk lifetime

    struct Aabb { int x0, y0, z0, x1, y1, z1; };  // exclusive max

    // A captured fracture chunk: a local solid+material grid of dims dx*dy*dz,
    // its geometric center (localCx,localCy,localCz) in local voxel space (the
    // physics body's center of mass), the pooled body id, the per-frame world
    // AABB bookkeeping and the parallel-built region upload buffer.
    struct Chunk {
        int dx = 0, dy = 0, dz = 0;
        std::vector<std::uint8_t> mats;             // dx*dy*dz, 0 = empty
        std::uint32_t solidCount = 0;
        float localCx = 0.f, localCy = 0.f, localCz = 0.f;
        int id = -1;
        float age = 0.0f;                            // seconds alive (TTL / kill-plane cull)
        bool hasPrev = false;
        Aabb prev{};                                 // last frame's painted AABB
        Aabb cur{};                                  // this frame's AABB
        vox::physics::BodyState pose{};              // this frame's transform
        std::vector<std::uint32_t> region;           // built in parallel
    };

    std::uint8_t debrisMat_ = 0;
    int          maxLive_   = 256;    // voxel.debris.max (live-chunk cap + pool size)
    float        scale_     = 1.0f;   // voxel.debris.scale (chunk partition coarseness)
    std::vector<Chunk> live_;
    std::vector<Chunk*> jobItems_;    // chunks active THIS frame (parallel set)
    std::vector<vox::physics::BodyState> states_;
    std::mt19937 rng_{0xC0FFEEu};

    float Frand(float lo, float hi) {
        std::uniform_real_distribution<float> d(lo, hi);
        return d(rng_);
    }

    static std::size_t Idx(const Chunk& c, int lx, int ly, int lz) {
        return static_cast<std::size_t>(lz) * c.dx * c.dy
             + static_cast<std::size_t>(ly) * c.dx + static_cast<std::size_t>(lx);
    }

    static bool FindState(const std::vector<vox::physics::BodyState>& v, int id,
                          vox::physics::BodyState& out) {
        for (const auto& s : v) if (s.id == id) { out = s; return true; }
        return false;
    }

    static bool EmptyBox(const Aabb& a) { return a.x0 >= a.x1 || a.y0 >= a.y1 || a.z0 >= a.z1; }

    // Rotate vector v by quaternion q (x,y,z,w). Standard q*v*conj(q).
    static void QuatRotate(const vox::physics::BodyState& q, float vx, float vy, float vz,
                           float& ox, float& oy, float& oz) {
        // t = 2 * cross(q.xyz, v); out = v + q.w*t + cross(q.xyz, t)
        const float tx = 2.0f * (q.qy * vz - q.qz * vy);
        const float ty = 2.0f * (q.qz * vx - q.qx * vz);
        const float tz = 2.0f * (q.qx * vy - q.qy * vx);
        ox = vx + q.qw * tx + (q.qy * tz - q.qz * ty);
        oy = vy + q.qw * ty + (q.qz * tx - q.qx * tz);
        oz = vz + q.qw * tz + (q.qx * ty - q.qy * tx);
    }
    // Rotate by the CONJUGATE (inverse) of q -- world delta -> local delta.
    static void QuatRotateInv(const vox::physics::BodyState& q, float vx, float vy, float vz,
                              float& ox, float& oy, float& oz) {
        // conj(q) negates the xyz part.
        const float tx = 2.0f * (-q.qy * vz + q.qz * vy);
        const float ty = 2.0f * (-q.qz * vx + q.qx * vz);
        const float tz = 2.0f * (-q.qx * vy + q.qy * vx);
        ox = vx + q.qw * tx + (-q.qy * tz + q.qz * ty);
        oy = vy + q.qw * ty + (-q.qz * tx + q.qx * tz);
        oz = vz + q.qw * tz + (-q.qx * ty + q.qy * tx);
    }

    // World-space integer AABB (clamped, exclusive max) covering the chunk's
    // rotated local box. Rotate the 8 corners of [0,dx]x[0,dy]x[0,dz] (offset by
    // -localCenter) by the body quat, add the body position, bound, floor/ceil,
    // pad by 1 cell so the inverse-sample never clips an edge voxel.
    static Aabb WorldAabb(const Chunk& c, const vox::physics::BodyState& st) {
        const int G = static_cast<int>(vox::voxel::kWorldDim);
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        const float ex[2] = {-c.localCx, c.localCx};  // local box corner offsets
        const float ey[2] = {-c.localCy, c.localCy};
        const float ez[2] = {-c.localCz, c.localCz};
        for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
        for (int k = 0; k < 2; ++k) {
            float rx, ry, rz;
            QuatRotate(st, ex[i], ey[j], ez[k], rx, ry, rz);
            const float wx = st.px + rx, wy = st.py + ry, wz = st.pz + rz;
            lo[0] = std::min(lo[0], wx); hi[0] = std::max(hi[0], wx);
            lo[1] = std::min(lo[1], wy); hi[1] = std::max(hi[1], wy);
            lo[2] = std::min(lo[2], wz); hi[2] = std::max(hi[2], wz);
        }
        Aabb a;
        a.x0 = static_cast<int>(std::floor(lo[0])) - 1;
        a.y0 = static_cast<int>(std::floor(lo[1])) - 1;
        a.z0 = static_cast<int>(std::floor(lo[2])) - 1;
        a.x1 = static_cast<int>(std::ceil(hi[0])) + 1;
        a.y1 = static_cast<int>(std::ceil(hi[1])) + 1;
        a.z1 = static_cast<int>(std::ceil(hi[2])) + 1;
        a.x0 = std::clamp(a.x0, 0, G); a.y0 = std::clamp(a.y0, 0, G); a.z0 = std::clamp(a.z0, 0, G);
        a.x1 = std::clamp(a.x1, 0, G); a.y1 = std::clamp(a.y1, 0, G); a.z1 = std::clamp(a.z1, 0, G);
        return a;
    }

    // Build chunk t's `region` buffer for its current AABB t.cur via inverse
    // sampling: for each WORLD cell center, inverse-transform to chunk-local
    // space, locate the containing local voxel; if solid write its ORIGINAL
    // material, else write the authoritative terrain. Pure read of `world` +
    // write to t.region only -> safe to run in parallel across chunks.
    void BuildChunkRegion(const vox::voxel::VoxelWorld& world, Chunk& t) const {
        const Aabb& a = t.cur;
        t.region.clear();
        if (EmptyBox(a)) return;
        const std::size_t n = static_cast<std::size_t>(a.x1 - a.x0) * (a.y1 - a.y0) * (a.z1 - a.z0);
        t.region.resize(n);
        std::size_t w = 0;
        for (int z = a.z0; z < a.z1; ++z)
        for (int y = a.y0; y < a.y1; ++y)
        for (int x = a.x0; x < a.x1; ++x, ++w) {
            // World cell center -> delta from body origin -> inverse-rotate ->
            // shift by localCenter to get the continuous local position.
            const float wx = static_cast<float>(x) + 0.5f - t.pose.px;
            const float wy = static_cast<float>(y) + 0.5f - t.pose.py;
            const float wz = static_cast<float>(z) + 0.5f - t.pose.pz;
            float lx, ly, lz;
            QuatRotateInv(t.pose, wx, wy, wz, lx, ly, lz);
            lx += t.localCx; ly += t.localCy; lz += t.localCz;
            // Containing local voxel = floor of the continuous local coordinate.
            const int ix = static_cast<int>(std::floor(lx));
            const int iy = static_cast<int>(std::floor(ly));
            const int iz = static_cast<int>(std::floor(lz));
            std::uint8_t m = 0;
            if (ix >= 0 && ix < t.dx && iy >= 0 && iy < t.dy && iz >= 0 && iz < t.dz) {
                m = t.mats[Idx(t, ix, iy, iz)];
            }
            t.region[w] = m ? static_cast<std::uint32_t>(m)
                            : static_cast<std::uint32_t>(world.GetVoxel(x, y, z));
        }
    }

    // Clear `a`'s cells back to the authoritative terrain (cull / world clear).
    void ClearBoxToTerrain(const vox::voxel::VoxelWorld& world, vox::render::Renderer& renderer,
                           const Aabb& a) {
        if (EmptyBox(a)) return;
        scratch_.clear();
        scratch_.reserve(static_cast<std::size_t>(a.x1 - a.x0) * (a.y1 - a.y0) * (a.z1 - a.z0));
        for (int z = a.z0; z < a.z1; ++z)
            for (int y = a.y0; y < a.y1; ++y)
                for (int x = a.x0; x < a.x1; ++x)
                    scratch_.push_back(static_cast<std::uint32_t>(world.GetVoxel(x, y, z)));
        renderer.EditVoxels(a.x0, a.y0, a.z0, a.x1, a.y1, a.z1, scratch_.data(), nullptr);
    }

    // Restore the previous footprint to terrain. The paint step (which uploads
    // the full `keep`=cur region right after, in the same frame) overwrites the
    // prev/cur overlap, so clearing the WHOLE prev box to terrain is correct --
    // cells still under the chunk get terrain then are immediately repainted
    // before present (no visible flash). Fast-path: if prev is fully inside cur,
    // the paint step covers all of it, so there's nothing to clear.
    void ClearBoxExcept(const vox::voxel::VoxelWorld& world, vox::render::Renderer& renderer,
                        const Aabb& a, const Aabb& keep) {
        if (EmptyBox(a)) return;
        if (a.x0 >= keep.x0 && a.x1 <= keep.x1 && a.y0 >= keep.y0 && a.y1 <= keep.y1 &&
            a.z0 >= keep.z0 && a.z1 <= keep.z1) {
            return;  // prev fully covered by the upcoming paint of cur
        }
        ClearBoxToTerrain(world, renderer, a);
    }

    std::vector<std::uint32_t> scratch_;  // serial-only (main-thread) upload buffer
};
#endif  // VOX_HAVE_PHYSX

void Usage() {
    vox::log::Info("Voxhammer {} -- standalone DX12 voxel-destruction engine", VOX_VERSION_STRING);
    vox::log::Info("Usage: voxhammer [flags]");
    vox::log::Info("  --remote-password=<pw>  Hash (argon2id) + store remote.password_hash, then run.");
    vox::log::Info("  --reset-cert            Regenerate the self-signed TLS cert before binding.");
    vox::log::Info("  --remote / --no-remote  Force the ConsoleServer on / off.");
    vox::log::Info("  --no-window             Run headless (no GLFW window / DX12).");
    vox::log::Info("  --cvars=<path>          Override the cvars file (default: <userdata>/cvars.toml).");
    vox::log::Info("  --vsync=on|off          Override renderer.vsync for this run.");
    vox::log::Info("  --version / --help");
}

void RegisterCoreCvars() {
    Console& c = Console::Get();
    auto reg = [&](const char* n, const char* d, const char* desc, CVarParams p) { c.RegisterCVar(n, d, desc, std::move(p)); };
    reg("renderer.gi.bounces", "1", "QUALITY GI path depth: 0 = direct only (no indirect fill, dramatic dark shadows); 1 = clean single bounce (default); 2-5 fills enclosed rooms but is noisier.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 5, .range_step = 1});
    reg("renderer.gi.emissive", "1.0", "Global emissive multiplier (scales ALL emitters incl. the demo orb). Keep ~1 for sane values; use renderer.vox.emissive_boost to brighten loaded .vox maps instead.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 8.0f, .range_step = 0.25f});
    reg("renderer.vox.emissive_boost", "8.0", "LIVE emissive multiplier for LOADED .vox maps only (MagicaVoxel _emit is dim; crank high to flood-light a room, 1 = raw). Demo/code emissive is unaffected. Uncapped.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 256.0f, .range_step = 4.0f});
    reg("renderer.emissive.surface", "1.0", "How bright an emitter's OWN surface looks when viewed directly (1 = full glow; lower so a strong room-light emitter shows its color instead of blowing to white). Does NOT change how much it lights the scene.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 1.0f, .range_step = 0.05f});
    reg("renderer.gi.intensity", "1.0", "QUALITY indirect-GI strength: scales the sky-fill + surface inter-reflection bounce. >1 brightens dark enclosed rooms (skylight barely reaches deep interiors); 1 = physical. Artistic dial.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 32.0f, .range_step = 0.5f});
    reg("renderer.gi.restir.spatial_passes", "2", "ReSTIR GI spatial resampling passes.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 4, .range_step = 1});
    reg("renderer.upscaling.mode", "DLSS_Q", "Super-resolution / upscaler preset.", {.type = CVarType::Enum, .flags = CVAR_ARCHIVE, .enum_values = {"DLSS_DLAA", "DLSS_Q", "DLSS_B", "DLSS_P", "DLSS_UP", "FSR_Q", "FSR_B", "FSR_P", "NATIVE"}});
    reg("renderer.frame_gen.factor", "OFF", "Frame-generation multiplier.", {.type = CVarType::Enum, .flags = CVAR_ARCHIVE, .enum_values = {"OFF", "2X", "3X", "4X"}});
    reg("renderer.hdr.enabled", "0", "HDR10 output (auto-detected display).", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.hdr.peak_nits", "1000", "HDR peak luminance target.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 400, .range_max = 4000, .range_step = 50});
    reg("renderer.debug.clear_color", "0.05 0.05 0.08", "Swapchain clear color (RGB 0..1).", {.type = CVarType::Color, .flags = CVAR_ARCHIVE});
    reg("renderer.vsync", "1", "Vertical sync.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.exposure", "1.0", "Render exposure (pre-tonemap multiplier).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.1f, .range_max = 4.0f, .range_step = 0.05f});
    reg("renderer.sun.azimuth", "0.7", "Sun azimuth (radians).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 6.2832f, .range_step = 0.02f});
    reg("renderer.sun.elevation", "0.6", "Sun elevation (radians; lower = longer shadows).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.1f, .range_max = 1.5f, .range_step = 0.02f});
    reg("renderer.ambient", "0.28", "Ambient fill light (lower = punchier shadows).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 1.0f, .range_step = 0.01f});
    reg("renderer.shadow.softness", "0.08", "Soft-shadow penumbra half-angle (radians; 0 = hard).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 0.5f, .range_step = 0.01f});
    reg("renderer.ao.strength", "0.55", "Ambient-occlusion darkening (0 = off).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 1.0f, .range_step = 0.01f});
    reg("renderer.ao.radius", "4.0", "Ambient-occlusion ray reach (voxels).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 1.0f, .range_max = 24.0f, .range_step = 1.0f});
    reg("renderer.lighting.mode", "PERFORMANCE", "Lighting tier: PERFORMANCE (raymarch + AO, high FPS) or QUALITY (path-traced GI; accumulates while the view is still).", {.type = CVarType::Enum, .flags = CVAR_ARCHIVE, .enum_values = {"PERFORMANCE", "QUALITY"}});
    reg("renderer.dither", "1", "Triangular dither before 8-bit output (hides gradient banding).", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.ao.samples", "8", "Ambient-occlusion rays per pixel.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 32, .range_step = 1});
    reg("renderer.shadow.samples", "12", "Soft-shadow penumbra rays per pixel.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 16, .range_step = 1});
    reg("renderer.gi.samples", "1", "QUALITY: indirect GI bounce samples accumulated per frame.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 8, .range_step = 1});
    reg("renderer.gi.denoise", "1", "QUALITY: temporal denoise so path-traced GI stays clean while the camera moves (slight ghosting trade-off).", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.gi.reproject", "1", "QUALITY: temporally REPROJECT GI history across camera motion (realigns each pixel to the same world point) so motion stays sharp with little ghosting. Off = screen-space EMA only.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.gi.reproject_history", "8", "QUALITY+reproject: max accumulated GI samples kept DURING camera motion (the swim<->grain knob). Higher = smoother but more swimming/lag; lower = crisper/more responsive but grainier. Default = min (crisp, no swim) -- raise it once the spatial denoiser is on. Still frames always converge fully.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 8, .range_max = 1024, .range_step = 8});
    reg("renderer.gi.denoiser", "NONE", "QUALITY GI denoiser. NONE = single-pass (no spatial filter, zero extra cost). ATROUS = multi-pass edge-aware a-trous spatial denoise. SVGF = a-trous + per-pixel variance. ATROUS/SVGF let you run reproject_history LOW (no swim) while the spatial filter removes the grain.", {.type = CVarType::Enum, .flags = CVAR_ARCHIVE, .enum_values = {"NONE", "ATROUS", "SVGF"}});
    reg("renderer.gi.atrous_iters", "5", "Denoiser (ATROUS/SVGF): edge-aware a-trous wavelet iterations; more = wider/smoother filter (step doubles each iter).", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 6, .range_step = 1});
    reg("renderer.gi.denoise_phi_normal", "64.0", "Denoiser (ATROUS/SVGF): normal edge-stop exponent; higher preserves normal/voxel-face edges more sharply.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 1.0f, .range_max = 256.0f, .range_step = 1.0f});
    reg("renderer.gi.denoise_phi_depth", "1.0", "Denoiser (ATROUS/SVGF): depth/position edge-stop scale; smaller = more sensitive to depth silhouettes (less bleed across them).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.05f, .range_max = 8.0f, .range_step = 0.05f});
    reg("renderer.gi.denoise_phi_lum", "4.0", "Denoiser (ATROUS/SVGF): luminance edge-stop sigma (grain<->detail balance; SVGF scales it by sqrt(variance)).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.1f, .range_max = 32.0f, .range_step = 0.1f});
    reg("renderer.gbuffer.obb", "0", "EXPERIMENTAL (Milestone A1): draw the deferred G-buffer pass by OBB-rasterizing the world object + fragment DDA instead of the proven full-screen PSShade. OFF (default) = the verified A0 path; ON = the A1 path (under verification). Only affects QUALITY + a denoiser (ATROUS/SVGF).", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.empty_space_skip", "1", "Skip empty bricks in the voxel raymarch (faster; visually identical). Off = per-voxel DDA.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.gi.debug", "0", "QUALITY debug: show ONLY the indirect GI bounce (no direct/albedo) to confirm GI is contributing.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("physics.gpu_rigids.enabled", "1", "GPU rigid bodies (NVIDIA).", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("physics.gpu_rigids.max_islands", "10000", "Max active dynamic islands.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 256, .range_max = 16384, .range_step = 256});
    reg("physics.solver.position_iters", "8", "Solver position iterations.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 32, .range_step = 1});
    reg("sim.fluid.grid_resolution", "MED", "FLIP/Eulerian fluid grid resolution.", {.type = CVarType::Enum, .flags = CVAR_ARCHIVE, .enum_values = {"LOW", "MED", "HIGH", "ULTRA"}});
    reg("sim.fire.combustion_rate", "1.0", "Combustion rate multiplier.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.1f, .range_max = 5.0f, .range_step = 0.1f});
    reg("voxel.streaming.horizon_meters", "256", "Voxel streaming horizon.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 64.0f, .range_max = 512.0f, .range_step = 8.0f});
    reg("voxel.lod.aggressive_eviction", "0", "Aggressively evict distant chunks.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("voxel.import_path", "", "MagicaVoxel .vox file to load at startup (empty = procedural demo scene).", {.type = CVarType::String, .flags = CVAR_ARCHIVE});
    reg("voxel.cursor.x", "0", "3D cursor X for voxel.place (0..127).", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 127, .range_step = 1});
    reg("voxel.cursor.y", "0", "3D cursor Y for voxel.place (0..127).", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 127, .range_step = 1});
    reg("voxel.cursor.z", "0", "3D cursor Z for voxel.place (0..127).", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 127, .range_step = 1});
    reg("voxel.break_radius", "3", "Carve radius (voxels) for voxel.break.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 16, .range_step = 1});
    reg("voxel.explode_radius", "8", "Carve radius (voxels) for voxel.explode (a big blast crater).", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 32, .range_step = 1});
    reg("voxel.explode_force", "12.0", "Radial blast velocity (units/sec) for voxel.explode debris -- chunks fly outward from the blast center.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 64.0f, .range_step = 1.0f});
    reg("voxel.debris.max", "256", "Max live debris bodies (carve + explode). Spawning is skipped at the cap to bound physics/render cost.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 0, .range_max = 4096, .range_step = 16});
    reg("voxel.debris.scale", "1.0", "Debris chunk-size multiplier (dials chunkiness; 1 = stock 1..4-voxel cubes).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.25f, .range_max = 4.0f, .range_step = 0.25f});
    reg("audio.master_volume", "0.8", "Master output volume.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 1.0f, .range_step = 0.01f});
    reg("camera.pos", "32 40 -24", "Free-fly camera position (world units).", {.type = CVarType::Vec3, .flags = CVAR_ARCHIVE});
    reg("camera.yaw", "0.0", "Camera yaw (radians).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = -3.1416f, .range_max = 3.1416f, .range_step = 0.02f});
    reg("camera.pitch", "-0.5", "Camera pitch (radians).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = -1.5f, .range_max = 1.5f, .range_step = 0.02f});
    reg("camera.fov", "1.2", "Camera vertical FOV (radians).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.4f, .range_max = 2.4f, .range_step = 0.02f});
    reg("camera.invert_x", "1", "Invert horizontal mouse-look.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("camera.invert_y", "0", "Invert vertical mouse-look.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("camera.move_speed", "15", "Fly-cam move speed (units/sec).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 1.0f, .range_max = 120.0f, .range_step = 1.0f});
    reg("camera.boost", "3.0", "Fly-cam Shift speed multiplier.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 1.0f, .range_max = 10.0f, .range_step = 0.5f});
    reg("camera.sensitivity", "0.0025", "Mouse-look sensitivity (rad/px).", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0005f, .range_max = 0.01f, .range_step = 0.0005f});
    reg("editor.active", "0", "Editor mode (vs play).", {.type = CVarType::Bool});
    reg("debug.show_brick_grid", "0", "Overlay the brick grid.", {.type = CVarType::Bool, .flags = CVAR_DEVELOPER});
    reg("debug.show_physx_wireframe", "0", "Overlay PhysX collision wireframe.", {.type = CVarType::Bool, .flags = CVAR_DEVELOPER});
    reg("debug.pause_simulation", "0", "Pause the simulation.", {.type = CVarType::Bool, .flags = CVAR_CHEAT});
    reg("profiling.tracy_enabled", "0", "Stream to the Tracy profiler.", {.type = CVarType::Bool});
    reg("remote.port_ws", "27960", "WebSocket/HTTPS port.", {.type = CVarType::Int, .flags = CVAR_READONLY});
    reg("remote.port_tcp", "27961", "Scripted TCP port.", {.type = CVarType::Int, .flags = CVAR_READONLY});
    reg("remote.session_ttl_seconds", "3600", "Console session idle TTL.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 300, .range_max = 86400, .range_step = 60});
    reg("remote.password_hash", "", "argon2id hash of the remote console password.", {.type = CVarType::String, .flags = CVAR_ARCHIVE | CVAR_READONLY});
    reg("renderer.screenshot.format", "PNG", "Screenshot file format.", {.type = CVarType::Enum, .flags = CVAR_ARCHIVE, .enum_values = {"PNG", "BMP"}});
}

// "Ctrl+Shift+F5" -> (key="F5", mods). Single token (no spaces).
void ParseKeySpec(const std::string& spec, std::string& key, std::uint32_t& mods) {
    mods = pf::MOD_NONE;
    std::string cur;
    std::vector<std::string> parts;
    for (char ch : spec) {
        if (ch == '+') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(ch);
    }
    parts.push_back(cur);
    key = parts.empty() ? "" : parts.back();
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == "Ctrl") mods |= pf::MOD_CTRL;
        else if (parts[i] == "Shift") mods |= pf::MOD_SHIFT;
        else if (parts[i] == "Alt") mods |= pf::MOD_ALT;
    }
}
std::string KeySpec(const std::string& key, std::uint32_t mods) {
    std::string s;
    for (auto& m : pf::ModNames(mods)) s += m + "+";
    return s + key;
}

void RegisterCoreCommands(ConsoleServer& server, pf::Keybindings& kb) {
    Console& c = Console::Get();
    auto outln = [](Output& o, std::string_view s) { o.PrintLine(s); };

    c.RegisterCommand("version", "Print engine version.", [](std::span<const std::string_view>, Output& o) { o.Print(VOX_VERSION_STRING); });
    c.RegisterCommand("quit", "Quit the engine.", [](std::span<const std::string_view>, Output& o) { g_running = false; o.Print("quitting"); });
    c.RegisterCommand("toggle", "Toggle a boolean cvar.", [](std::span<const std::string_view> a, Output& o) {
        if (a.empty()) { o.Print("usage: toggle <cvar>"); return; }
        CVar* cv = Console::Get().FindCVar(a[0]);
        if (!cv) { o.Format("unknown cvar: {}", a[0]); return; }
        std::string nv = cv->GetBool() ? "0" : "1";
        Console::Get().Execute(std::string(a[0]) + " " + nv);
    });
    c.RegisterCommand("pix_capture_next_frame", "Trigger a PIX GPU capture (stub).", [](std::span<const std::string_view>, Output& o) { o.Print("pix: stub"); });
    c.RegisterCommand("reload_scene", "Reload the current scene (stub).", [](std::span<const std::string_view>, Output& o) { o.Print("reload_scene: stub"); });
    // physics.dump_islands is re-registered later (after `physics` is constructed) so it can
    // report PhysicsWorld::ActiveIslands() instead of a hardcoded stub.
    c.RegisterCommand("voxel.dump_chunks", "Dump resident voxel chunks (stub).", [](std::span<const std::string_view>, Output& o) { o.Print("0 chunks resident"); });
    c.RegisterCommand("setcursor", "Snap the voxel placement cursor (voxel.cursor.*) to the current camera position.", [](std::span<const std::string_view>, Output& o) {
        Console& cc = Console::Get();
        float p[3] = {0.0f, 0.0f, 0.0f};
        if (CVar* cp = cc.FindCVar("camera.pos")) ParseRGB(cp->value, p[0], p[1], p[2]);
        auto clampi = [](float v) { int i = static_cast<int>(std::lround(v)); return i < 0 ? 0 : (i > 127 ? 127 : i); };
        int x = clampi(p[0]), y = clampi(p[1]), z = clampi(p[2]);
        cc.SetCVarOverride("voxel.cursor.x", std::to_string(x));
        cc.SetCVarOverride("voxel.cursor.y", std::to_string(y));
        cc.SetCVarOverride("voxel.cursor.z", std::to_string(z));
        o.Format("cursor set to ({}, {}, {})", x, y, z);
    });
    // `lua` command is registered by ScriptHost::Init() (engine/script) when scripting is enabled.

    c.RegisterCommand("console.rotate_cert", "Regenerate the TLS cert.", [&server](std::span<const std::string_view>, Output& o) { server.RotateCert(); o.Print("ok"); });
    c.RegisterCommand("console.rotate_sessions", "Invalidate all sessions.", [&server](std::span<const std::string_view>, Output& o) { server.RotateSessions(); o.Print("ok"); });
    c.RegisterCommand("console.list_sessions", "List active sessions.", [&server](std::span<const std::string_view>, Output& o) { o.Print(server.ListSessions()); });

    // --- blue-noise cache utilities ---
    c.RegisterCommand("bluenoise.clear_cache", "Delete the cached blue-noise tile (bluenoise64.bin); restart to re-bake.",
        [](std::span<const std::string_view>, Output& o) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(pf::UserDataDir()) / "bluenoise64.bin", ec);
            o.Print("blue-noise cache cleared - restart the engine to re-bake");
        });
    c.RegisterCommand("open_data_folder", "Open the Voxhammer user-data folder (cvars, keybindings, shots, cert) in Explorer.",
        [](std::span<const std::string_view>, Output& o) {
            std::wstring widePath = std::filesystem::path(pf::UserDataDir()).wstring();
            ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            o.Format("opened {}", pf::UserDataDir());
        });
    c.RegisterCommand("console.open", "Open this web console in the default browser (bind to a key, e.g. Grave/backtick).",
        [](std::span<const std::string_view>, Output& o) {
            int port = 27960;
            if (CVar* pw = Console::Get().FindCVar("remote.port_ws")) port = pw->GetInt();
            const std::wstring url = L"https://127.0.0.1:" + std::to_wstring(port) + L"/";
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            o.Format("opened https://127.0.0.1:{}/", port);
        });

    // --- keybindings management (the web editor drives these via `exec`) ---
    c.RegisterCommand("binds", "List keybindings as JSON.", [&kb](std::span<const std::string_view>, Output& o) {
        std::string j = "[";
        const auto& all = kb.All();
        for (std::size_t i = 0; i < all.size(); ++i) {
            const auto& b = all[i];
            std::string mods;
            const auto names = pf::ModNames(b.modifiers);
            for (std::size_t k = 0; k < names.size(); ++k) { if (k) mods += ","; mods += "\"" + names[k] + "\""; }
            if (i) j += ",";
            j += fmt::format(R"({{"key":"{}","mods":[{}],"target":"{}"}})", b.key, mods, b.target);
        }
        j += "]";
        o.Print(j);
    });
    c.RegisterCommand("bind", "bind <key[+mods]> <target...>", [&kb](std::span<const std::string_view> a, Output& o) {
        if (a.size() < 2) { o.Print("usage: bind <Key|Ctrl+Key> <cvar.toggle X | cvar.set X v | command N>"); return; }
        std::string key;
        std::uint32_t mods;
        ParseKeySpec(std::string(a[0]), key, mods);
        std::string target;
        for (std::size_t i = 1; i < a.size(); ++i) { if (i > 1) target += " "; target += a[i]; }
        kb.Set({key, mods, target});
        kb.Save();
        o.Format("bound {} -> {}", KeySpec(key, mods), target);
    });
    c.RegisterCommand("unbind", "unbind <key[+mods]>", [&kb](std::span<const std::string_view> a, Output& o) {
        if (a.empty()) { o.Print("usage: unbind <Key|Ctrl+Key>"); return; }
        std::string key;
        std::uint32_t mods;
        ParseKeySpec(std::string(a[0]), key, mods);
        bool ok = kb.Remove(key, mods);
        kb.Save();
        o.Print(ok ? "unbound" : "no such binding");
    });
    c.RegisterCommand("bind_reload", "Reload keybindings.toml.", [&kb](std::span<const std::string_view>, Output& o) { kb.Reload(); o.Print("reloaded"); });
    c.RegisterCommand("keybindings.reset", "Reset ALL keybindings to factory defaults (overwrites keybindings.toml).",
        [&kb](std::span<const std::string_view>, Output& o) {
            kb.ResetToDefaults();
            o.Format("keybindings reset to defaults ({} bindings)", kb.All().size());
        });
    (void)outln;
}

struct Args {
    std::optional<std::string> remote_password;
    std::string cvars_path;  // empty => default to UserDataDir()/cvars.toml (resolved in main)
    bool no_remote = false;
    bool no_window = false;
    bool reset_cert = false;
    bool show_version = false;
    bool show_help = false;
};
std::string_view ValueOf(std::string_view a) {
    auto e = a.find('=');
    return e == std::string_view::npos ? std::string_view{} : a.substr(e + 1);
}
Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s == "--version") a.show_version = true;
        else if (s == "--help" || s == "-h") a.show_help = true;
        else if (s == "--no-remote") a.no_remote = true;
        else if (s == "--remote") a.no_remote = false;
        else if (s == "--no-window") a.no_window = true;
        else if (s == "--reset-cert") a.reset_cert = true;
        else if (s.rfind("--remote-password=", 0) == 0) a.remote_password = std::string(ValueOf(s));
        else if (s.rfind("--cvars=", 0) == 0) a.cvars_path = std::string(ValueOf(s));
        else if (s.rfind("--vsync=", 0) == 0) {}  // applied post-registration
        else vox::log::Warn("unknown flag: {}", s);
    }
    return a;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace vox::console;
    Args args = ParseArgs(argc, argv);
    if (args.show_version) { vox::log::Info("voxhammer {}", VOX_VERSION_STRING); return 0; }
    if (args.show_help) { Usage(); return 0; }

    vox::log::Info("Voxhammer {} booting", VOX_VERSION_STRING);
    RegisterCoreCvars();
    Console& console = Console::Get();
    vox::platform::LogSummary();

    // Default the cvars file to the user-data dir (alongside keybindings.toml /
    // shots / cert), NOT the working directory -- so settings persist regardless
    // of where the exe is launched and live in one findable place.
    if (args.cvars_path.empty()) {
        args.cvars_path = (std::filesystem::path(vox::platform::UserDataDir()) / "cvars.toml").string();
    }
    vox::log::Info("cvars file: {}", args.cvars_path);
    console.LoadCvarsToml(args.cvars_path);

    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s.rfind("--vsync=", 0) == 0) console.Execute(std::string("renderer.vsync ") + (ValueOf(s) == "on" ? "1" : "0"));
    }
    if (args.remote_password) {
        std::string hash = vox::console::HashPassword(*args.remote_password);
        console.SetCVarOverride("remote.password_hash", hash);
        vox::log::Info("stored remote.password_hash ({})", vox::console::HashingIsCryptographic() ? "argon2id" : "PLACEHOLDER");
    }
    if (args.reset_cert) {
        std::error_code ec;
        std::filesystem::remove((std::filesystem::path(vox::platform::UserDataDir()) / "cert" / "cert.pem"), ec);
        vox::log::Info("--reset-cert: removed existing cert (regenerated on bind)");
    }

    // Keybindings.
    pf::Keybindings keybindings;
    keybindings.LoadOrCreateDefaults((std::filesystem::path(vox::platform::UserDataDir()) / "keybindings.toml").string());

    // ConsoleServer.
    ConsoleServer server;
    RegisterCoreCommands(server, keybindings);
    if (!args.no_remote) {
        ConsoleServer::Config cfg;
        if (CVar* p = console.FindCVar("remote.password_hash")) cfg.password_hash = p->value;
        if (CVar* p = console.FindCVar("remote.port_ws")) cfg.http_port = static_cast<std::uint16_t>(p->GetInt());
        if (CVar* p = console.FindCVar("remote.port_tcp")) cfg.line_port = static_cast<std::uint16_t>(p->GetInt());
        if (CVar* p = console.FindCVar("remote.session_ttl_seconds")) cfg.session_ttl_seconds = p->GetInt();
        cfg.cert_dir = (std::filesystem::path(vox::platform::UserDataDir()) / "cert").string();
        server.Start(cfg, &console);
    }

    // Engine subsystems (M1 bring-up): jobs scheduler, ECS, scripting, voxel world.
    vox::jobs::JobScheduler jobs;
    jobs.Init();  // 0 => auto-size from CPU topology
    vox::ecs::EcsWorld ecs;
    ecs.Init();
    vox::script::ScriptHost scripts;
    scripts.Init();  // registers the `lua` console command
    vox::voxel::VoxelWorld world;
    world.Init();

    // Optional .vox import overrides the renderer's procedural demo scene.
    // vs must outlive renderer.Init so that vs.palette.data() stays valid.
    vox::voxel::VoxScene vs;
    std::vector<std::uint32_t> voxelGrid;
    bool haveVoxels = false;
    bool voxLoaded  = false;   // true once a real .vox map is loaded (import/place/drop); gates the vox emissive boost
    const std::uint32_t* palettePtr = nullptr;
    if (CVar* ip = console.FindCVar("voxel.import_path"); ip && !ip->value.empty()) {
        if (vox::voxel::LoadVox(ip->value, vs)) {
            world = std::move(vs.world);
            voxelGrid = world.BakeFlatGrid(vox::voxel::kWorldDim);
            haveVoxels = true;
            voxLoaded  = true;
            palettePtr = vs.palette.data();
            vox::log::Info("voxel: imported {} ({} voxels, {} chunks)", ip->value, vs.voxelCount, world.ResidentChunks());
        } else {
            vox::log::Warn("voxel: failed to import '{}' -- falling back to the procedural demo scene", ip->value);
            // The saved camera was framed for the now-missing .vox scene, so leaving it would
            // strand the view in the demo scene's empty space (reads as a black screen). Snap
            // the camera cvars back to the default establishing shot so the fallback is visible.
            console.SetCVarOverride("camera.pos",   "32 40 -24");
            console.SetCVarOverride("camera.yaw",   "0.0");
            console.SetCVarOverride("camera.pitch", "-0.5");
        }
    }
    if (!haveVoxels) {
        // No .vox loaded: stamp a destructible procedural demo into the world so
        // voxel.break (and the X key) carves the default scene out of the box.
        // StampVox merges the palette into the world, so carving keeps colors.
        GenerateDemoScene(vs);
        world.StampVox(vs.world, vs.palette, 0, 0, 0);
        voxelGrid = world.BakeFlatGrid(vox::voxel::kWorldDim);
        haveVoxels = true;
        palettePtr = world.Palette().data();
        vox::log::Info("voxel: generated destructible demo scene ({} voxels, {} chunks)", vs.voxelCount, world.ResidentChunks());
    }

    // PhysX rigid-body world (PASS 2). Behind VOX_HAVE_PHYSX -> a logging stub
    // when the SDK is off, so the default build is byte-for-byte unchanged.
    vox::physics::PhysicsWorld physics;
    physics.Init();
    physics.AddGroundPlane();  // y = 0 floor for carve debris to land on

#if defined(VOX_HAVE_PHYSX)
    // Pin a debris material in a slot that is FREE in the live render palette
    // (scan high->low for an empty entry; fall back to 255). Mirror it into both
    // the world palette (so later place/clear/drop re-pushes keep debris colored)
    // and a mutable copy of the initial palette handed to the renderer, so the
    // debris color is present from frame 0 regardless of which palette is active.
    vox::voxel::VoxPalette livePalette{};
    if (palettePtr) std::memcpy(livePalette.data(), palettePtr, livePalette.size() * sizeof(std::uint32_t));
    std::uint8_t debrisMat = 255;
    for (int i = 255; i >= 1; --i) {
        if (livePalette[static_cast<std::size_t>(i)] == 0u) { debrisMat = static_cast<std::uint8_t>(i); break; }
    }
    constexpr std::uint32_t kDebrisColor = 0x00404C66u;  // warm grey-orange rubble (RGBA8, alpha 0)
    livePalette[debrisMat] = kDebrisColor;
    world.SetPaletteColor(debrisMat, kDebrisColor);
    palettePtr = livePalette.data();  // renderer.Init copies this; debris color included
    DebrisField debris;
    debris.Init(debrisMat);
#endif

    // Window + DX12 + key dispatch.
    pf::Window window;
    vox::render::Renderer renderer;
    bool hasWindow = false;
    if (!args.no_window) {
        hasWindow = window.Create(1280, 720, "Voxhammer");
        if (hasWindow) {
            renderer.Init(window.NativeHandle(), window.Width(), window.Height(), haveVoxels ? &voxelGrid : nullptr, palettePtr);
            window.SetKeyHandler([&keybindings](const std::string& key, std::uint32_t mods) {
                keybindings.Dispatch(key, mods, [](const std::string& line) {
                    vox::log::Info("keybind: {}", line);
                    Console::Get().Execute(line);
                });
            });
            vox::log::Info("camera: hold LEFT-MOUSE + WASD/QE to fly (Shift=fast); mouse looks");
        }
    }

    // --- Hot voxel placement commands ---
    // voxel.place: stamp the .vox at voxel.import_path into the live world at cursor.
    // voxel.clear: empty the world immediately.
    // Both call renderer.SetVoxels (WaitIdle + memcpy) on the main thread (via console.Drain).
    {
        Console& c = Console::Get();
        // Report the live count of awake dynamic rigid bodies. ActiveIslands() exists in
        // both builds (real value under PhysX, 0 in the stub), so no gating is needed.
        c.RegisterCommand("physics.dump_islands", "Report active (awake) dynamic rigid bodies.",
            [&physics](std::span<const std::string_view>, Output& o) {
                o.Format("{} active islands", physics.ActiveIslands());
            });
        c.RegisterCommand("voxel.place",
            "Stamp voxel.import_path into the world at (voxel.cursor.x/y/z). Additive.",
            [&world, &renderer, &voxLoaded](std::span<const std::string_view>, Output& o) {
                Console& cc = Console::Get();
                CVar* ipCv = cc.FindCVar("voxel.import_path");
                CVar* cxCv = cc.FindCVar("voxel.cursor.x");
                CVar* cyCv = cc.FindCVar("voxel.cursor.y");
                CVar* czCv = cc.FindCVar("voxel.cursor.z");
                if (!ipCv || ipCv->value.empty()) {
                    o.Print("voxel.place: load failed (set voxel.import_path)");
                    return;
                }
                const std::string path = ipCv->value;
                const int cx = cxCv ? cxCv->GetInt() : 0;
                const int cy = cyCv ? cyCv->GetInt() : 0;
                const int cz = czCv ? czCv->GetInt() : 0;
                vox::voxel::VoxScene vs;
                if (vox::voxel::LoadVox(path, vs)) {
                    world.StampVox(vs.world, vs.palette, cx, cy, cz);
                    voxLoaded = true;
                    auto grid = world.BakeFlatGrid(vox::voxel::kWorldDim);
                    renderer.SetVoxels(grid, world.Palette().data());
                    o.Format("placed {} at ({},{},{}) - {} voxels", path, cx, cy, cz, vs.voxelCount);
                } else {
                    o.Print("voxel.place: load failed (set voxel.import_path)");
                }
            });

        c.RegisterCommand("voxel.clear",
            "Clear all voxels from the world.",
#if defined(VOX_HAVE_PHYSX)
            [&world, &renderer, &voxLoaded, &physics, &debris](std::span<const std::string_view>, Output& o) {
                debris.ClearAll(world, physics, renderer);  // drop debris bodies + their cells first
#else
            [&world, &renderer, &voxLoaded](std::span<const std::string_view>, Output& o) {
#endif
                world.Clear();
                voxLoaded = false;
                auto grid = world.BakeFlatGrid(vox::voxel::kWorldDim);
                renderer.SetVoxels(grid, world.Palette().data());
                o.Print("voxel world cleared");
            });

        // voxel.break: ray-cast from the camera along its forward vector and carve
        // a sphere (voxel.break_radius) out of the first solid voxel that's hit.
        c.RegisterCommand("voxel.break",
            "Carve a sphere (voxel.break_radius) out of the voxel the camera looks at.",
#if defined(VOX_HAVE_PHYSX)
            [&world, &renderer, &physics, &debris](std::span<const std::string_view>, Output& o) {
#else
            [&world, &renderer](std::span<const std::string_view>, Output& o) {
#endif
                Console& cc = Console::Get();
                float pos[3] = {0.0f, 0.0f, 0.0f};
                if (CVar* cp = cc.FindCVar("camera.pos")) ParseRGB(cp->value, pos[0], pos[1], pos[2]);
                const float yaw = cc.FindCVar("camera.yaw") ? cc.FindCVar("camera.yaw")->GetFloat() : 0.0f;
                const float pitch = cc.FindCVar("camera.pitch") ? cc.FindCVar("camera.pitch")->GetFloat() : 0.0f;

                // Forward convention -- MUST match the flycam in the run loop:
                //   fwd = {cos(pitch)*sin(yaw), sin(pitch), cos(pitch)*cos(yaw)}
                const float cp = std::cos(pitch), sp = std::sin(pitch);
                const float cyw = std::cos(yaw), syw = std::sin(yaw);
                const float fwd[3] = {cp * syw, sp, cp * cyw};

                int hx = 0, hy = 0, hz = 0;
                const float maxDist = static_cast<float>(vox::voxel::kWorldDim) * 2.0f;
                if (!world.RaycastSolid(pos, fwd, maxDist, hx, hy, hz)) {
                    o.Print("voxel.break: nothing solid in range");
                    return;
                }
                const int radius = cc.FindCVar("voxel.break_radius")
                                       ? cc.FindCVar("voxel.break_radius")->GetInt() : 2;
#if defined(VOX_HAVE_PHYSX)
                // Capture the REAL voxels about to be removed as falling chunks
                // BEFORE carving (so world.GetVoxel still returns the solids).
                // Each chunk keeps its true shape + original colors and tumbles;
                // the run loop re-stamps it at the body's full transform.
                debris.SetMaxLive(cc.FindCVar("voxel.debris.max") ? cc.FindCVar("voxel.debris.max")->GetInt() : 256);
                debris.SetScale(cc.FindCVar("voxel.debris.scale") ? cc.FindCVar("voxel.debris.scale")->GetFloat() : 1.0f);
                debris.ReservePool(physics);
                debris.SpawnFromCarve(world, physics, hx, hy, hz, radius,
                                      /*force=*/0.0f, /*radial=*/false);
#endif
                const int removed = world.CarveSphere(hx, hy, hz, radius);
                // Push ONLY the carved region to the GPU (incremental, no full
                // re-bake and no GPU stall) so rapid carving stays smooth.
                const int G = static_cast<int>(vox::voxel::kWorldDim);
                const int x0 = std::max(0, hx - radius), y0 = std::max(0, hy - radius), z0 = std::max(0, hz - radius);
                const int x1 = std::min(G, hx + radius + 1), y1 = std::min(G, hy + radius + 1), z1 = std::min(G, hz + radius + 1);
                std::vector<std::uint32_t> region;
                region.reserve(static_cast<std::size_t>(x1 - x0) * (y1 - y0) * (z1 - z0));
                for (int z = z0; z < z1; ++z)
                    for (int y = y0; y < y1; ++y)
                        for (int x = x0; x < x1; ++x)
                            region.push_back(static_cast<std::uint32_t>(world.GetVoxel(x, y, z)));
                renderer.EditVoxels(x0, y0, z0, x1, y1, z1, region.data(), nullptr);  // palette unchanged by carve
                o.Format("voxel.break: removed {} voxels around hit ({},{},{}) r={}",
                         removed, hx, hy, hz, radius);
            });

        // voxel.explode: like voxel.break but carves a LARGER crater
        // (voxel.explode_radius) and spawns a big RADIAL debris blast (chunks fly
        // outward from the hit, scaled by voxel.explode_force). The CARVE half
        // works in BOTH builds; only the debris spawn is gated on VOX_HAVE_PHYSX
        // (in the OFF build it just blows a crater, no flying chunks).
        c.RegisterCommand("voxel.explode",
            "Blast a large crater (voxel.explode_radius) where the camera looks + a radial debris burst (voxel.explode_force).",
#if defined(VOX_HAVE_PHYSX)
            [&world, &renderer, &physics, &debris](std::span<const std::string_view>, Output& o) {
#else
            [&world, &renderer](std::span<const std::string_view>, Output& o) {
#endif
                Console& cc = Console::Get();
                float pos[3] = {0.0f, 0.0f, 0.0f};
                if (CVar* cp = cc.FindCVar("camera.pos")) ParseRGB(cp->value, pos[0], pos[1], pos[2]);
                const float yaw = cc.FindCVar("camera.yaw") ? cc.FindCVar("camera.yaw")->GetFloat() : 0.0f;
                const float pitch = cc.FindCVar("camera.pitch") ? cc.FindCVar("camera.pitch")->GetFloat() : 0.0f;

                // Forward convention -- MUST match the flycam / voxel.break.
                const float cp = std::cos(pitch), sp = std::sin(pitch);
                const float cyw = std::cos(yaw), syw = std::sin(yaw);
                const float fwd[3] = {cp * syw, sp, cp * cyw};

                int hx = 0, hy = 0, hz = 0;
                const float maxDist = static_cast<float>(vox::voxel::kWorldDim) * 2.0f;
                if (!world.RaycastSolid(pos, fwd, maxDist, hx, hy, hz)) {
                    o.Print("voxel.explode: nothing solid in range");
                    return;
                }
                const int radius = cc.FindCVar("voxel.explode_radius")
                                       ? cc.FindCVar("voxel.explode_radius")->GetInt() : 8;
#if defined(VOX_HAVE_PHYSX)
                // Capture the REAL voxels about to be removed as a RADIAL debris
                // burst BEFORE carving (so world.GetVoxel still sees the solids):
                // each chunk keeps its true shape + original colors and tumbles
                // outward from the crater, magnitude scaled by voxel.explode_force.
                const float force = cc.FindCVar("voxel.explode_force")
                                        ? cc.FindCVar("voxel.explode_force")->GetFloat() : 12.0f;
                debris.SetMaxLive(cc.FindCVar("voxel.debris.max") ? cc.FindCVar("voxel.debris.max")->GetInt() : 256);
                debris.SetScale(cc.FindCVar("voxel.debris.scale") ? cc.FindCVar("voxel.debris.scale")->GetFloat() : 1.0f);
                debris.ReservePool(physics);
                debris.SpawnFromCarve(world, physics, hx, hy, hz, radius, /*force=*/force, /*radial=*/true);
#endif
                const int removed = world.CarveSphere(hx, hy, hz, radius);
                // Push ONLY the carved region to the GPU (incremental, same path
                // as voxel.break -- no full re-bake / GPU stall).
                const int G = static_cast<int>(vox::voxel::kWorldDim);
                const int x0 = std::max(0, hx - radius), y0 = std::max(0, hy - radius), z0 = std::max(0, hz - radius);
                const int x1 = std::min(G, hx + radius + 1), y1 = std::min(G, hy + radius + 1), z1 = std::min(G, hz + radius + 1);
                std::vector<std::uint32_t> region;
                region.reserve(static_cast<std::size_t>(x1 - x0) * (y1 - y0) * (z1 - z0));
                for (int z = z0; z < z1; ++z)
                    for (int y = y0; y < y1; ++y)
                        for (int x = x0; x < x1; ++x)
                            region.push_back(static_cast<std::uint32_t>(world.GetVoxel(x, y, z)));
                renderer.EditVoxels(x0, y0, z0, x1, y1, z1, region.data(), nullptr);  // palette unchanged by carve
                o.Format("voxel.explode: removed {} voxels around hit ({},{},{}) r={}",
                         removed, hx, hy, hz, radius);
            });
    }

    // Real screenshot command: capture DX12 backbuffer -> PNG or BMP in UserDataDir/shots/.
    {
        Console& c = Console::Get();
        c.RegisterCommand("screenshot", "Capture the current frame to UserDataDir/shots/.",
            [&renderer](std::span<const std::string_view>, Output& o) {
                namespace pf2 = vox::platform;
                std::filesystem::path dir = std::filesystem::path(pf2::UserDataDir()) / "shots";
                std::error_code mkec;
                std::filesystem::create_directories(dir, mkec);

                // Format: "PNG" or "BMP" from cvar; anything else falls back to PNG.
                CVar* fmtCv = Console::Get().FindCVar("renderer.screenshot.format");
                const bool usePng = !(fmtCv && fmtCv->value == "BMP");
                const char* ext = usePng ? "png" : "bmp";

                // Timestamp for unique filename.
                std::time_t t = std::time(nullptr);
                char tsBuf[32] = {};
                std::tm tmLocal{};
#if defined(_WIN32)
                localtime_s(&tmLocal, &t);
#else
                localtime_r(&t, &tmLocal);
#endif
                std::strftime(tsBuf, sizeof(tsBuf), "%Y%m%d_%H%M%S", &tmLocal);
                std::string filename = fmt::format("voxhammer_{}.{}", tsBuf, ext);
                std::string path = (dir / filename).string();

                if (renderer.CaptureScreenshot(path, usePng)) {
                    o.Format("saved {}", path);
                } else {
                    o.Print("screenshot failed (no DX12?)");
                }
            });
    }

    // Reset all non-readonly cvars to their registered defaults and persist.
    {
        Console& c = Console::Get();
        c.RegisterCommand("cvars.reset",
            "Reset all user-tunable cvars to factory defaults and save cvars.toml. "
            "Read-only cvars (ports, password hash) are skipped.",
            [&console, cvars_path = args.cvars_path](std::span<const std::string_view>, Output& o) {
                // Collect (name, default_value) pairs first -- never mutate during enumeration.
                std::vector<std::pair<std::string, std::string>> resets;
                console.EnumerateCVars("", [&resets](CVar& cv) {
                    if (cv.flags & CVAR_READONLY) return;
                    resets.emplace_back(cv.name, cv.default_value);
                });
                for (const auto& [name, def] : resets)
                    console.SetCVarOverride(name, def);
                console.SaveCvarsToml(cvars_path);
                o.Format("reset {} settings to defaults", resets.size());
            });
    }

    // Web drag-drop .vox upload handler.
    // Called on the main thread (via QueueTask / Drain) -- SetVoxels is safe.
    server.SetVoxUploadHandler([&](std::string name, std::vector<std::uint8_t> bytes) {
        vox::voxel::VoxScene vs;
        if (vox::voxel::LoadVoxFromMemory(bytes.data(), bytes.size(), vs)) {
            int cx = console.FindCVar("voxel.cursor.x") ? console.FindCVar("voxel.cursor.x")->GetInt() : 0;
            int cy = console.FindCVar("voxel.cursor.y") ? console.FindCVar("voxel.cursor.y")->GetInt() : 0;
            int cz = console.FindCVar("voxel.cursor.z") ? console.FindCVar("voxel.cursor.z")->GetInt() : 0;
            world.StampVox(vs.world, vs.palette, cx, cy, cz);
            voxLoaded = true;
            auto grid = world.BakeFlatGrid(vox::voxel::kWorldDim);
            renderer.SetVoxels(grid, world.Palette().data());
            // Cache the uploaded bytes to <userdata>/uploads/<name> and point
            // voxel.import_path at it, so the web "Place" button (which re-stamps
            // voxel.import_path) works for dropped files and a restart can reuse it.
            std::error_code ec;
            const std::filesystem::path dir = std::filesystem::path(vox::platform::UserDataDir()) / "uploads";
            std::filesystem::create_directories(dir, ec);
            std::string leaf = std::filesystem::path(name).filename().string();
            if (leaf.empty()) leaf = "upload.vox";
            const std::filesystem::path dst = dir / leaf;
            if (std::ofstream of(dst, std::ios::binary); of) {
                of.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            std::error_code exists_ec;
            if (std::filesystem::exists(dst, exists_ec)) {
                console.SetCVarOverride("voxel.import_path", dst.string());
                vox::log::Info("voxel: dropped {} ({} voxels) at ({},{},{}); voxel.import_path -> {}",
                               name, vs.voxelCount, cx, cy, cz, dst.string());
            } else {
                vox::log::Info("voxel: dropped {} ({} voxels) at ({},{},{}) (could not cache upload to disk)",
                               name, vs.voxelCount, cx, cy, cz);
            }
        } else {
            vox::log::Warn("voxel: bad .vox upload {}", name);
        }
    });

    // Engine loop.
    vox::log::Info("entering main loop ({})", hasWindow ? "windowed" : "headless");
    using clk = std::chrono::steady_clock;
    auto prev = clk::now();
    auto lastTele = prev;
    while (g_running.load()) {
        if (hasWindow) {
            window.PollEvents();
            if (window.ShouldClose()) break;
        }
        keybindings.CheckHotReload();
        console.Drain();
        if (server.UnbindRequested() && server.IsRunning()) server.Stop();

        auto now = clk::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;

        // Fly-cam: hold LEFT-MOUSE + WASD/QE to move, mouse to look, Shift=fast.
        // Writes the camera.* cvars (so the web console reflects it live).
        if (hasWindow) {
            pf::Window::CameraInput ci = window.PollCameraInput();
            if (ci.active && (ci.move_strafe || ci.move_up || ci.move_fwd || ci.look_dx || ci.look_dy)) {
                float yaw = console.FindCVar("camera.yaw")->GetFloat();
                float pitch = console.FindCVar("camera.pitch")->GetFloat();
                float pos[3] = {0, 0, 0};
                ParseRGB(console.FindCVar("camera.pos")->value, pos[0], pos[1], pos[2]);
                const float PI = 3.14159265f;
                float sens = console.FindCVar("camera.sensitivity")->GetFloat();
                float invx = console.FindCVar("camera.invert_x")->GetBool() ? -1.0f : 1.0f;
                float invy = console.FindCVar("camera.invert_y")->GetBool() ? -1.0f : 1.0f;
                yaw += ci.look_dx * sens * invx;
                pitch -= ci.look_dy * sens * invy;
                while (yaw > PI) yaw -= 2 * PI;
                while (yaw < -PI) yaw += 2 * PI;
                pitch = std::clamp(pitch, -1.5f, 1.5f);
                float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
                float fwd[3] = {cp * sy, sp, cp * cy};
                float rl = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
                float right[3] = {rl > 1e-5f ? -fwd[2] / rl : 1.0f, 0.0f, rl > 1e-5f ? fwd[0] / rl : 0.0f};
                float base = console.FindCVar("camera.move_speed")->GetFloat();
                float speed = (ci.fast ? base * console.FindCVar("camera.boost")->GetFloat() : base) * dt;
                for (int i = 0; i < 3; ++i)
                    pos[i] += (right[i] * ci.move_strafe + (i == 1 ? ci.move_up : 0.0f) + fwd[i] * ci.move_fwd) * speed;
                console.SetCVarOverride("camera.yaw", fmt::format("{:.4f}", yaw));
                console.SetCVarOverride("camera.pitch", fmt::format("{:.4f}", pitch));
                console.SetCVarOverride("camera.pos", fmt::format("{:.2f} {:.2f} {:.2f}", pos[0], pos[1], pos[2]));
            }
        }

        ecs.Step(dt);  // advance ECS systems (kinematics + lifetimes)

        // PhysX rigid-body step (no-op stub when VOX_HAVE_PHYSX is off). Honor
        // debug.pause_simulation so the debris freezes when the sim is paused.
        if (!console.FindCVar("debug.pause_simulation")->GetBool()) {
            physics.Step(dt);
#if defined(VOX_HAVE_PHYSX)
            // Re-insert live debris bodies into the GPU voxel grid (clear last
            // frame's cells, paint this frame's) -- only when the renderer is up.
            if (renderer.Valid() && (!debris.Empty()))
                debris.Update(world, physics, renderer, jobs, dt);
#endif
        }

        if (renderer.Valid()) {
            vox::render::FrameParams fp;
            ParseRGB(console.FindCVar("renderer.debug.clear_color")->value, fp.clear[0], fp.clear[1], fp.clear[2]);
            ParseRGB(console.FindCVar("camera.pos")->value, fp.cam_pos[0], fp.cam_pos[1], fp.cam_pos[2]);
            fp.cam_yaw = console.FindCVar("camera.yaw")->GetFloat();
            fp.cam_pitch = console.FindCVar("camera.pitch")->GetFloat();
            fp.cam_fov = console.FindCVar("camera.fov")->GetFloat();
            fp.exposure = console.FindCVar("renderer.exposure")->GetFloat();
            fp.hdr = console.FindCVar("renderer.hdr.enabled")->GetBool() ? 1 : 0;
            float saz = console.FindCVar("renderer.sun.azimuth")->GetFloat();
            float sel = console.FindCVar("renderer.sun.elevation")->GetFloat();
            float ce = std::cos(sel);
            fp.sun[0] = ce * std::sin(saz);
            fp.sun[1] = std::sin(sel);
            fp.sun[2] = ce * std::cos(saz);
            fp.ambient = console.FindCVar("renderer.ambient")->GetFloat();
            fp.shadow_softness = console.FindCVar("renderer.shadow.softness")->GetFloat();
            fp.ao_strength = console.FindCVar("renderer.ao.strength")->GetFloat();
            fp.ao_radius = console.FindCVar("renderer.ao.radius")->GetFloat();
            fp.lighting_mode = (console.FindCVar("renderer.lighting.mode")->value == "QUALITY") ? 1 : 0;
            fp.dither = console.FindCVar("renderer.dither")->GetBool() ? 1 : 0;
            fp.ao_samples = console.FindCVar("renderer.ao.samples")->GetInt();
            fp.shadow_samples = console.FindCVar("renderer.shadow.samples")->GetInt();
            fp.gi_samples = console.FindCVar("renderer.gi.samples")->GetInt();
            fp.gi_bounces = console.FindCVar("renderer.gi.bounces")->GetInt();
            fp.gi_emissive = console.FindCVar("renderer.gi.emissive")->GetFloat();
            // Boost emissive ONLY for loaded .vox maps (MagicaVoxel _emit is dim); the
            // demo/code emissive stays at the sane gi.emissive. Live + uncapped.
            fp.vox_emissive = voxLoaded ? console.FindCVar("renderer.vox.emissive_boost")->GetFloat() : 1.0f;
            fp.emissive_surface = console.FindCVar("renderer.emissive.surface")->GetFloat();
            fp.gi_intensity = console.FindCVar("renderer.gi.intensity")->GetFloat();
            fp.gi_reproject_history = console.FindCVar("renderer.gi.reproject_history")->GetInt();
            const std::string& giDn = console.FindCVar("renderer.gi.denoiser")->value;
            fp.gi_denoiser = (giDn == "SVGF") ? 2 : (giDn == "ATROUS") ? 1 : 0;
            fp.gbuffer_obb = console.FindCVar("renderer.gbuffer.obb")->GetBool() ? 1 : 0;  // A1 OBB-raster gate (default OFF -> proven A0 path)
            fp.gi_atrous_iters = console.FindCVar("renderer.gi.atrous_iters")->GetInt();
            fp.gi_denoise_phi_normal = console.FindCVar("renderer.gi.denoise_phi_normal")->GetFloat();
            fp.gi_denoise_phi_depth = console.FindCVar("renderer.gi.denoise_phi_depth")->GetFloat();
            fp.gi_denoise_phi_lum = console.FindCVar("renderer.gi.denoise_phi_lum")->GetFloat();
            renderer.SetGiDenoise(console.FindCVar("renderer.gi.denoise")->GetBool());  // self-guards; resets accum only on toggle
            renderer.SetEmptySpaceSkip(console.FindCVar("renderer.empty_space_skip")->GetBool());  // self-guards
            renderer.SetGiDebug(console.FindCVar("renderer.gi.debug")->GetBool());  // self-guards
            renderer.SetGiReproject(console.FindCVar("renderer.gi.reproject")->GetBool());  // self-guards; resets accum only on toggle
            renderer.RenderFrame(fp);  // vsync caps the loop
        }

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTele).count() >= 100) {
            lastTele = now;
            float fps = dt > 0.0f ? 1.0f / dt : 0.0f;
            bool paused = console.FindCVar("debug.pause_simulation")->GetBool();
            std::string data = fmt::format(
                R"({{"fps":{:.1f},"frame_ms":{:.2f},"gpu_mem_mb":0,"gpu_budget_mb":16384,"chunks":{},"islands":{},"archetypes":{},"denoiser_ms":0,"upscaler_ms":0,"paused":{}}})",
                fps, dt * 1000.0f, world.ResidentChunks(), physics.ActiveIslands(), ecs.EntityCount(), paused ? "true" : "false");
            server.BroadcastEvent("frame_stats", data);
        }

        if (!renderer.Valid()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));  // headless: don't spin
        }
    }

    vox::log::Info("shutting down");
    renderer.Shutdown();
    if (hasWindow) window.Destroy();
    scripts.Shutdown();
    ecs.Shutdown();
    physics.Shutdown();  // releases PhysX scene/actors (no-op stub when off)
    world.Shutdown();
    jobs.Shutdown();
    server.Stop();
    console.SaveCvarsToml(args.cvars_path);
    return 0;
}
