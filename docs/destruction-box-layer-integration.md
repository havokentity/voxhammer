<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 Rajesh D'Monte
-->
# Destruction box-layer integration plan

How the three CPU-only destruction modules (`box_graph`, `slice_fracture`,
`stress` — built standalone with doctest tests in `engine/voxel/destruction/`)
get wired into the live engine. Realises the **two-tier hybrid** decision:

> The dense grid (`VoxelWorld` 128³, `Chunk.mats`) stays **canonical** — it is
> what the DDA raymarcher renders and what a carve mutates. A **persistent box
> decomposition + adjacency graph**, derived from the grid and updated only in
> dirty regions, becomes the **physics/connectivity/stability** representation.
> Connectivity, fracture, and stress are decided on the *boxes* (hundreds–
> thousands), never by per-voxel BFS over 2M cells.

## The grid ↔ box contract (the one rule everything obeys)
- **Grid = truth for voxels + rendering.** A carve clears cells in the grid and
  uploads incrementally (`EditVoxels`); the renderer never sees boxes.
- **Boxes = derived, decision-making layer.** The box set drives *what is
  connected*, *what fails*, *what detaches*. When a decision selects an island /
  failed part, its **voxels are read back from the grid** (the boxes give the
  AABBs) to spawn debris, then those cells are cleared and the touched regions
  re-decomposed.
- **Sync mechanism = the existing dirty-region plumbing.** We already partition
  the world into 64 regions (`REGION=32`, 4³) and re-decompose only the regions a
  carve/settle touched (`MarkCarveRegionsDirty` / `g_colliderRegionDirty` /
  `LaunchColliderJob` → `ColliderDecomposeJob`). The box layer rides the same
  dirty set — no new sync path.

## Phase 0 — Persistent box layer (foundation, no behaviour change)
Today `ColliderDecomposeJob` decomposes each dirty region into boxes for PhysX
and **discards** them. Change: **persist** them.
- Per region, keep `std::vector<Box>` (+ a **material tag per box**) alongside the
  `ColliderBox` list. Add material by decomposing **per material id** (group cells
  by material before `decomposeComponent`) or by sampling the grid at `box.mn`.
- Boxes are region-local-inclusive → store in **world voxel coords** (`box.mn +=
  regionOrigin`) so cross-region adjacency works for connectivity.
- Optional refactor: lift the region/box state out of `game/main.cpp` into
  `engine/voxel/WorldBoxLayer.{h,cpp}`. Pragmatic first step: extend the existing
  main.cpp plumbing; factor out only if it grows.
- **Verify:** debug assert that, per region, the union of persisted boxes ==
  exactly the region's solid cells (no behaviour change yet).

## Phase 1 — Settle via `box_graph` (replace the 2M-voxel BFS) — uses Part A
Replace `SettleWorld`'s dense per-voxel flood with box-level union-find.
- Gather all regions' world-coord boxes into one `vector<Box>` (+ a parallel
  material array, + remember each box's region for dirty-marking).
- `labelBoxComponents(boxes)` → components. Anchor set = indices of boxes with
  `box.mn.y < voxel.anchor_layers`. `anchoredComponents(comps, anchorIdx)`.
- Each **unanchored** component = a falling island. For it: read its voxels from
  the grid (over the component's boxes' AABBs), spawn via `DebrisField::SpawnIsland`
  (existing), clear those cells, `MarkRegionsDirtyForAabb` the touched regions,
  `EditVoxels` them (the incremental-upload path from `be09ee6`).
- Files: `engine/voxel/StructuralSettle.{h,cpp}` (swap the internals), `game/main.cpp`
  `RunStructuralSettle`.
- **Gate/verify:** keep the old BFS behind `voxel.settle_boxgraph=0` as an A/B
  fallback; assert both paths detect the same islands on the existing
  `voxel_destruction_test` fixtures, then default the box path ON.
- **Win:** the debounced settle stops scaling with world volume; it scales with
  box count. This is the remaining-stutter killer for big worlds.

## Phase 2 — Fracture-at-any-point via `slice_fracture` — uses Part B
Sphere carves already split structures (carve → regions re-decompose → Phase-1
connectivity finds what detached). `slice_fracture` adds **precise thin cuts** —
a planar slice that cleaves a structure in two with minimal material loss.
- New `voxel.slice` command (or a carve mode): build a thin **cut AABB** (a 1-N
  voxel-thick slab at the aim point/plane). `applyCut(regionBoxes, cut)` →
  `survivors` + `removed` (+ source indices for material).
- Clear the `removed` cells in the grid (`EditVoxels`), re-decompose touched
  regions, then run **Phase-1** connectivity on the survivors → the two halves
  come apart and fall.
- Files: `game/main.cpp` (new command), the box layer.
- **Gate:** new opt-in command; no change to `voxel.break/explode` defaults.

## Phase 3 — Stress: weak spans collapse — uses Part C
After connectivity (Phase 1), run load-propagation on each **anchored** component
so over-stressed members snap on their own.
- `StressInput`: the component's boxes; `weight` = box volume; `strength` =
  per-material strength (small table, cvar-tunable `voxel.stress.<mat>` / a
  default); `anchorBoxIndices` = ground boxes. → `computeStress` → `failed[]`.
- Failed boxes "snap": remove them (or their up-stream edges) and **re-label**
  (`labelBoxComponents`) → the now-unsupported part becomes a new unanchored
  island → falls via the Phase-1 spawn path. Iterate until stable (cap a few
  passes; cheap on boxes).
- Runs inside the **debounced** settle (off the box graph, so it's near-free).
- Files: a stress pass in `StructuralSettle` / `RunStructuralSettle`; a material-
  strength table.
- **Gate:** `voxel.stress=0` by default until tuned; `voxel.stress.*` knobs.
- **Differentiator:** this is beyond what the BoxCutter reference does (it is
  binary-connectivity only) — loaded pipes snap mid-span, thin supports buckle.

## Phase 4 — (optional, later) chunk-side migration
Migrate `DebrisField` (`TryCarveRay`/`CarveChunk` re-fracture, `BuildLocalBoxes`
colliders) to a **persistent per-chunk box decomposition** + `applyCut` +
`labelBoxComponents` instead of per-voxel BFS on the chunk grid. Low priority —
chunk grids are small, so the per-voxel path is already cheap there.

## Separate track (mine, not dependent on A/B/C): debris lifecycle
Sleep/freeze settled debris, pool + cull, and **re-merge** a settled chunk back
into the world grid (stamp its voxels in → regains full GI, frees its rigid body,
caps live-body count). This is what sustains high debris counts; it rides on the
renderer/PhysX/main-loop and is independent of the box modules.

## Sequencing & safety
1. Phase 0 (persist boxes + material) — invisible, assert-validated.
2. Phase 1 (settle via box_graph) — the perf win; A/B against the old BFS.
3. Phase 3 (stress) — the emergent layer; default OFF until tuned.
4. Phase 2 (slice) — precise cuts; opt-in command.
5. Phase 4 + debris lifecycle — last.

Every phase is gated by a cvar with the proven path as fallback, builds with
`windows-release-physx`, and is verified in a real release run before its gate
defaults ON.
