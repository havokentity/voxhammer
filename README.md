# Voxhammer

A standalone **Windows-only, DX12, NVIDIA-first** voxel-destruction game engine and
a no-narrative sandbox that exercises every system — voxel world, full
destructibility, voxel-coupled rigid-body physics, path-traced GI, fluid + fire
simulation, and propagated audio. The technical pillars of Teardown, with no
story, characters, missions, or progression.

> **Status: M0 — pass 1 (skeleton + stubs).** The repo layout, build system,
> per-subsystem static libraries, the cvar/command registry spine, embedded web
> bundle, ADRs, and CI exist and link into a runnable `voxhammer.exe`. The DX12
> renderer, physics, sim, audio, and the hardened network console are stubbed
> and land in later passes. See `docs/` and the build roadmap.

## Target hardware

- **Reference:** RTX 5090 (32 GB), Ryzen 9 9950X3D, 64 GB DDR5, Gen5 NVMe.
- **Floor:** RT-capable GPU (RTX 2060 / RX 6600 XT / Arc A580+), 8C AVX2 CPU, NVMe.

NVIDIA is the primary optimization target; AMD/Intel paths work but aren't the perf bar.

## Tech at a glance

| Layer | Tech |
|---|---|
| Graphics | DirectX 12 (Agility SDK), DXR 1.1 inline, DirectStorage 1.2 + GDeflate, SM 6.6+ |
| Renderer | Pure voxel path tracer, ReSTIR DI+GI, DLSS 4 (SR+RR+MFG) / FSR 3 + NRD |
| Physics | NVIDIA PhysX 5 (GPU rigid bodies on NVIDIA) |
| ECS / Jobs / SIMD | EnTT · enkiTS · ISPC |
| Audio | FMOD Studio + Steam Audio |
| Scripting | LuaJIT + FFI |
| Control plane | cvars + commands + **web-only** network console (no in-game console) |

## Control plane

The only control surface is a **web console** at `https://localhost:27960/`
(WebSocket `wss://localhost:27960`, scripted TCP on `127.0.0.1:27961`), ported
from `demont-engine`. Security is mandatory: self-signed TLS, argon2id password,
`HttpOnly`/`Secure` session cookies, Origin enforcement, and rate-limited auth.
The server **refuses to bind without a password** — set one with
`--remote-password=<pw>`. There is intentionally **no in-game console**
(`docs/adr/adr-003`). In-engine actions go through the keybindings system.

## Building

Requires Windows 10 22H2+/11 x64, Visual Studio (C++ workload), and a
bootstrapped [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```bat
scripts\bootstrap_win.bat
```

or, inside a VS dev prompt:

```bat
cmake --preset windows-debug
cmake --build build/windows-debug --parallel
ctest --test-dir build/windows-debug --output-on-failure
build\windows-debug\game\voxhammer.exe --version
```

Presets: `windows-debug` (MSVC), `windows-relwithdebinfo` (clang-cl),
`windows-release` (clang-cl).

## License

MIT © 2026 Rajesh D'Monte. See `LICENSE`. Vendor SDKs under `thirdparty/` carry
their own licenses (PhysX BSD-3, DLSS/Reflex/FMOD proprietary, etc.).
