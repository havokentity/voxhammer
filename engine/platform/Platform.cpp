// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "platform/Platform.h"

#include "platform/Log.h"

#include <cstdlib>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#include <intrin.h>
#endif

namespace vox::platform {

std::string UserDataDir() {
    namespace fs = std::filesystem;
    fs::path base;
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA")) {
        base = fs::path(appdata) / "Voxhammer";
    } else {
        base = fs::temp_directory_path() / "Voxhammer";
    }
#else
    if (const char* home = std::getenv("HOME")) {
        base = fs::path(home) / ".local/share/Voxhammer";
    } else {
        base = fs::temp_directory_path() / "Voxhammer";
    }
#endif
    std::error_code ec;
    fs::create_directories(base, ec);
    return base.string();
}

const char* ToString(GpuVendor v) {
    switch (v) {
    case GpuVendor::Nvidia: return "NVIDIA";
    case GpuVendor::Amd:    return "AMD";
    case GpuVendor::Intel:  return "Intel";
    case GpuVendor::Unknown: break;
    }
    return "Unknown";
}

CpuInfo DetectCpu() {
    CpuInfo info;
    info.logical_cores = std::thread::hardware_concurrency();
    // Rough physical-core estimate until topology enumeration lands.
    info.physical_cores = info.logical_cores ? info.logical_cores / 2 : 0;

#if defined(_WIN32)
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 0);
    const int max_leaf = regs[0];
    if (max_leaf >= 7) {
        __cpuidex(regs, 7, 0);
        info.has_avx2   = (regs[1] & (1 << 5)) != 0;   // EBX bit 5  = AVX2
        info.has_avx512 = (regs[1] & (1 << 16)) != 0;  // EBX bit 16 = AVX-512F
    }
    // TODO(platform): parse brand string + cache topology to flag 3D V-Cache.
#endif
    return info;
}

GpuInfo DetectGpu() {
    // TODO(platform): enumerate adapters via DXGI, read vendor id (0x10DE NVIDIA,
    // 0x1002 AMD, 0x8086 Intel), VRAM, and DXR support tier. Picks DLSS vs
    // FSR/NRD path.
    return GpuInfo{};
}

DisplayInfo DetectDisplay() {
    // TODO(platform): query the output's HDR metadata via DXGI.
    return DisplayInfo{};
}

void LogSummary() {
    const CpuInfo cpu = DetectCpu();
    const GpuInfo gpu = DetectGpu();
    const DisplayInfo disp = DetectDisplay();

    vox::log::Info("cpu: {} logical cores, AVX2={}, AVX-512={}, X3D-VCache={}", cpu.logical_cores,
                   cpu.has_avx2, cpu.has_avx512, cpu.has_x3d_vcache);
    vox::log::Info("gpu: {} (RT={}, vram={} MB) [detection stubbed]", ToString(gpu.vendor),
                   gpu.ray_tracing, gpu.vram_bytes / (1024 * 1024));
    vox::log::Info("display: HDR={} ({} nits) [detection stubbed]", disp.hdr_capable, disp.peak_nits);
}

}  // namespace vox::platform
