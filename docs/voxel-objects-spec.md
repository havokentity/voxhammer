<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Rajesh D'Monte -->
# Voxel Objects & Teardown-style Destruction — Design Spec

**Branch:** `feat/voxel-objects` (kept off `main`; `main` stays the stable engine).
**Status:** Milestone A (render path) = the immediate build. This **supersedes the
placeholder destruction #4** (which captured carved voxels, made PhysX bodies, but
*re-stamped* the chunk voxels back into the static GPU grid every frame via
`EditVoxels` — a hack that aliases and is not how Teardown works).

---
## 1. Vision (user-specced, grounded in Teardown's actual tech)

The world is **not** one global voxel grid — it is a **scene of independent VOXEL
OBJECTS**, each with its own local voxel grid (own resolution + orientation +
palette), transform, rigid body, material, and connectivity graph. Buildings /
terrain = big *anchored* objects; props / vehicles / inclined ladders = smaller
*free* objects, broken apart independently.

- **Destruction** = per-object connectivity + **integer stability** (cheap BFS from
  anchors, *not* a force/weight solve) → break at computed **fracture points** →
  detached regions become **new rigid-body objects** (themselves re-breakable), with
  optional **hinge joints** for the "dangle / sag" look.
- **Rendering** = **rasterize each object's oriented bounding box (OBB)** → **ray-march
  its voxels in the fragment shader** → deferred lighting + temporal denoise. (Voxels
  are *ray-marched*, never rasterized — the box is just a cheap proxy for which pixels
  to march + hardware depth compositing.)

This mirrors Teardown (Dennis Gustafsson / Tuxedo Labs): "thousands of smaller volumes
filled with voxels," OBB-raster + fragment-shader voxel DDA, deferred lighting, temporal
denoise. The destruction internals are *not* publicly documented, so Milestone B uses the
user's integer-stability design + the established flood-fill-from-anchors pattern.

---
## 2. The unifying primitive: `VoxelObject`

Everything — the static world, a ladder, a vehicle, a falling chunk — is one type:

```
VoxelObject {
    id
    Texture3D<uint8> grid          // object-local material indices, dims dx*dy*dz; 0 = empty
    mip chain (3 levels)           // max-occupancy downsample -> dense octree for empty-skip
    palette (color + material)
    transform (model, invModel)    // arbitrary rotation/position -> inclined objects free
    AABB / OBB
    flags (anchored/static | dynamic)
    physicsBodyId                  // PhysX rigid body (static for anchored, dynamic for free)
    connectivity metadata          // for the stability solver (Milestone B)
}
```

The renderer draws **N** of them; destruction **splits one into many**.

---
## 3. Milestone A — Deferred voxel-object renderer (THE BUILD)

**Goal:** rework the renderer from "forward raymarch of one global structured-buffer
grid" into Teardown's **deferred, per-object** pipeline. Milestone A delivers the
**render path only** — no dynamics/destruction logic yet. The current 128³ world becomes
`VoxelObject #0`; prove it renders pixel-identical, then add a second dynamic object, then
route the existing fracture chunks through it (finally drawing them as true rigid bodies).

### 3.1 Pipeline (per frame)
1. **G-buffer pass — OBB raster + voxel DDA**
   - Per object, draw its OBB (unit cube scaled to `dims`, by `model`), **backfaces only**
     (front-cull, so the camera being inside an object never clips), depth test + write.
     Instanced: 1 instance / object; instance data = `invModel`, `dims`, grid-SRV index,
     palette index.
   - **Fragment shader:** reconstruct the world ray → transform into **object-local** space
     (`invModel`) → clip to the local AABB `[0,dims]` → **voxel-DDA the local grid** with
     mip-octree empty-skip → on solid hit: world pos + face normal (rotated by the object)
     + material → write **G-buffer** (albedo, normal + linear depth, material
     [reflect/rough/metal/emit], motion vector); on miss: `discard` (depth buffer then
     reveals the next object or the sky).
   - The **hardware depth buffer composites** all objects (nearest voxel hit wins) +
     occlusion — this is the "hybrid."
2. **Deferred lighting — full-screen, reads the G-buffer.** Direct sun + soft shadow, GI
   (our path-traced indirect), reflections (later). **This is exactly where the GI +
   à-trous/SVGF denoiser + temporal reproject we already built slot in** — they already
   operate demodulated on a G-buffer-style split.
3. **Composite / tonemap / dither → backbuffer** (reuse existing).

### 3.2 Acceleration
- Per-object **mip chain** (3 levels, max-occupancy) for empty-space skipping.
- *Later:* a **coarse global grid** (à la Teardown's ~2504×256×2504, 2×2×2 packed) used
  only for GI/reflection rays, so secondary rays trace one cheap volume instead of every
  object.

### 3.3 DX12
- New PSO: OBB-raster VS (instanced unit cube) + voxel-DDA PS → **MRT G-buffer** + DSV.
- Per-object grids: `Texture3D<uint>`; SRVs via a descriptor table → bindless as the count
  grows. Instance data in a structured buffer.
- **Reuse the G-buffer render targets already built for the SVGF denoiser.**

### 3.4 Sub-steps (each independently verifiable, windowed)
- **A0** — Split the *current* single PS pass into **G-buffer + deferred-lighting**, still
  raymarching the one global grid full-screen. **Verify pixel-identical.** (De-risks the
  deferred split using the denoiser G-buffer we already have.)
- **A1** — Replace the full-screen raymarch with **OBB-raster of the single world object**
  + fragment DDA. **Verify identical.** (Proves OBB-raster + local-space DDA + depth.)
- **A2** — Add a **second** `VoxelObject` (a test cube grid) at a **rotating** transform,
  depth-composited with the world. (Proves multi-object + arbitrary transforms.)
- **A3** — Route the existing **fracture chunks** through `VoxelObject` (each chunk = a
  dynamic object at its PhysX transform). Chunks render as **true rigid bodies**; the
  `EditVoxels` stamp is removed. **Fixes the user's complaint.**

---
## 4. Roadmap after A

- **B — Connectivity + integer stability.** Per-object voxel-neighbour graph; **BFS from
  ANCHORS** (ground/base contact); an **integer stability** value propagates from anchors
  (cheap, capped — *not* a force solve). On carve, **re-flood the dirty region, amortized
  across frames**; regions that lose their anchor path (or drop below the stability
  threshold for their span) **detach → become new `VoxelObject`s + PhysX bodies**;
  mass/inertia come basically free from the voxel count. *Nothing floats.*
- **C — Material strength + fracture-point selection + re-fracture.** Per-material
  thresholds; the stability/stress picks **where** to break (an overloaded unsupported
  span resolves to its weakest point); detached objects are themselves re-breakable.
- **D — Hinge breaks (the "dangle / sag").** A break may leave a **PhysX hinge/revolute
  joint** at the fracture point so the segment **swings down inclined** instead of cleanly
  detaching — looks like bending, is actually break + rigid pivot. **No deformation / FEM /
  soft bodies** — all PhysX rigid bodies + joints. (Mostly folds into C.)
- **E — Independent objects.** Author/load vehicles, props, inclined ladders as their own
  `VoxelObject`s (own resolution + orientation), broken apart independently.

---
## 5. Reuse from the current engine (not wasted)
- **GI path-trace + à-trous/SVGF + temporal reproject + tonemap/dither** → the deferred
  lighting + denoise stage (Teardown's pipeline shape).
- **Voxel DDA (`traceVoxel`)** → adapted to march a *local* grid in object space.
- **PhysX bodies + chunk capture + actor pooling** (from #4) → feed dynamic `VoxelObject`
  transforms.
- **`VoxelWorld`** → becomes `VoxelObject #0` (the anchored world).
- **PhysX-ON build** (`windows-release-physx` / `windows-debug-physx`).

---
## 6. Risks & mitigations
- **Big renderer rework** (forward→deferred, single-grid→per-object) → the A0–A3 ladder
  verifies pixel-identical at each step before adding capability.
- **GI/denoiser re-anchoring** to the new G-buffer → should fit (built demodulated).
- **Per-object 3D-texture count / memory / binding** → start with a small fixed set, move
  to bindless as object counts grow.
- **Perf with many objects** (OBB overdraw + per-pixel DDA) → mip empty-skip + the depth
  buffer + the coarse global GI grid (later).
- **Destruction noise during motion** → user-deferred (the denoiser handles still frames).

---
## 7. References
- Juan Diego Montoya, *"Teardown Teardown"* — rendering breakdown
  (https://juandiegomontoya.github.io/teardown_breakdown.html).
- Dennis Gustafsson talks / Voxagon blog — per-object voxel volumes, custom voxel engine.
- IOLITE / Procedural-World — voxel connectivity + flood-fill destruction patterns
  (basis for Milestone B, since Teardown's exact stability algorithm is unpublished).
