# thirdparty/

Vendor SDKs that are **not** available (or not suitable) via vcpkg live here as
git submodules or pinned binaries. The vcpkg-managed open-source deps (fmt,
tomlplusplus, glm, doctest, civetweb, libsodium, openssl, enkits, entt,
mimalloc, imgui, glfw3, luajit, tracy, meshoptimizer, nlohmann-json) are
declared in the root `vcpkg.json` and are **not** vendored here.

> Skeleton pass: no submodules are added yet. Each subsystem links its SDK in
> the milestone that needs it (see the build roadmap). The table below is the
> intended pinning plan.

| SDK | Used by | Acquisition | License |
|---|---|---|---|
| DirectX 12 Agility SDK | `vox_render` | NuGet redist → `thirdparty/agility/` | redist |
| DirectStorage 1.2 (+GDeflate) | `vox_render`, `vox_voxel` | NuGet redist | redist |
| NVIDIA PhysX 5 | `vox_physics` | submodule `NVIDIA-Omniverse/PhysX` | BSD-3 |
| NVIDIA DLSS 4 SDK | `vox_render` | NVIDIA developer download | NVIDIA SDK |
| NVIDIA Reflex SDK | `vox_render` | NVIDIA developer download | NVIDIA SDK |
| NVIDIA NRD | `vox_render` (AMD/Intel path) | submodule `NVIDIAGameWorks/RayTracingDenoiser` | MIT |
| AMD FSR 3.1 | `vox_render` (AMD/Intel path) | submodule `GPUOpen-Effects/FidelityFX-SDK` | MIT |
| AMD Anti-Lag 2 SDK | `vox_render` (AMD) | AMD developer download | AMD SDK |
| FMOD Studio API | `vox_audio` | FMOD download (indie license) | proprietary |
| Steam Audio | `vox_audio` | submodule `ValveSoftware/steam-audio` | Apache 2.0 |
| ISPC | `vox_ispc` (build-time) | pinned binary → `thirdparty/ispc/bin/` | BSD-3 |

## Adding a submodule

```bat
git submodule add <url> thirdparty/<name>
```

Pinned binaries (ISPC, Agility/DirectStorage redists) are committed or fetched
by a `scripts/` helper, not added as submodules.
