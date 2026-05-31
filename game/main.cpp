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
#include "voxel/StructuralSettle.h"
#include "voxel/VoxImport.h"
#include "voxel/VoxelWorld.h"
#include "voxel/destruction/box_decompose.h"  // Milestone C: world box-compound collider
#include "voxel/destruction/connectivity.h"
#include "voxel/destruction/island_extract.h"  // re-fracture a carved debris chunk into pieces
#include "voxel/destruction/box_graph.h"        // Phase 1: box-level union-find connectivity

#include <glm/glm.hpp>  // glm::ivec3 for the world-collider decompose

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
#include <utility>
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
                        vox::render::Renderer& renderer,
                        int hx, int hy, int hz, int radius, float force, bool radial) {
        if (maxLive_ <= 0 || radius <= 0) return;
        const bool useObb = UseObb();
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

            const int id = AcquireForChunk(physics, c, scx, scy, scz, vx, vy, vz, wx, wy, wz);
            if (id < 0) return;  // pool exhausted
            c.id = id;
            if (useObb) RegisterDynObject(renderer, world, c);   // A3: draw as an OBB rigid body
            live_.push_back(std::move(c));
        }
    }

    // Spawn a single STRUCTURALLY-DETACHED island (Milestone B1) as one falling
    // rigid-body chunk. `mats` is a tight local material grid of dims
    // (dx,dy,dz) (row-major idx = z*dx*dy + y*dx + lx; 0 = empty -> concave
    // shape preserved); `ox/oy/oz` is the voxel offset of its (0,0,0) corner in
    // the world grid. The island detaches in place (no blast), then gravity
    // drops it -- "nothing floats". MUST be called AFTER the island's voxels
    // have been cleared from `world` (StructuralSettle already did that), so
    // the per-frame re-stamp does not fight the now-empty terrain cells.
    void SpawnIsland(vox::physics::PhysicsWorld& physics, vox::render::Renderer& renderer,
                     const vox::voxel::VoxelWorld& world,
                     const std::vector<std::uint8_t>& mats, int dx, int dy, int dz,
                     int ox, int oy, int oz) {
        if (maxLive_ <= 0 || dx <= 0 || dy <= 0 || dz <= 0) return;
        if (static_cast<int>(live_.size()) >= maxLive_) return;  // cap reached
        if (mats.size() != static_cast<std::size_t>(dx) * dy * dz) return;

        Chunk c;
        c.dx = dx; c.dy = dy; c.dz = dz;
        c.mats = mats;  // copy the tight island grid verbatim (already 0=empty)
        c.solidCount = 0;
        for (std::uint8_t m : c.mats) if (m) ++c.solidCount;
        if (c.solidCount == 0) return;

        // Body local center == geometric center of the local box; spawn so the
        // island's voxels initially coincide with the world cells it came from.
        c.localCx = 0.5f * static_cast<float>(c.dx);
        c.localCy = 0.5f * static_cast<float>(c.dy);
        c.localCz = 0.5f * static_cast<float>(c.dz);
        const float scx = static_cast<float>(ox) + c.localCx;
        const float scy = static_cast<float>(oy) + c.localCy;
        const float scz = static_cast<float>(oz) + c.localCz;

        // Detach-in-place: zero linear velocity (a tiny jitter so a stack of
        // islands doesn't sit in a perfectly balanced column), gentle spin so
        // it topples naturally as it falls.
        const float vx = Frand(-0.5f, 0.5f), vy = 0.0f, vz = Frand(-0.5f, 0.5f);
        const float wx = Frand(-2.5f, 2.5f), wy = Frand(-2.5f, 2.5f), wz = Frand(-2.5f, 2.5f);

        const int id = AcquireForChunk(physics, c, scx, scy, scz, vx, vy, vz, wx, wy, wz);
        if (id < 0) return;  // pool exhausted
        c.id = id;
        if (UseObb()) RegisterDynObject(renderer, world, c);   // A3: tumble as an OBB rigid body (else re-stamp = no smooth rotation)
        live_.push_back(std::move(c));
    }

    // "Broken things stay breakable": ray-cast the camera ray against every live
    // debris chunk (in each chunk's own rotated local frame), and if a chunk is
    // the NEAREST thing the ray hits (closer than worldHitDist -- the distance to
    // the world's RaycastSolid hit, or FLT_MAX if the world missed), carve a
    // sphere out of THAT chunk and re-fracture it: the carve may split the chunk
    // into several connected components, each of which becomes its own new rigid
    // body inheriting the parent's pose + velocity. Returns true if a chunk was
    // hit + carved (so the caller skips the world carve); false => let the world
    // carve proceed as before. No-op (returns false) unless voxel.debris_breakable.
    bool TryCarveRay(vox::voxel::VoxelWorld& world, vox::physics::PhysicsWorld& physics,
                     vox::render::Renderer& renderer,
                     const float ro[3], const float rd[3], float maxDist, int radius,
                     float worldHitDist) {
        {
            CVar* cv = Console::Get().FindCVar("voxel.debris_breakable");
            if (cv && !cv->GetBool()) return false;
        }
        // 1) Nearest chunk hit, must beat the world hit (+ half a voxel so a debris
        //    chunk re-stamped INTO the world grid wins the coincident tie).
        int   bestIdx = -1;
        float bestT   = worldHitDist + 0.5f;
        int   bhx = 0, bhy = 0, bhz = 0;
        for (std::size_t i = 0; i < live_.size(); ++i) {
            const Chunk& c = live_[i];
            if (c.id < 0 || c.solidCount == 0) continue;
            vox::physics::BodyState st;
            if (!physics.GetBodyState(c.id, st)) continue;
            // World ray -> chunk-local grid space: lo = R^-1*(ro - bodyPos) + C,
            // ld = R^-1*rd (R orthonormal, so ld stays unit + t is world-distance).
            float lo[3], ld[3], dlt[3] = {ro[0] - st.px, ro[1] - st.py, ro[2] - st.pz};
            QuatRotateInv(st, dlt[0], dlt[1], dlt[2], lo[0], lo[1], lo[2]);
            lo[0] += c.localCx; lo[1] += c.localCy; lo[2] += c.localCz;
            QuatRotateInv(st, rd[0], rd[1], rd[2], ld[0], ld[1], ld[2]);
            int lx, ly, lz; float t;
            if (LocalRaycast(c, lo, ld, maxDist, lx, ly, lz, t) && t < bestT) {
                bestT = t; bestIdx = static_cast<int>(i); bhx = lx; bhy = ly; bhz = lz;
            }
        }
        if (bestIdx < 0) return false;  // world (or nothing) is nearer
        CarveChunk(world, physics, renderer, static_cast<std::size_t>(bestIdx), bhx, bhy, bhz, radius);
        return true;
    }

    // Per-frame: cull dead chunks (clearing their last footprint), then re-stamp
    // each live chunk at its body's full transform. `jobs` parallelizes the
    // per-chunk region build; uploads are serialized on the calling (main) thread.
    void Update(vox::voxel::VoxelWorld& world, vox::physics::PhysicsWorld& physics,
                vox::render::Renderer& renderer, vox::jobs::JobScheduler& jobs, float dt) {
        // 1) Cull chunks below the kill plane (or aged out if voxel.debris.ttl > 0; default
        //    0 = debris PERSIST where they land, only culled when they fall off the world).
        //    Build their "clear previous footprint to terrain" work, then release the body.
        const float ttl = Console::Get().FindCVar("voxel.debris.ttl")
                               ? Console::Get().FindCVar("voxel.debris.ttl")->GetFloat() : 0.0f;
        for (auto it = live_.begin(); it != live_.end();) {
            it->age += dt;
            const float y = physics.BodyPosY(it->id);
            const bool dead = !std::isfinite(y) || y < kKillY || (ttl > 0.0f && it->age > ttl);
            if (dead) {
                if (it->dynHandle >= 0) renderer.RemoveDynObject(it->dynHandle);  // A3: free the OBB slot
                else if (it->hasPrev) ClearBoxToTerrain(world, renderer, it->prev);
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
            if (t.dynHandle >= 0) {
                // A3 OBB path: hand the live rigid-body transform to the renderer (it
                // draws the chunk as an independent voxel object) and SKIP the grid
                // re-stamp entirely -- so the chunk rotates smoothly, not grid-snapped.
                const float pos[3]  = { st.px, st.py, st.pz };
                const float quat[4] = { st.qx, st.qy, st.qz, st.qw };
                const float ctr[3]  = { t.localCx, t.localCy, t.localCz };
                renderer.SetDynObjectTransform(t.dynHandle, pos, quat, ctr);
                continue;
            }
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
        renderer.ClearDynObjects();      // A3: free all OBB dynamic-object slots
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
        // Milestone A3 (renderer.debris_obb): when the OBB path is on, this chunk is a
        // renderer dynamic VoxelObject (-1 = none / re-stamp path). On spawn we
        // AddDynObject(its local grid); each frame SetDynObjectTransform from the body;
        // on death RemoveDynObject. The re-stamp (EditVoxels) is skipped while this is set.
        int dynHandle = -1;
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

    // Milestone A3 gate: render debris as independent OBB VoxelObjects (smooth rigid
    // rotation) instead of re-stamping them into the world grid. Read live so a runtime
    // toggle takes effect; default OFF => the proven re-stamp path, byte-for-byte.
    static bool UseObb() {
        CVar* cv = Console::Get().FindCVar("renderer.debris_obb");
        return cv && cv->GetBool();
    }

    static std::size_t Idx(const Chunk& c, int lx, int ly, int lz) {
        return static_cast<std::size_t>(lz) * c.dx * c.dy
             + static_cast<std::size_t>(ly) * c.dx + static_cast<std::size_t>(lx);
    }

    // Milestone A3: register chunk `c` as a renderer dynamic VoxelObject. Widens the
    // chunk's uint8 local mats grid (idx z*dx*dy + y*dx + x, 0 = empty) to uint32 and
    // uploads it + the world palette (debris materials are world material indices) into
    // a pooled slot. Sets c.dynHandle (-1 if the pool is full -> that chunk silently
    // falls back to nothing rendered; physics still simulates it). Caller gates on UseObb().
    void RegisterDynObject(vox::render::Renderer& renderer, const vox::voxel::VoxelWorld& world, Chunk& c) {
        if (c.dx <= 0 || c.dy <= 0 || c.dz <= 0) return;
        std::vector<std::uint32_t> grid(c.mats.size());
        for (std::size_t i = 0; i < c.mats.size(); ++i) grid[i] = static_cast<std::uint32_t>(c.mats[i]);
        c.dynHandle = renderer.AddDynObject(grid.data(), c.dx, c.dy, c.dz, world.Palette().data());
    }

    // #2 gate: give each chunk a box-COMPOUND collider (greedy box-cover of its
    // own voxels) instead of one oversized bounding box, so concave chunks fit
    // through gaps. Read live; default ON. (Only matters once physics.world_collider
    // is on -- against the flat ground plane a bounding box behaves the same.)
    static bool UseCompound() {
        CVar* cv = Console::Get().FindCVar("physics.debris_compound");
        return cv && cv->GetBool();
    }

    // Above this box count the compound's PhysX cost outweighs the accuracy gain,
    // so the chunk falls back to a single bounding box. Debris chunks are small,
    // so a convex-ish chunk decomposes into only a handful of boxes; this caps the
    // rare highly-concave outlier.
    static constexpr std::size_t kMaxChunkBoxes = 48;

    // Build a LOCAL-frame box-compound cover of chunk `c`'s solid voxels (reuse the
    // B0 greedy decompose). A voxel grid box [v..v+1] maps to the body-local box
    // [v - C .. v+1 - C] (C = local center), exactly matching the renderer's
    // R*(p - C) + pos stamp, so collider shapes and rendered voxels stay locked.
    // Returns false (caller uses a single bounding box) when: the chunk is a solid
    // box (no concavity -> bounding box is already exact), the decompose is empty,
    // or it needs more than kMaxChunkBoxes boxes (bounding box is cheaper).
    bool BuildLocalBoxes(const Chunk& c, std::vector<vox::physics::ColliderBox>& out) const {
        out.clear();
        const std::size_t total = static_cast<std::size_t>(c.dx) * c.dy * c.dz;
        if (total == 0 || c.mats.size() != total) return false;
        if (c.solidCount == total) return false;  // fully solid -> single box is exact
        vox::destruction::ComponentField cf;
        cf.ids.assign(total, std::uint16_t{0});
        for (std::size_t i = 0; i < total; ++i) cf.ids[i] = c.mats[i] ? std::uint16_t{1} : std::uint16_t{0};
        cf.anchoredCount = 1;
        cf.totalCount = 1;
        const std::vector<vox::destruction::Box> boxes =
            vox::destruction::decomposeComponent(cf, glm::ivec3(c.dx, c.dy, c.dz), 1);
        if (boxes.empty() || boxes.size() > kMaxChunkBoxes) return false;
        out.reserve(boxes.size());
        for (const auto& b : boxes) {
            vox::physics::ColliderBox cb;
            cb.minX = static_cast<float>(b.mn.x)     - c.localCx;
            cb.minY = static_cast<float>(b.mn.y)     - c.localCy;
            cb.minZ = static_cast<float>(b.mn.z)     - c.localCz;
            cb.maxX = static_cast<float>(b.mx.x + 1) - c.localCx;
            cb.maxY = static_cast<float>(b.mx.y + 1) - c.localCy;
            cb.maxZ = static_cast<float>(b.mx.z + 1) - c.localCz;
            out.push_back(cb);
        }
        return true;
    }

    // Acquire a pooled body for chunk `c` at world center (scx,scy,scz) with the
    // given linear/angular velocity. Uses a box-compound collider when UseCompound()
    // and the cover is worthwhile; otherwise a single bounding box. Returns the
    // body id (-1 = pool exhausted).
    int AcquireForChunk(vox::physics::PhysicsWorld& physics, const Chunk& c,
                        float scx, float scy, float scz,
                        float vx, float vy, float vz, float wx, float wy, float wz) const {
        if (UseCompound()) {
            std::vector<vox::physics::ColliderBox> lboxes;
            if (BuildLocalBoxes(c, lboxes)) {
                const int id = physics.AcquireBoxCompound(scx, scy, scz, lboxes, vx, vy, vz, wx, wy, wz);
                if (id >= 0) return id;  // fall through to single box only if the pool rejected it
            }
        }
        return physics.AcquireBox(scx, scy, scz, c.localCx, c.localCy, c.localCz, vx, vy, vz, wx, wy, wz);
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

    // Map a parent-local grid point (lpx,lpy,lpz) to world via the body pose:
    // W = bodyPos + R*(p - C). (C = local center; R = body orientation.)
    static void LocalGridPointToWorld(const vox::physics::BodyState& st,
                                      float cx, float cy, float cz,
                                      float lpx, float lpy, float lpz, float out[3]) {
        float rx, ry, rz;
        QuatRotate(st, lpx - cx, lpy - cy, lpz - cz, rx, ry, rz);
        out[0] = st.px + rx; out[1] = st.py + ry; out[2] = st.pz + rz;
    }

    // Amanatides-Woo voxel DDA over chunk `c`'s local mats grid (box [0,dx]x..).
    // From local ray (lo,ld) (ld unit), returns the first SOLID cell + the param
    // t (world units, since the local frame is a rigid transform of the world).
    static bool LocalRaycast(const Chunk& c, const float lo[3], const float ld[3],
                             float maxDist, int& hx, int& hy, int& hz, float& tHit) {
        const int dim[3] = {c.dx, c.dy, c.dz};
        float tEnter = 0.0f, tExit = maxDist;
        for (int a = 0; a < 3; ++a) {                 // slab-clip to the grid box
            if (std::fabs(ld[a]) < 1e-8f) {
                if (lo[a] < 0.0f || lo[a] > static_cast<float>(dim[a])) return false;
            } else {
                const float inv = 1.0f / ld[a];
                float t1 = (0.0f - lo[a]) * inv, t2 = (static_cast<float>(dim[a]) - lo[a]) * inv;
                if (t1 > t2) std::swap(t1, t2);
                tEnter = std::max(tEnter, t1);
                tExit  = std::min(tExit, t2);
                if (tEnter > tExit) return false;
            }
        }
        float t = tEnter + 1e-4f;
        if (t > tExit) return false;
        float p[3] = {lo[0] + ld[0] * t, lo[1] + ld[1] * t, lo[2] + ld[2] * t};
        int   cell[3], step[3];
        float tMax[3], tDelta[3];
        for (int a = 0; a < 3; ++a) {
            cell[a] = static_cast<int>(std::floor(p[a]));
            cell[a] = std::max(0, std::min(dim[a] - 1, cell[a]));
            if (ld[a] > 1e-8f) {
                step[a] = 1; tDelta[a] = 1.0f / ld[a];
                tMax[a] = t + (static_cast<float>(cell[a] + 1) - p[a]) / ld[a];
            } else if (ld[a] < -1e-8f) {
                step[a] = -1; tDelta[a] = -1.0f / ld[a];
                tMax[a] = t + (static_cast<float>(cell[a]) - p[a]) / ld[a];
            } else {
                step[a] = 0; tDelta[a] = 1e30f; tMax[a] = 1e30f;
            }
        }
        const int guardMax = (dim[0] + dim[1] + dim[2]) * 2 + 8;
        for (int guard = 0; guard < guardMax; ++guard) {
            if (cell[0] >= 0 && cell[0] < dim[0] && cell[1] >= 0 && cell[1] < dim[1] &&
                cell[2] >= 0 && cell[2] < dim[2]) {
                if (c.mats[Idx(c, cell[0], cell[1], cell[2])] != 0) {
                    hx = cell[0]; hy = cell[1]; hz = cell[2]; tHit = t; return true;
                }
            }
            int axis = 0;
            if (tMax[1] < tMax[axis]) axis = 1;
            if (tMax[2] < tMax[axis]) axis = 2;
            t = tMax[axis];
            if (t > tExit) return false;
            cell[axis] += step[axis];
            tMax[axis] += tDelta[axis];
            if (cell[axis] < 0 || cell[axis] >= dim[axis]) return false;  // exited the box
        }
        return false;
    }

    // Erase a SPHERE of solid voxels (radius, Euclidean) around local (cx,cy,cz)
    // from chunk `c`'s mats. Returns the count removed.
    static int CarveLocalSphere(Chunk& c, int cx, int cy, int cz, int radius) {
        const int r2 = radius * radius;
        int removed = 0;
        const int z0 = std::max(0, cz - radius), z1 = std::min(c.dz - 1, cz + radius);
        const int y0 = std::max(0, cy - radius), y1 = std::min(c.dy - 1, cy + radius);
        const int x0 = std::max(0, cx - radius), x1 = std::min(c.dx - 1, cx + radius);
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const int dx = x - cx, dy = y - cy, dz = z - cz;
                    if (dx * dx + dy * dy + dz * dz > r2) continue;
                    const std::size_t i = Idx(c, x, y, z);
                    if (c.mats[i]) { c.mats[i] = 0; ++removed; }
                }
        return removed;
    }

    // Release a live chunk (its body, render object + re-stamp footprint) and
    // erase it from live_. Mirrors the cull path in Update().
    void RemoveChunkAt(std::size_t idx, vox::voxel::VoxelWorld& world,
                       vox::physics::PhysicsWorld& physics, vox::render::Renderer& renderer) {
        if (idx >= live_.size()) return;
        Chunk& c = live_[idx];
        if (c.dynHandle >= 0) renderer.RemoveDynObject(c.dynHandle);
        else if (c.hasPrev) ClearBoxToTerrain(world, renderer, c.prev);
        if (c.id >= 0) physics.ReleaseBox(c.id);
        live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(idx));
    }

    // Carve a sphere out of chunk `idx`'s local grid at local (lx,ly,lz), then
    // re-fracture: relabel connected components and spawn each as its OWN new
    // body at the parent's pose + inherited velocity (plus a gentle outward kick
    // off the carve point so split pieces separate). The parent is released.
    void CarveChunk(vox::voxel::VoxelWorld& world, vox::physics::PhysicsWorld& physics,
                    vox::render::Renderer& renderer,
                    std::size_t idx, int lx, int ly, int lz, int radius) {
        if (idx >= live_.size()) return;
        vox::physics::BodyState st;
        if (!physics.GetBodyState(live_[idx].id, st)) return;
        float plin[3] = {0, 0, 0}, pang[3] = {0, 0, 0};
        physics.GetBodyVelocity(live_[idx].id, plin, pang);

        const int removed = CarveLocalSphere(live_[idx], lx, ly, lz, radius);
        if (removed == 0) return;  // grazed empty space

        // Take the parent's grid + frame, then drop the parent body (frees a pool
        // slot for the children we're about to spawn).
        const int   pdx = live_[idx].dx, pdy = live_[idx].dy, pdz = live_[idx].dz;
        const float pcx = live_[idx].localCx, pcy = live_[idx].localCy, pcz = live_[idx].localCz;
        std::vector<std::uint8_t> pmats = std::move(live_[idx].mats);
        RemoveChunkAt(idx, world, physics, renderer);

        vox::destruction::ComponentField cf =
            vox::destruction::labelComponents(pmats.data(), glm::ivec3(pdx, pdy, pdz), nullptr);
        if (cf.totalCount <= 0) return;  // fully destroyed -> nothing respawns

        float cw[3];  // world carve point (for the separation kick)
        LocalGridPointToWorld(st, pcx, pcy, pcz, static_cast<float>(lx) + 0.5f,
                              static_cast<float>(ly) + 0.5f, static_cast<float>(lz) + 0.5f, cw);

        for (int comp = 1; comp <= cf.totalCount; ++comp) {
            if (static_cast<int>(live_.size()) >= maxLive_) break;  // live cap
            vox::destruction::LocalGrid g = vox::destruction::extractIsland(
                pmats.data(), cf, glm::ivec3(pdx, pdy, pdz), static_cast<std::uint16_t>(comp));
            if (g.dims.x <= 0 || g.dims.y <= 0 || g.dims.z <= 0) continue;

            Chunk ch;
            ch.dx = g.dims.x; ch.dy = g.dims.y; ch.dz = g.dims.z;
            ch.mats = std::move(g.data);
            ch.solidCount = 0; for (std::uint8_t m : ch.mats) if (m) ++ch.solidCount;
            if (ch.solidCount == 0) continue;
            ch.localCx = 0.5f * ch.dx; ch.localCy = 0.5f * ch.dy; ch.localCz = 0.5f * ch.dz;

            float wc[3];  // child body center = parent W(originInParent + childC)
            LocalGridPointToWorld(st, pcx, pcy, pcz,
                                  static_cast<float>(g.originInParent.x) + ch.localCx,
                                  static_cast<float>(g.originInParent.y) + ch.localCy,
                                  static_cast<float>(g.originInParent.z) + ch.localCz, wc);

            float ox = wc[0] - cw[0], oy = wc[1] - cw[1], oz = wc[2] - cw[2];
            const float ol = std::sqrt(ox * ox + oy * oy + oz * oz);
            if (ol > 1e-3f) { ox /= ol; oy /= ol; oz /= ol; }
            else { ox = Frand(-1.0f, 1.0f); oy = 1.0f; oz = Frand(-1.0f, 1.0f); }
            const float kick = 2.0f;
            const float vx = plin[0] + ox * kick, vy = plin[1] + oy * kick, vz = plin[2] + oz * kick;

            const int newId = AcquireForChunk(physics, ch, wc[0], wc[1], wc[2],
                                              vx, vy, vz, pang[0], pang[1], pang[2]);
            if (newId < 0) break;  // pool exhausted
            ch.id = newId;
            physics.SetBodyPose(newId, wc, &st.qx);   // children keep the parent's rotation
            if (UseObb()) RegisterDynObject(renderer, world, ch);
            live_.push_back(std::move(ch));
        }
    }

    std::vector<std::uint32_t> scratch_;  // serial-only (main-thread) upload buffer
};
#endif  // VOX_HAVE_PHYSX

// ---------------------------------------------------------------------------
// Milestone B1: structural settle. Detach + drop any voxels no longer connected
// to the ground, reusing the B0 connectivity/island-extract algorithms
// (engine/voxel/StructuralSettle) and the existing DebrisField rigid-body spawn.
//
// 1) vox::voxel::SettleWorld() bakes the world, floods from the ground anchors,
//    finds the UNANCHORED islands, CLEARS their voxels from the live world and
//    returns each island's tight local grid + world origin.
// 2) For each detached island we spawn a falling debris body (PhysX builds);
//    in the stub build the voxels are simply removed (no rigid body).
// 3) If anything detached we re-bake + renderer.SetVoxels so the world updates.
//
// Returns the number of voxels detached this pass (0 = nothing floated).
static void MarkRegionsDirtyForAabb(int x0, int y0, int z0, int x1, int y1, int z1);  // fwd-decl (defined with the region markers below)
#if defined(VOX_HAVE_PHYSX)
// Phase 1: box-graph structural settle + the persistent box-layer total (both
// defined after the box layer below). Forward-declared so RunStructuralSettle
// (which the commands call) can dispatch to / query them.
static std::size_t BoxLayerTotal();
static int RunStructuralSettleBoxGraph(vox::console::Console& cc, vox::voxel::VoxelWorld& world,
                                       vox::render::Renderer& renderer,
                                       vox::physics::PhysicsWorld& physics, DebrisField& debris,
                                       int anchorLayers);
#endif

int RunStructuralSettle(vox::console::Console& cc,
                        vox::voxel::VoxelWorld& world,
                        vox::render::Renderer& renderer
#if defined(VOX_HAVE_PHYSX)
                        , vox::physics::PhysicsWorld& physics,
                        DebrisField& debris
#endif
) {
    const int anchorLayers = cc.FindCVar("voxel.anchor_layers")
                                 ? cc.FindCVar("voxel.anchor_layers")->GetInt() : 1;
#if defined(VOX_HAVE_PHYSX)
    // Phase 1: prefer the box-graph settle when enabled AND the persistent box
    // layer is current+populated (it rides the world_collider path, and the main
    // loop only fires settle once that layer has caught up). Else fall back to the
    // proven per-voxel SettleWorld below.
    {
        CVar* bg = cc.FindCVar("voxel.settle_boxgraph");
        CVar* wc = cc.FindCVar("physics.world_collider");
        const bool useBoxGraph = bg && bg->GetBool() && wc && wc->GetBool() && BoxLayerTotal() > 0;
        if (useBoxGraph)
            return RunStructuralSettleBoxGraph(cc, world, renderer, physics, debris, anchorLayers);
    }
#endif
    vox::voxel::SettleResult res =
        vox::voxel::SettleWorld(world, vox::voxel::kWorldDim, anchorLayers);
    if (res.detachedVoxels == 0) return 0;  // nothing floats; world untouched

#if defined(VOX_HAVE_PHYSX)
    // Spawn each detached island as a falling rigid body (its voxels are
    // already cleared from the world by SettleWorld). Pool is sized to
    // voxel.debris.max as elsewhere.
    debris.SetMaxLive(cc.FindCVar("voxel.debris.max") ? cc.FindCVar("voxel.debris.max")->GetInt() : 256);
    debris.SetScale(cc.FindCVar("voxel.debris.scale") ? cc.FindCVar("voxel.debris.scale")->GetFloat() : 1.0f);
    debris.ReservePool(physics);
    for (const auto& isl : res.islands) {
        debris.SpawnIsland(physics, renderer, world, isl.data, isl.dims.x, isl.dims.y, isl.dims.z,
                           isl.origin.x, isl.origin.y, isl.origin.z);
        // Those world cells were just cleared -> rebuild ONLY the islands' regions (not all 64).
        MarkRegionsDirtyForAabb(isl.origin.x, isl.origin.y, isl.origin.z,
                                isl.origin.x + isl.dims.x, isl.origin.y + isl.dims.y, isl.origin.z + isl.dims.z);
    }
#endif

    // Push the now-cleared island cells to the GPU INCREMENTALLY (EditVoxels per
    // island AABB), NOT a full re-bake. Renderer::SetVoxels does a GPU WaitIdle
    // (full pipeline stall) AND rescans all kWorldDim^3 (=2M) voxels to rebuild
    // the empty-space brick grid -- that stall, not the physics, was the settle
    // hitch (carve never stutters precisely because it uses EditVoxels). EditVoxels
    // writes only the touched cells + refreshes only the overlapping bricks, no
    // WaitIdle. Read the CURRENT world state for each box (island voxels already
    // cleared by SettleWorld; any neighbouring terrain in the AABB is preserved).
    std::vector<std::uint32_t> region;
    for (const auto& isl : res.islands) {
        const int x0 = isl.origin.x, y0 = isl.origin.y, z0 = isl.origin.z;
        const int x1 = x0 + isl.dims.x, y1 = y0 + isl.dims.y, z1 = z0 + isl.dims.z;
        region.clear();
        region.reserve(static_cast<std::size_t>(isl.dims.x) * isl.dims.y * isl.dims.z);
        for (int z = z0; z < z1; ++z)
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    region.push_back(static_cast<std::uint32_t>(world.GetVoxel(x, y, z)));
        renderer.EditVoxels(x0, y0, z0, x1, y1, z1, region.data(), nullptr);
    }
    return res.detachedVoxels;
}

#if defined(VOX_HAVE_PHYSX)
// Milestone C: (re)build the STATIC world box-compound collider from the live
// voxel terrain so falling debris (A3 OBB chunks) and detached islands (B1
// settle) LAND on the visible terrain instead of falling through to the y=0
// plane / kill plane.
//
// Approach (per the spec): a GREEDY BOX COVER of the solid voxels -> a compound
// of PxBoxGeometry shapes. NOT per-voxel, NOT a heightfield -- a flat floor
// becomes a handful of boxes, a wall a few; concavity/holes are preserved
// (union of boxes). The cover is computed PER REGION (see below) so a carve only
// rebuilds the few regions it touched, not the whole world.
//
// World == voxel grid coords (voxel size 1), matching how AcquireBox/debris use
// world units, so a voxel at integer (x,y,z) occupies the world box
// [x..x+1] x [y..y+1] x [z..z+1]. box_decompose returns INCLUSIVE voxel AABBs
// [mn..mx], so the world box is [mn .. mx+1].
//
// A whole-world rebuild (bake -> decompose 2M cells -> rebuild one giant PhysX
// compound) is expensive (~1s stutter), AND even with the decompose pushed
// off-thread the PhysX APPLY of every box is still O(world) on the main thread.
// So the collider is partitioned into a fixed grid of cubic REGIONS and we only
// re-decompose + re-apply the regions a carve/settle actually TOUCHED:
//
//   * REGION GRID: kRegionSize-voxel cubes; kWorldDim % kRegionSize == 0 so the
//     world is exactly kRegionGrid^3 regions. Each region owns its own static
//     box-compound PxRigidStatic (PhysicsWorld::SetColliderRegion). A long floor
//     becomes a few region-local boxes instead of one -- acceptable; boxes never
//     span a region boundary so a per-region rebuild stays independent.
//   * DIRTY SET: a file-scope std::vector<bool> over the region grid. A carve
//     sphere marks every region overlapping its AABB [h-r-1 .. h+r+1]; a settle
//     marks ALL regions (it can detach anywhere). Coordinate convention is
//     unchanged: voxel x -> world box [x..x+1]; region (rx,ry,rz) covers voxels
//     [r*kRegionSize .. (r+1)*kRegionSize); its sub-grid is decomposed locally
//     then offset by the region world origin.
//   * DEBOUNCE: the main loop kicks a rebuild only once carving has paused a few
//     quiet frames, not per carve.
//   * ASYNC, batched per region (so the rebuild never stutters the main thread):
//       1. MAIN: snapshot the dirty regions' sub-grids out of the live world
//          (bake must be main-thread to avoid racing a carve) into one job and
//          launch a worker; mark "in flight". Does NOT block. The dirty bits for
//          the snapshotted regions are cleared now (any region re-dirtied while
//          the job runs stays dirty -> processed next round).
//       2. WORKER: for EACH region in the batch, decomposeComponent over its
//          local sub-grid -> world-unit ColliderBoxes offset by the region
//          origin. NO PhysX. Sets the atomic `ready` flag last.
//       3. MAIN: poll `ready`; when set, join + apply each region's boxes via
//          SetColliderRegion (PhysX -> main-thread only), then clear in-flight.
//       4. COALESCE: only one batch is ever in flight; regions dirtied meanwhile
//          are picked up by the next batch, so it converges without piling up.
constexpr int kRegionSize = 32;  // kWorldDim(128) % 32 == 0 -> 4x4x4 = 64 regions
constexpr int kRegionGrid = static_cast<int>(vox::voxel::kWorldDim) / kRegionSize;
constexpr int kRegionCount = kRegionGrid * kRegionGrid * kRegionGrid;

// Dense region index (matches the row-major idx convention z*Y*X + y*X + x).
static inline int RegionIndex(int rx, int ry, int rz) {
    return (rz * kRegionGrid + ry) * kRegionGrid + rx;
}

static int  g_colliderQuietFrames = 0;
// Dirty bit per region; any true bit drives a rebuild once carving pauses.
static std::vector<bool> g_colliderRegionDirty(kRegionCount, false);
static bool AnyRegionDirty() {
    for (bool d : g_colliderRegionDirty) if (d) return true;
    return false;
}

// Phase 0 (box layer): one greedy box of the persistent world decomposition --
// the SAME boxes the collider builds, but kept (not discarded) in WORLD voxel
// coords + a representative material, so connectivity (Phase 1) + stress
// (Phase 3) run on the boxes instead of a per-voxel BFS over the dense grid.
struct WorldBox {
    vox::destruction::Box box;     // inclusive AABB, WORLD voxel coords
    std::uint8_t          material = 0;  // sampled at box.mn (boxes are solid-only)
};

// One region's snapshot handed to the worker: its dense index, world origin and
// a kRegionSize^3 uint8 sub-grid (row-major, region-local coords; holds the
// MATERIAL id per cell, 0 = empty -- nonzero = solid).
struct ColliderRegionJobItem {
    int index = 0;
    int ox = 0, oy = 0, oz = 0;  // region world origin (voxel coords)
    bool anySolid = false;
    int  solidCount = 0;                                  // # nonzero cells (box-layer invariant check)
    std::vector<std::uint8_t> grid;                       // region-local MATERIAL ids (0 = empty)
    std::vector<vox::physics::ColliderBox> boxes;         // worker writes (world units) -> PhysX
    std::vector<WorldBox>     worldBoxes;                 // worker writes (world voxel coords) -> persistent box layer
};

// --- async-rebuild state (file-scope; the batch outlives the worker since it is
// static and we gate re-launch on g_colliderJobInFlight) ---
static bool                          g_colliderJobInFlight = false;  // main-thread only
static std::atomic<bool>             g_colliderJobReady{false};      // worker -> main
static std::thread                   g_colliderThread;               // one worker at a time
static std::vector<ColliderRegionJobItem> g_colliderBatch;          // main writes, worker reads/writes

// Phase 0: the PERSISTENT world box layer, indexed by dense region index. Each
// region holds its current greedy box decomposition (world voxel coords) +
// per-box material. Written by PollColliderJob from the just-finished worker
// batch (so it stays in lockstep with the collider on the dirty-region cadence),
// read by the box-level connectivity/stress passes. Empty vector = cleared region.
static std::vector<std::vector<WorldBox>> g_worldRegionBoxes(kRegionCount);

// Total boxes currently in the persistent layer (verification / Phase-1 sizing).
static std::size_t BoxLayerTotal() {
    std::size_t n = 0;
    for (const auto& r : g_worldRegionBoxes) n += r.size();
    return n;
}

// Mark every region overlapping the carve sphere's AABB [h-r-1 .. h+r+1] dirty.
static void MarkCarveRegionsDirty(int hx, int hy, int hz, int radius) {
    const int G = static_cast<int>(vox::voxel::kWorldDim);
    const int pad = radius + 1;  // +1 covers the [x..x+1] world-box of edge voxels
    const int x0 = std::max(0, hx - pad), x1 = std::min(G - 1, hx + pad);
    const int y0 = std::max(0, hy - pad), y1 = std::min(G - 1, hy + pad);
    const int z0 = std::max(0, hz - pad), z1 = std::min(G - 1, hz + pad);
    if (x1 < x0 || y1 < y0 || z1 < z0) return;  // AABB entirely outside the world
    const int rx0 = x0 / kRegionSize, rx1 = x1 / kRegionSize;
    const int ry0 = y0 / kRegionSize, ry1 = y1 / kRegionSize;
    const int rz0 = z0 / kRegionSize, rz1 = z1 / kRegionSize;
    for (int rz = rz0; rz <= rz1; ++rz)
        for (int ry = ry0; ry <= ry1; ++ry)
            for (int rx = rx0; rx <= rx1; ++rx)
                g_colliderRegionDirty[RegionIndex(rx, ry, rz)] = true;
    g_colliderQuietFrames = 0;
}

// Mark every region overlapping a world-voxel AABB dirty (structural settle: only the
// detached islands' cells changed, so only those regions need a collider rebuild).
static void MarkRegionsDirtyForAabb(int x0, int y0, int z0, int x1, int y1, int z1) {
    const int G = static_cast<int>(vox::voxel::kWorldDim);
    x0 = std::max(0, x0); y0 = std::max(0, y0); z0 = std::max(0, z0);
    x1 = std::min(G - 1, x1); y1 = std::min(G - 1, y1); z1 = std::min(G - 1, z1);
    if (x1 < x0 || y1 < y0 || z1 < z0) return;
    for (int rz = z0 / kRegionSize; rz <= z1 / kRegionSize; ++rz)
        for (int ry = y0 / kRegionSize; ry <= y1 / kRegionSize; ++ry)
            for (int rx = x0 / kRegionSize; rx <= x1 / kRegionSize; ++rx)
                g_colliderRegionDirty[RegionIndex(rx, ry, rz)] = true;
    g_colliderQuietFrames = 0;
}

// Mark EVERY region dirty (initial build).
static void MarkAllRegionsDirty() {
    g_colliderRegionDirty.assign(kRegionCount, true);
    g_colliderQuietFrames = 0;
}

// --- Milestone B1 settle debounce ----------------------------------------
// A carve with voxel.auto_settle ON used to run the full-grid connectivity
// flood (RunStructuralSettle: O(2*kWorldDim^3)) SYNCHRONOUSLY inside the carve
// command, hitching on every single carve. Instead the carve just arms
// g_settlePending; the main loop runs ONE settle a few quiet frames after
// carving stops -- coalescing a burst of carves into a single flood (mirrors
// the dirty-region collider debounce above).
static bool g_settlePending = false;
static int  g_settleQuietFrames = 0;

// Compatibility shim kept for the (rare) call sites that want a full rebuild:
// equivalent to MarkAllRegionsDirty(). The main loop drives the async rebuild.
void RebuildWorldCollider(vox::voxel::VoxelWorld&, vox::physics::PhysicsWorld&) {
    MarkAllRegionsDirty();
}

// Pure-CPU worker body: for each region in the batch, single-id ComponentField
// over its local sub-grid -> decomposeComponent -> world-unit ColliderBoxes
// (offset by the region origin). Touches ONLY g_colliderBatch (no live world, no
// PhysX). Sets `ready` last.
static void ColliderDecomposeJob() {
    const glm::ivec3 rdims(kRegionSize, kRegionSize, kRegionSize);
    for (ColliderRegionJobItem& item : g_colliderBatch) {
        item.boxes.clear();
        item.worldBoxes.clear();
        if (!item.anySolid) continue;  // empty region -> SetColliderRegion + box layer clear it
        vox::destruction::ComponentField cf;
        cf.ids.resize(item.grid.size());
        for (std::size_t i = 0; i < item.grid.size(); ++i)
            cf.ids[i] = item.grid[i] ? std::uint16_t{1} : std::uint16_t{0};
        cf.anchoredCount = 1;
        cf.totalCount = 1;

        const std::vector<vox::destruction::Box> boxes =
            vox::destruction::decomposeComponent(cf, rdims, 1);

        // Inclusive region-local voxel AABB -> (a) world-unit ColliderBox [mn .. mx+1]
        // for PhysX, and (b) a WORLD-voxel-coord WorldBox [mn .. mx] (+ material) for
        // the persistent box layer (connectivity/stress). region idx = z*S*S + y*S + x.
        item.boxes.reserve(boxes.size());
        item.worldBoxes.reserve(boxes.size());
        std::int64_t coveredVol = 0;
        for (const auto& b : boxes) {
            vox::physics::ColliderBox cb;
            cb.minX = static_cast<float>(item.ox + b.mn.x);
            cb.minY = static_cast<float>(item.oy + b.mn.y);
            cb.minZ = static_cast<float>(item.oz + b.mn.z);
            cb.maxX = static_cast<float>(item.ox + b.mx.x + 1);
            cb.maxY = static_cast<float>(item.oy + b.mx.y + 1);
            cb.maxZ = static_cast<float>(item.oz + b.mx.z + 1);
            item.boxes.push_back(cb);

            WorldBox wb;
            wb.box.mn = glm::ivec3(item.ox + b.mn.x, item.oy + b.mn.y, item.oz + b.mn.z);
            wb.box.mx = glm::ivec3(item.ox + b.mx.x, item.oy + b.mx.y, item.oz + b.mx.z);
            const std::size_t li =
                (static_cast<std::size_t>(b.mn.z) * kRegionSize + b.mn.y) * kRegionSize + b.mn.x;
            wb.material = item.grid[li];   // box covers solids only, so mn corner is solid
            item.worldBoxes.push_back(wb);
            coveredVol += b.volume();
        }
        // Invariant (decomposeComponent guarantees union==solids, non-overlapping):
        // covered volume must equal the region's solid-cell count. Sanity net only.
        if (coveredVol != item.solidCount)
            vox::log::Warn("box layer: region {} cover {} != solids {}", item.index,
                           static_cast<long long>(coveredVol), item.solidCount);
    }
    g_colliderJobReady.store(true, std::memory_order_release);  // publish (must be last)
}

// MAIN THREAD: snapshot every currently-dirty region's sub-grid out of the live
// world into one batch and launch the worker. Clears the snapshotted regions'
// dirty bits (a region re-dirtied while the job runs stays dirty -> next round).
// Caller must guarantee no job is in flight (coalescing handled by the loop).
static void LaunchColliderJob(vox::voxel::VoxelWorld& world) {
    g_colliderBatch.clear();
    for (int rz = 0; rz < kRegionGrid; ++rz) {
        for (int ry = 0; ry < kRegionGrid; ++ry) {
            for (int rx = 0; rx < kRegionGrid; ++rx) {
                const int idx = RegionIndex(rx, ry, rz);
                if (!g_colliderRegionDirty[idx]) continue;
                g_colliderRegionDirty[idx] = false;  // snapshotting now

                ColliderRegionJobItem item;
                item.index = idx;
                item.ox = rx * kRegionSize;
                item.oy = ry * kRegionSize;
                item.oz = rz * kRegionSize;
                item.grid.assign(static_cast<std::size_t>(kRegionSize) * kRegionSize * kRegionSize, 0u);
                int solidCount = 0;
                for (int lz = 0; lz < kRegionSize; ++lz) {
                    for (int ly = 0; ly < kRegionSize; ++ly) {
                        for (int lx = 0; lx < kRegionSize; ++lx) {
                            const std::uint8_t m = world.GetVoxel(item.ox + lx, item.oy + ly, item.oz + lz);
                            if (m != 0) {
                                const std::size_t li =
                                    (static_cast<std::size_t>(lz) * kRegionSize + ly) * kRegionSize + lx;
                                item.grid[li] = m;   // store MATERIAL (nonzero = solid); box layer samples it
                                ++solidCount;
                            }
                        }
                    }
                }
                item.anySolid = solidCount > 0;
                item.solidCount = solidCount;
                g_colliderBatch.push_back(std::move(item));
            }
        }
    }
    if (g_colliderBatch.empty()) return;  // nothing to do (shouldn't happen: gated on AnyRegionDirty)

    g_colliderJobReady.store(false, std::memory_order_relaxed);
    g_colliderJobInFlight = true;
    g_colliderThread = std::thread(&ColliderDecomposeJob);  // joined when ready (or on shutdown)
}

// MAIN THREAD: poll the in-flight worker; when it has published a result, join
// it and apply each region's boxes via PhysX (main-thread only), then clear
// in-flight. Returns true if a result was applied this call.
static bool PollColliderJob(vox::physics::PhysicsWorld& physics) {
    if (!g_colliderJobInFlight) return false;
    if (!g_colliderJobReady.load(std::memory_order_acquire)) return false;

    if (g_colliderThread.joinable()) g_colliderThread.join();
    g_colliderJobInFlight = false;

    std::size_t totalBoxes = 0;
    for (ColliderRegionJobItem& item : g_colliderBatch) {
        physics.SetColliderRegion(item.index, kRegionCount, item.boxes);  // empty -> clears region
        // Phase 0: persist this region's box decomposition (empty = cleared region).
        if (item.index >= 0 && item.index < static_cast<int>(g_worldRegionBoxes.size()))
            g_worldRegionBoxes[item.index] = std::move(item.worldBoxes);
        totalBoxes += item.boxes.size();
    }
    vox::log::Trace("physics: world collider rebuilt ({} region(s), {} boxes; box-layer total {})",
                    g_colliderBatch.size(), totalBoxes, BoxLayerTotal());
    g_colliderBatch.clear();
    return true;
}

// Shutdown: join any in-flight worker so it doesn't outlive the static buffers.
static void JoinColliderJob() {
    if (g_colliderThread.joinable()) g_colliderThread.join();
    g_colliderJobInFlight = false;
}

// Phase 1: structural settle via box-graph union-find. Reads the persistent box
// layer (the main loop only fires settle once the collider/box rebuild has
// drained, so it's current), labels connected components, and drops every
// component NOT anchored to the bottom `anchorLayers` voxel rows. Connectivity is
// O(boxes) union-find over the layer instead of the per-voxel BFS's O(kWorldDim^3).
// Times the label pass + total and logs them so we can see whether the O(n^2)
// labelBoxComponents needs the spatial-grid speedup at this box count.
static int RunStructuralSettleBoxGraph(vox::console::Console& cc, vox::voxel::VoxelWorld& world,
                                       vox::render::Renderer& renderer,
                                       vox::physics::PhysicsWorld& physics, DebrisField& debris,
                                       int anchorLayers) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    // 1) Flatten the persistent box layer (world voxel coords).
    std::vector<vox::destruction::Box> boxes;
    boxes.reserve(BoxLayerTotal());
    for (const auto& region : g_worldRegionBoxes)
        for (const WorldBox& wb : region) boxes.push_back(wb.box);
    if (boxes.empty()) return 0;

    // 2) Union-find components + anchored set (boxes touching the ground rows).
    const auto t1 = clock::now();
    const vox::destruction::BoxComponents comps = vox::destruction::labelBoxComponents(boxes);
    const auto t2 = clock::now();
    std::vector<int> anchorIdx;
    for (std::size_t i = 0; i < boxes.size(); ++i)
        if (boxes[i].mn.y < anchorLayers) anchorIdx.push_back(static_cast<int>(i));
    const std::vector<bool> anchored = vox::destruction::anchoredComponents(comps, anchorIdx);
    if (comps.count <= 0) return 0;

    // 3) Per-component world AABB for the UNANCHORED components.
    struct Acc { glm::ivec3 mn{0}, mx{0}; bool init = false; };
    std::vector<Acc> acc(static_cast<std::size_t>(comps.count));
    bool anyDetached = false;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const int ci = comps.labels[i];
        if (ci < 0 || ci >= comps.count || anchored[ci]) continue;
        Acc& a = acc[static_cast<std::size_t>(ci)];
        if (!a.init) { a.mn = boxes[i].mn; a.mx = boxes[i].mx; a.init = true; }
        else { a.mn = glm::min(a.mn, boxes[i].mn); a.mx = glm::max(a.mx, boxes[i].mx); }
        anyDetached = true;
    }
    if (!anyDetached) return 0;  // everything reaches the ground

    debris.SetMaxLive(cc.FindCVar("voxel.debris.max") ? cc.FindCVar("voxel.debris.max")->GetInt() : 256);
    debris.SetScale(cc.FindCVar("voxel.debris.scale") ? cc.FindCVar("voxel.debris.scale")->GetFloat() : 1.0f);
    debris.ReservePool(physics);

    // 4) Each unanchored component -> a tight island grid (only its own box cells,
    //    so concavity is preserved) -> spawn as a falling body -> clear its cells
    //    -> incremental GPU upload of its AABB (no full re-bake/stall).
    int detached = 0, islands = 0;
    std::vector<std::uint32_t> upload;
    for (int ci = 0; ci < comps.count; ++ci) {
        if (anchored[ci] || !acc[static_cast<std::size_t>(ci)].init) continue;
        const glm::ivec3 mn = acc[static_cast<std::size_t>(ci)].mn;
        const glm::ivec3 mx = acc[static_cast<std::size_t>(ci)].mx;
        const int dx = mx.x - mn.x + 1, dy = mx.y - mn.y + 1, dz = mx.z - mn.z + 1;
        std::vector<std::uint8_t> mats(static_cast<std::size_t>(dx) * dy * dz, 0u);
        int cells = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (comps.labels[i] != ci) continue;
            const vox::destruction::Box& b = boxes[i];
            for (int z = b.mn.z; z <= b.mx.z; ++z)
                for (int y = b.mn.y; y <= b.mx.y; ++y)
                    for (int x = b.mn.x; x <= b.mx.x; ++x) {
                        const std::uint8_t m = world.GetVoxel(x, y, z);
                        if (!m) continue;
                        mats[(static_cast<std::size_t>(z - mn.z) * dy + (y - mn.y)) * dx + (x - mn.x)] = m;
                        ++cells;
                    }
        }
        if (cells == 0) continue;
        debris.SpawnIsland(physics, renderer, world, mats, dx, dy, dz, mn.x, mn.y, mn.z);
        for (std::size_t i = 0; i < boxes.size(); ++i) {  // clear this component's cells
            if (comps.labels[i] != ci) continue;
            const vox::destruction::Box& b = boxes[i];
            for (int z = b.mn.z; z <= b.mx.z; ++z)
                for (int y = b.mn.y; y <= b.mx.y; ++y)
                    for (int x = b.mn.x; x <= b.mx.x; ++x)
                        world.SetVoxel(x, y, z, 0);
        }
        upload.clear();
        upload.reserve(static_cast<std::size_t>(dx) * dy * dz);
        for (int z = mn.z; z <= mx.z; ++z)
            for (int y = mn.y; y <= mx.y; ++y)
                for (int x = mn.x; x <= mx.x; ++x)
                    upload.push_back(static_cast<std::uint32_t>(world.GetVoxel(x, y, z)));
        renderer.EditVoxels(mn.x, mn.y, mn.z, mx.x + 1, mx.y + 1, mx.z + 1, upload.data(), nullptr);
        MarkRegionsDirtyForAabb(mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
        detached += cells;
        ++islands;
    }

    const auto t3 = clock::now();
    const auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    vox::log::Info("settle(box-graph): {} boxes / {} comps -> {} island(s), {} vox | label {:.2f}ms total {:.2f}ms",
                   boxes.size(), comps.count, islands, detached, ms(t2 - t1), ms(t3 - t0));
    return detached;
}

// True iff the world-collider / box-layer rebuild has fully drained (no dirty
// regions, no worker in flight) -- i.e. the persistent box layer is CURRENT. The
// main loop gates the (debounced) settle on this so box-graph settle never reads
// a stale layer. Trivially true in the no-PhysX build.
static bool ColliderLayerIdle() {
    return !AnyRegionDirty() && !g_colliderJobInFlight;
}
#endif  // VOX_HAVE_PHYSX
#if !defined(VOX_HAVE_PHYSX)
static bool ColliderLayerIdle() { return true; }
#endif

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
    reg("renderer.test_object", "0", "EXPERIMENTAL (Milestone A2): spawn a rotating test voxel cube near world center, drawn through the OBB-raster path + depth-composited with the world (proves multi-object rendering). Needs renderer.gbuffer.obb 1 + QUALITY + a denoiser (ATROUS/SVGF).", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.debris_obb", "0", "EXPERIMENTAL (Milestone A3): render fracture debris as independent OBB voxel rigid bodies (smooth rotation) instead of re-stamping them into the world grid. Needs renderer.gbuffer.obb 1 + QUALITY + a denoiser. OFF (default) = the proven re-stamp path.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.empty_space_skip", "1", "Skip empty bricks in the voxel raymarch (faster; visually identical). Off = per-voxel DDA.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("renderer.gi.debug", "0", "QUALITY debug: show ONLY the indirect GI bounce (no direct/albedo) to confirm GI is contributing.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("physics.gpu_rigids.enabled", "1", "GPU rigid bodies (NVIDIA).", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("physics.gpu_rigids.max_islands", "10000", "Max active dynamic islands.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 256, .range_max = 16384, .range_step = 256});
    reg("physics.solver.position_iters", "8", "Solver position iterations.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 32, .range_step = 1});
    reg("physics.world_collider", "0", "Milestone C: build a STATIC box-compound collider matching the voxel terrain (greedy box cover -> one PxRigidStatic of PxBoxGeometry) so falling debris + B1 detached islands LAND on the terrain instead of dropping through to the y=0 plane. Default OFF = behavior unchanged. ON rebuilds the collider after each carve (voxel.break/explode) + after voxel.settle. Additive to the y=0 ground plane.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("physics.debris_compound", "1", "#2 of Teardown ladder: give each debris/island chunk a box-COMPOUND collider (greedy box cover of ITS OWN voxels, B0 decompose) instead of one oversized bounding box, so a CONCAVE chunk collides as its real shape and can fall THROUGH a gap a bounding box would wedge in (fixes 'a chunk hit an invisible box in a gap'). Solid (convex) chunks keep a single box; chunks needing >48 boxes fall back to one box (cost). Default ON; matters once physics.world_collider is on.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("voxel.debris_breakable", "1", "Broken stays breakable: voxel.break / voxel.explode also ray-cast the camera ray against live debris chunks; if a chunk is the nearest thing hit, it is carved in its OWN local frame and RE-FRACTURED (the carve relabels connected components and each piece becomes its own rigid body, inheriting the parent's pose + velocity). Default ON; turn OFF to only ever carve the static world.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
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
    reg("voxel.debris.ttl", "0", "Seconds before debris auto-cleans. 0 = PERSIST (debris stay where they land; only culled when they fall off the world). Higher live counts cost render+physics -- lower voxel.debris.max if it slows.", {.type = CVarType::Float, .flags = CVAR_ARCHIVE, .range_min = 0.0f, .range_max = 300.0f, .range_step = 5.0f});
    reg("voxel.auto_settle", "0", "Structural settle (Milestone B1): after EVERY carve (voxel.break/explode), auto-detach any voxels no longer connected to the ground and drop them as debris. Default OFF = carve behavior unchanged; turn ON to make the world 'nothing floats'. (Full-grid flood per carve; use voxel.settle to run it manually.)", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("voxel.settle_boxgraph", "1", "Phase 1: run structural settle via box-graph union-find over the persistent box layer (O(boxes)) instead of the per-voxel BFS over the dense grid (O(kWorldDim^3)). Requires physics.world_collider on (it maintains the layer); falls back to the per-voxel SettleWorld when off or the layer is empty. Logs label/total timing each settle. Default ON.", {.type = CVarType::Bool, .flags = CVAR_ARCHIVE});
    reg("voxel.anchor_layers", "1", "Structural settle: number of bottom grid rows (y < N) treated as GROUND anchors. A solid voxel in these layers anchors its whole connected component; anything with no path down to them detaches. 1 = only the y==0 floor.", {.type = CVarType::Int, .flags = CVAR_ARCHIVE, .range_min = 1, .range_max = 32, .range_step = 1});
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

    // Milestone C: build the static world box-compound collider once now that
    // the terrain is generated/loaded, IF physics.world_collider is ON. Default
    // OFF -> behavior unchanged (only the y=0 ground plane). It is rebuilt after
    // each carve + settle below.
    if (CVar* wc = console.FindCVar("physics.world_collider"); wc && wc->GetBool()) {
        RebuildWorldCollider(world, physics);
    }
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
#if defined(VOX_HAVE_PHYSX)
                // Milestone C: an empty world has no terrain -> drop the collider
                // (the y=0 ground plane remains). No-op if it was never built.
                physics.ClearWorldCollider();
#endif
                o.Print("voxel world cleared");
            });

        // voxel.settle (Milestone B1): detach + drop any voxels not connected to
        // the ground. Runs the structural-settle pass once: connectivity flood
        // from the y<voxel.anchor_layers ground anchors -> any unanchored island
        // detaches, is cleared from the world and spawned as falling debris.
        c.RegisterCommand("voxel.settle",
            "Detach + drop any voxels not connected to the ground (structural settle).",
#if defined(VOX_HAVE_PHYSX)
            [&world, &renderer, &physics, &debris](std::span<const std::string_view>, Output& o) {
                const int n = RunStructuralSettle(Console::Get(), world, renderer, physics, debris);
#else
            [&world, &renderer](std::span<const std::string_view>, Output& o) {
                const int n = RunStructuralSettle(Console::Get(), world, renderer);
#endif
                if (n == 0) o.Print("voxel.settle: nothing floats (all voxels reach the ground)");
                else o.Format("voxel.settle: detached {} voxel(s) as falling debris", n);
#if defined(VOX_HAVE_PHYSX)
                // Milestone C: rebuild the static world collider so the dropped
                // islands land on the settled terrain (collider gated OFF by default).
                if (n > 0 && Console::Get().FindCVar("physics.world_collider") &&
                    Console::Get().FindCVar("physics.world_collider")->GetBool()) {
                    RebuildWorldCollider(world, physics);
                }
#endif
            });

#if defined(VOX_HAVE_PHYSX)
        // Phase 0 (box layer): report the persistent world box decomposition --
        // box count vs the dense voxel count, so you can see the sparsity win and
        // confirm it's populated. Rides the collider path: needs
        // physics.world_collider ON (Phase 1 will decouple it from the collider).
        c.RegisterCommand("voxel.boxlayer",
            "Report persistent world box-layer stats (boxes vs voxels; needs physics.world_collider on).",
            [&world](std::span<const std::string_view>, Output& o) {
                const std::size_t total = BoxLayerTotal();
                int nonEmpty = 0;
                for (const auto& r : g_worldRegionBoxes) if (!r.empty()) ++nonEmpty;
                std::size_t solids = 0;
                const int G = static_cast<int>(vox::voxel::kWorldDim);
                for (int z = 0; z < G; ++z)
                    for (int y = 0; y < G; ++y)
                        for (int x = 0; x < G; ++x)
                            if (world.GetVoxel(x, y, z) != 0) ++solids;
                if (total == 0)
                    o.Print("box layer: empty (enable physics.world_collider so it populates)");
                else
                    o.Format("box layer: {} boxes across {}/{} regions, covering {} solid voxels (~{}x sparser)",
                             total, nonEmpty, kRegionCount, solids,
                             total ? (solids / total) : 0);
            });
#endif

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

                const int radius = cc.FindCVar("voxel.break_radius")
                                       ? cc.FindCVar("voxel.break_radius")->GetInt() : 2;
                int hx = 0, hy = 0, hz = 0;
                const float maxDist = static_cast<float>(vox::voxel::kWorldDim) * 2.0f;
                const bool worldHit = world.RaycastSolid(pos, fwd, maxDist, hx, hy, hz);
                float worldHitDist = 1e30f;
                if (worldHit) {
                    const float wdx = (hx + 0.5f) - pos[0], wdy = (hy + 0.5f) - pos[1], wdz = (hz + 0.5f) - pos[2];
                    worldHitDist = std::sqrt(wdx * wdx + wdy * wdy + wdz * wdz);
                }
#if defined(VOX_HAVE_PHYSX)
                debris.SetMaxLive(cc.FindCVar("voxel.debris.max") ? cc.FindCVar("voxel.debris.max")->GetInt() : 256);
                debris.SetScale(cc.FindCVar("voxel.debris.scale") ? cc.FindCVar("voxel.debris.scale")->GetFloat() : 1.0f);
                debris.ReservePool(physics);
                // "Broken stays breakable": if a debris chunk is the nearest thing
                // the ray hits, carve + re-fracture THAT chunk and skip the world.
                if (debris.TryCarveRay(world, physics, renderer, pos, fwd, maxDist, radius, worldHitDist)) {
                    o.Print("voxel.break: carved a debris chunk");
                    return;
                }
#endif
                if (!worldHit) {
                    o.Print("voxel.break: nothing solid in range");
                    return;
                }
#if defined(VOX_HAVE_PHYSX)
                // Capture the REAL voxels about to be removed as falling chunks
                // BEFORE carving (so world.GetVoxel still returns the solids).
                // Each chunk keeps its true shape + original colors and tumbles;
                // the run loop re-stamps it at the body's full transform.
                debris.SpawnFromCarve(world, physics, renderer, hx, hy, hz, radius,
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
                // Milestone B1: opt-in structural settle right after the carve.
                // Default OFF (voxel.auto_settle=0) so carve behavior is
                // unchanged; ON makes the now-unsupported region detach + fall.
                // Don't flood synchronously per carve (the full-grid connectivity
                // pass hitches): just ARM the debounce. The main loop runs ONE
                // settle once carving pauses, coalescing a burst into one flood.
                if (cc.FindCVar("voxel.auto_settle") && cc.FindCVar("voxel.auto_settle")->GetBool()) {
                    g_settlePending = true;
                    g_settleQuietFrames = 0;
                }
#if defined(VOX_HAVE_PHYSX)
                // Milestone C: mark only the regions this carve touched dirty so
                // the async rebuild re-decomposes + re-applies just those (not the
                // whole world). If auto-settle detached anything it may have
                // removed voxels anywhere -> mark ALL regions. Gated OFF by
                // default -> no-op when disabled.
                if (cc.FindCVar("physics.world_collider") && cc.FindCVar("physics.world_collider")->GetBool()) {
                    MarkCarveRegionsDirty(hx, hy, hz, radius);  // settle marks its own detached-island regions
                }
#endif
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

                const int radius = cc.FindCVar("voxel.explode_radius")
                                       ? cc.FindCVar("voxel.explode_radius")->GetInt() : 8;
                int hx = 0, hy = 0, hz = 0;
                const float maxDist = static_cast<float>(vox::voxel::kWorldDim) * 2.0f;
                const bool worldHit = world.RaycastSolid(pos, fwd, maxDist, hx, hy, hz);
                float worldHitDist = 1e30f;
                if (worldHit) {
                    const float wdx = (hx + 0.5f) - pos[0], wdy = (hy + 0.5f) - pos[1], wdz = (hz + 0.5f) - pos[2];
                    worldHitDist = std::sqrt(wdx * wdx + wdy * wdy + wdz * wdz);
                }
#if defined(VOX_HAVE_PHYSX)
                const float force = cc.FindCVar("voxel.explode_force")
                                        ? cc.FindCVar("voxel.explode_force")->GetFloat() : 12.0f;
                debris.SetMaxLive(cc.FindCVar("voxel.debris.max") ? cc.FindCVar("voxel.debris.max")->GetInt() : 256);
                debris.SetScale(cc.FindCVar("voxel.debris.scale") ? cc.FindCVar("voxel.debris.scale")->GetFloat() : 1.0f);
                debris.ReservePool(physics);
                // "Broken stays breakable": a nearer debris chunk takes the blast
                // (carve + re-fracture it) instead of the world.
                if (debris.TryCarveRay(world, physics, renderer, pos, fwd, maxDist, radius, worldHitDist)) {
                    o.Print("voxel.explode: shattered a debris chunk");
                    return;
                }
#endif
                if (!worldHit) {
                    o.Print("voxel.explode: nothing solid in range");
                    return;
                }
#if defined(VOX_HAVE_PHYSX)
                // Capture the REAL voxels about to be removed as a RADIAL debris
                // burst BEFORE carving (so world.GetVoxel still sees the solids):
                // each chunk keeps its true shape + original colors and tumbles
                // outward from the crater, magnitude scaled by voxel.explode_force.
                debris.SpawnFromCarve(world, physics, renderer, hx, hy, hz, radius, /*force=*/force, /*radial=*/true);
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
                // Milestone B1: opt-in structural settle right after the blast
                // (default OFF). ON => any region the crater left unsupported
                // detaches + falls instead of floating over the new void.
                // Debounced like voxel.break: arm the settle, the main loop runs
                // ONE flood once carving pauses (no per-blast full-grid hitch).
                if (cc.FindCVar("voxel.auto_settle") && cc.FindCVar("voxel.auto_settle")->GetBool()) {
                    g_settlePending = true;
                    g_settleQuietFrames = 0;
                }
#if defined(VOX_HAVE_PHYSX)
                // Milestone C: mark only the regions the crater touched dirty so
                // the async rebuild re-decomposes + re-applies just those. If
                // auto-settle detached anything (can remove voxels anywhere) mark
                // ALL regions. Gated OFF by default -> no-op when disabled.
                if (cc.FindCVar("physics.world_collider") && cc.FindCVar("physics.world_collider")->GetBool()) {
                    MarkCarveRegionsDirty(hx, hy, hz, radius);  // settle marks its own detached-island regions
                }
#endif
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
    float simTime = 0.0f;  // accumulated wall-clock seconds for time-based animation (e.g. the A2 test cube's spin)
    while (g_running.load()) {
        if (hasWindow) {
            window.PollEvents();
            if (window.ShouldClose()) break;
        }
        keybindings.CheckHotReload();
        console.Drain();
        if (server.UnbindRequested() && server.IsRunning()) server.Stop();
        // Debounced + ASYNC dirty-REGION world-collider rebuild: only the regions a
        // carve/settle touched are re-decomposed (worker) + re-applied (this main thread),
        // never the whole world. Poll a finished worker first (apply each region's boxes via
        // PhysX here), then -- once carving has paused a few frames AND no worker is in
        // flight -- snapshot the dirty regions and kick a new worker. Coalescing: only one
        // batch is in flight; regions re-dirtied meanwhile are picked up by the next batch.
        PollColliderJob(physics);
        // Milestone B1: debounced structural settle. A carve with voxel.auto_settle
        // ON arms g_settlePending; run ONE full-grid flood here once carving has
        // paused a few frames (coalescing a burst into a single settle). Runs BEFORE
        // the collider launch below so any islands it detaches dirty their regions in
        // time for this frame's rebuild snapshot.
        // Fire the debounced settle once carving has paused AND the collider/box
        // layer has drained (ColliderLayerIdle) -- so box-graph settle reads a
        // CURRENT box layer, never one mid-rebuild. (Idle is trivially true when
        // physics.world_collider is off -> the SettleWorld fallback runs as before.)
        if (g_settlePending && ++g_settleQuietFrames > 10 && ColliderLayerIdle()) {
#if defined(VOX_HAVE_PHYSX)
            RunStructuralSettle(Console::Get(), world, renderer, physics, debris);
#else
            RunStructuralSettle(Console::Get(), world, renderer);
#endif
            g_settlePending = false;
            g_settleQuietFrames = 0;
        }
        if (AnyRegionDirty() && !g_colliderJobInFlight && ++g_colliderQuietFrames > 10) {
            LaunchColliderJob(world);   // snapshots + clears the dirty regions it batches
            g_colliderQuietFrames = 0;
        }

        auto now = clk::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        simTime += dt;

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
            fp.test_object = console.FindCVar("renderer.test_object")->GetBool() ? 1 : 0;  // A2 rotating test cube (default OFF)
            fp.time_sec = simTime;  // wall-clock seconds -> time-based animation (A2 cube spin; was never wired -> stuck at 0)
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
#if defined(VOX_HAVE_PHYSX)
    JoinColliderJob();   // wait out any in-flight async collider worker before tearing down
#endif
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
