# Voxhammer — agent & contributor conventions

Voxhammer is a **standalone Windows-only DX12 voxel-destruction engine** plus a
no-narrative sandbox. It is a sibling to `demont-engine` and deliberately shares
that engine's **control-plane vocabulary** (cvars, commands, network console,
embedded web UI) so the two feel co-authored.

## Current status — M0, pass 1: skeleton + stubs

The repository tree, build system (CMake + vcpkg manifest), per-subsystem static
libraries, the cvar/command registry spine, ADRs, and CI are in place. Most
subsystems are **stubs with TODOs**. Heavy paths are gated OFF behind CMake
options + vcpkg manifest features so the core tree configures and links fast.

See `docs/` and the `Voxhammer Build Prompt` for the full roadmap (M0–M8).

## Conventions

- **Language:** C++20. DOD throughout. No exceptions in hot paths.
- **Namespace:** `vox::` top-level; `vox::console` mirrors demont's `pt::console`
  API 1:1 (CVar / Command / Console / ConsoleServer, ports 27960/27961, identical
  JSON protocol) so the full control-plane port stays a near-copy.
- **File headers:** every source/CMake file starts with
  `// SPDX-License-Identifier: MIT` + `// Copyright (c) 2026 Rajesh D'Monte`.
- **One subsystem per static library** (`vox_<name>`). `engine/CMakeLists.txt`
  aggregates them into `vox_engine`, which `game/voxhammer.exe` links.
- **clang-format** enforced (LLVM base, 4-space, 120 col).
- **No in-game console** — ever. The web console + keybindings are the action
  surface. See `docs/adr/adr-003-no-ingame-console.md`.
- **DX12 only.** No Vulkan backend, no software fallback. RT-capable GPU required.
  See `docs/adr/adr-002-dx12-over-vulkan.md`.

## Building (Windows)

The toolchain (CMake/Ninja/MSVC/clang-cl) lives inside Visual Studio and is only
on PATH after `vcvars64.bat`. vcpkg must be bootstrapped and `VCPKG_ROOT` set.

```bat
scripts\bootstrap_win.bat        :: vcvars + ensure VCPKG_ROOT + configure + build
:: or manually, inside a VS dev prompt with VCPKG_ROOT set:
cmake --preset windows-debug
cmake --build build/windows-debug --parallel
ctest --test-dir build/windows-debug --output-on-failure
```

The first configure builds the core vcpkg ports (fmt, tomlplusplus, glm,
doctest) from source — a few minutes once. Subsystem deps activate via vcpkg
manifest features, e.g.
`-DVCPKG_MANIFEST_FEATURES=console;tls -DVOX_ENABLE_CONSOLE_SERVER=ON`.

## Where things live

| Path | Role |
|---|---|
| `engine/cvar` | CVar/Command registry, `cvars.toml` persistence + schema migration |
| `engine/console` | network ConsoleServer (WS/TCP/HTTPS, argon2id auth) — *stub* |
| `engine/web` | embedded vanilla-JS console frontend |
| `engine/{jobs,ecs,ispc,platform,voxel,physics,render,audio,script}` | *stubs* |
| `game` | `voxhammer.exe` sandbox entry point |
| `tools/{voximport,assetcook,profview}` | dev CLIs — *stubs* |
| `thirdparty` | vendor SDK submodules / pinned binaries (Agility, PhysX, DLSS, …) |
| `docs/adr` | architecture decision records |
