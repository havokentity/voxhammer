// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#if VOX_ENABLE_JOBS
// Forward-declare to avoid pulling all of enkiTS into every TU.
namespace enki { class TaskScheduler; }
#endif

namespace vox::jobs {

// CCD-aware routing hints for the 9950X3D (two-CCD topology):
//   CacheHot  -> prefer workers pinned to the 3D V-Cache CCD (128 MB L3)
//   IOBound   -> prefer the frequency-optimised CCD
//   AudioRT   -> prefer the frequency-optimised CCD (low-latency path)
// Actual CPU pinning is best-effort and guarded by a TODO below.
enum class TaskTag { Default, CacheHot, IOBound, AudioRT };

class JobScheduler {
public:
    JobScheduler();
    ~JobScheduler();  // defined in .cpp so unique_ptr destructor sees full type

    // worker_threads == 0 => auto-size from std::thread::hardware_concurrency().
    void Init(unsigned worker_threads = 0);
    void Shutdown();
    bool Running() const { return running_; }

    // Split [0, count) across worker threads in balanced ranges and call body
    // for each [begin, end) slice.  Blocks until all slices complete.
    // Falls back to a single inline call when VOX_ENABLE_JOBS is off.
    void ParallelFor(uint32_t count,
                     const std::function<void(uint32_t begin, uint32_t end)>& body);

    // Enqueue a fire-and-forget task.  Returns immediately; task executes
    // asynchronously on a worker.  The tag is advisory only (see TODO below).
    // Falls back to a synchronous inline call when VOX_ENABLE_JOBS is off.
    void Run(TaskTag tag, std::function<void()> fn);

private:
#if VOX_ENABLE_JOBS
    std::unique_ptr<enki::TaskScheduler> scheduler_;
#endif
    bool running_ = false;
};

}  // namespace vox::jobs
