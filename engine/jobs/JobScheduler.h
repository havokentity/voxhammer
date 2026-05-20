// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

namespace vox::jobs {

// enkiTS-backed task scheduler. CCD-aware tags steer work on the 9950X3D:
// CacheHot -> the 3D V-Cache CCD (128 MB L3), IOBound/AudioRT -> the frequency
// CCD. enkiTS wiring + affinity activate with VOX_ENABLE_JOBS (vcpkg feature
// 'jobs'); this pass is a stub.
enum class TaskTag { Default, CacheHot, IOBound, AudioRT };

class JobScheduler {
public:
    void Init(unsigned worker_threads = 0);  // 0 => auto from CPU topology
    void Shutdown();
    bool Running() const { return running_; }

private:
    bool running_ = false;
};

}  // namespace vox::jobs
