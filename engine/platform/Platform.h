// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

// Host capability detection. The renderer picks its vendor path (DLSS vs
// FSR/NRD) and the job system pins CCD-affinity from this. SKELETON: CPU core
// count + AVX2/AVX-512 are real (cpuid); GPU/HDR/X3D-CCD are stubs pending
// DXGI enumeration + CPUID model parsing in a later pass.

#include <cstdint>
#include <string>

namespace vox::platform {

enum class GpuVendor { Unknown, Nvidia, Amd, Intel };
const char* ToString(GpuVendor v);

struct GpuInfo {
    GpuVendor     vendor      = GpuVendor::Unknown;
    std::string   name;
    bool          ray_tracing = false;  // RT-capable (required by the engine)
    std::uint64_t vram_bytes  = 0;
};

struct CpuInfo {
    unsigned logical_cores  = 0;
    unsigned physical_cores = 0;
    bool     has_avx2       = false;
    bool     has_avx512     = false;
    bool     has_x3d_vcache = false;  // 9950X3D-style 3D V-Cache CCD present
};

struct DisplayInfo {
    bool hdr_capable = false;
    int  peak_nits   = 0;
};

GpuInfo     DetectGpu();
CpuInfo     DetectCpu();
DisplayInfo DetectDisplay();

// Logs all three (called once at boot).
void LogSummary();

// %APPDATA%/Voxhammer (created if missing). Home of cvars are kept in CWD, but
// cert/, keybindings.toml, and other per-user state live here.
std::string UserDataDir();

}  // namespace vox::platform
