# ADR-002: DirectX 12 over Vulkan

- **Status:** Accepted
- **Date:** 2026-05-20

## Context

Voxhammer is Windows-only and NVIDIA-first. Raw API performance on Windows in
2026 is roughly equivalent between DX12 and Vulkan — implementation-dependent,
±5% per workload. The choice is therefore about **ecosystem**, not raw speed.

## Decision

Use **DirectX 12** (Agility SDK, DXR 1.1 inline ray queries, DirectStorage 1.2
+ GDeflate, SM 6.6 baseline / 6.8 where the 5090 benefits). Reasons:

- **PIX** is meaningfully better than RenderDoc for GPU performance profiling.
- **DirectStorage 1.2 + GDeflate** GPU decompression is more polished than the
  Vulkan equivalents.
- The **Agility SDK** lets us ship feature levels independent of the Windows
  version.
- NVIDIA's **DX12 path-tracing SDK and samples** are first-class references.

No Vulkan backend. No software / non-RT fallback (an RT-capable GPU is required).

## Consequences

- Single, focused rendering backend — less abstraction, more tuning headroom.
- Tied to Windows. If Linux / Steam Deck ever became a goal, this decision
  would flip; it isn't, so it doesn't.
