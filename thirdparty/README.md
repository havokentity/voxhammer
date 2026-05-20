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
| NVIDIA PhysX 5 | `vox_physics` | shallow clone `NVIDIA-Omniverse/PhysX` (pinned, see below) | BSD-3 |
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

## NVIDIA PhysX 5 (PASS 1 — CPU rigid bodies, gated OFF by default)

PhysX is the open-source NVIDIA PhysX 5 SDK (BSD-3-Clause). No account, login, or
EULA — it's a plain public `git clone`. It is **not** committed to this repo: the
multi-GB source tree and its build outputs are git-ignored (see `.gitignore`).
The reproducible "source of truth" is this pin + the build steps + the one small
build patch tracked at `thirdparty/physx-voxhammer.patch`.

The whole integration is behind the `VOX_ENABLE_PHYSX` CMake option (**default
OFF**). With it OFF, `vox_physics` is a logging stub and the engine links with no
PhysX dependency. With it ON, `engine/physics/CMakeLists.txt` links the static
PhysX libs and defines `VOX_HAVE_PHYSX`, so `PhysicsWorld.cpp` compiles its real
CPU rigid-body path.

### Pin

| | |
|---|---|
| Repo | `https://github.com/NVIDIA-Omniverse/PhysX.git` (`NVIDIA-GameWorks/PhysX` redirects here) |
| Tag | `105.1-physx-5.3.1` |
| Commit | `85befb639eb736bc8ec7d4a2e9726ec2ab1d0434` |
| Version | PhysX `5.3.1.e9cf8e18b` |
| License | BSD-3-Clause |

### 1. Acquire (shallow clone into `thirdparty/physx`)

```bat
cd thirdparty
git clone --depth 1 --branch 105.1-physx-5.3.1 https://github.com/NVIDIA-Omniverse/PhysX.git physx
```

The SDK lives at `thirdparty/physx/physx/` (note the doubled `physx/` — the repo
root holds `physx/`, `blast/`, `flow/`; we only build `physx/`).

### 2. Patch the bundled build scripts (one small, documented change)

PhysX's `compiler/public` build sets `PUBLIC_RELEASE=1`, which forces a CMake
`FILE(COPY ...)` of prebuilt `PhysXDevice` / `freeglut` / `PhysXGpu` DLLs that
only exist when you use the bundled `generate_projects` + *packman* download
flow. A CPU-only source build (what we do) has none of them, so configure dies
with `FILE COPY cannot find .../PhysXGpu_64.dll`. The patch disables only that
DLL-copy block (it does not change any compile defines or simulation behaviour):

```bat
cd thirdparty\physx
git apply ..\physx-voxhammer.patch
```

### 3. Build (static libs, dynamic debug CRT, MSVC, x64)

Drive PhysX's native CMake directly (bypassing `generate_projects.bat` /
*packman*, which only knows VS2017/2019/2022 presets — not the VS "18" /
MSVC 19.50 toolchain here). Run from a VS x64 dev shell with CMake + Ninja on
`PATH`. `NV_USE_STATIC_WINCRT=OFF` + `NV_USE_DEBUG_WINCRT=ON` ⇒ `/MDd`, matching
the engine's `windows-debug` preset (CRT mismatch would break linking).

```bat
cd thirdparty
set PHYSX_ROOT=%CD%\physx\physx
cmake -S "%PHYSX_ROOT%\compiler\public" -B physx_build -G "Ninja Multi-Config" ^
  -DTARGET_BUILD_PLATFORM=windows -DPX_OUTPUT_ARCH=x86 ^
  -DPHYSX_ROOT_DIR="%PHYSX_ROOT%" ^
  -DPX_OUTPUT_LIB_DIR="%PHYSX_ROOT%" -DPX_OUTPUT_BIN_DIR="%PHYSX_ROOT%" ^
  -DNV_USE_STATIC_WINCRT=OFF -DNV_USE_DEBUG_WINCRT=ON ^
  -DPX_GENERATE_STATIC_LIBRARIES=TRUE ^
  -DPX_BUILDSNIPPETS=FALSE -DPX_BUILDPVDRUNTIME=FALSE ^
  -DPX_CMAKE_SUPPRESS_REGENERATION=ON --no-warn-unused-cli
cmake --build physx_build --config debug
```

(`debug` is the PhysX config name; PhysX configs are `debug | checked | profile |
release`. For a Release engine build, build `checked` or `release` here and point
`VOX_PHYSX_CONFIG` at it — see below.)

### 4. Outputs

Static libs land in (config `debug`, dynamic CRT = `md`, MSVC 19.x ⇒ `vc143`):

```
thirdparty/physx/physx/bin/win.x86_64.vc143.md/debug/
    PhysX_static_64.lib            PhysXCommon_static_64.lib
    PhysXFoundation_static_64.lib  PhysXExtensions_static_64.lib
    PhysXCooking_static_64.lib     PhysXPvdSDK_static_64.lib
    PhysXCharacterKinematic_static_64.lib
    PhysXVehicle_static_64.lib     PhysXVehicle2_static_64.lib
```

The generated `thirdparty/physx/physx/include/PxConfig.h` carries
`#define PX_PHYSX_STATIC_LIB`. CPU-only ⇒ **no DLLs** to copy next to the exe.

### 5. Build the engine against it

```bat
:: from a VS dev shell at the repo root, with VCPKG_ROOT set
cmake --preset windows-debug -B build/windows-debug-physx -DVOX_ENABLE_PHYSX=ON
cmake --build build/windows-debug-physx --parallel
ctest --test-dir build/windows-debug-physx -R physics --output-on-failure
```

`engine/physics/CMakeLists.txt` exposes cache vars to retarget the prebuilt libs
without editing CMake:

| Cache var | Default | Meaning |
|---|---|---|
| `VOX_PHYSX_ROOT` | `thirdparty/physx/physx` | SDK root (inner `physx/` dir) |
| `VOX_PHYSX_PLATFORM_BIN` | `win.x86_64.vc143.md` | `bin/<platform>` subdir (toolset + CRT) |
| `VOX_PHYSX_CONFIG` | `debug` | `debug \| checked \| profile \| release` |

Smoke test (`tests/physics_test.cpp`, only built when `VOX_ENABLE_PHYSX=ON`):
a 1 m dynamic box dropped from y=20 over a ground plane falls and settles at
~0.51 (≈ the 0.5 half-extent) and goes to sleep — confirming PhysX is really
simulating. Verified before/after Y: **20.0 → 0.5127**.

### Toolchain note (this machine)

The from-source build compiled cleanly with the very new MSVC **19.50** (VS "18")
+ CMake **4.2** + Ninja **1.12** — PhysX 5.3.1's `/WX` (warnings-as-errors) did
**not** trip on this newer compiler, so no warning-suppression patch was needed
(only the DLL-copy patch in step 2). If a future toolchain *does* hit new `/WX`
warnings in PhysX sources, the fix is to strip `/WX` from `PHYSX_COMMON_FLAGS` in
`physx/source/compiler/cmake/windows/CMakeLists.txt` (add it to the patch).
