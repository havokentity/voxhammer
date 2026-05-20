// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "jobs/JobScheduler.h"

#include "platform/Log.h"

#include <algorithm>
#include <thread>

#if VOX_ENABLE_JOBS
#include <TaskScheduler.h>   // enkiTS
#endif

namespace vox::jobs {

// ---------------------------------------------------------------------------
// Constructor / destructor — defined here so unique_ptr<enki::TaskScheduler>
// destructor has the full type visible (avoids "incomplete type" static_assert).
// ---------------------------------------------------------------------------

#if VOX_ENABLE_JOBS
JobScheduler::JobScheduler() = default;
JobScheduler::~JobScheduler() = default;
#else
JobScheduler::JobScheduler() = default;
JobScheduler::~JobScheduler() = default;
#endif

// ---------------------------------------------------------------------------
// enkiTS-backed implementation
// ---------------------------------------------------------------------------

#if VOX_ENABLE_JOBS

// A TaskSet that calls a ranged body lambda.
// enkiTS splits a TaskSet into m_SetSize partitions; the partition is
// [startIndex, endIndex).  We map count -> setSize and forward each slice.
struct RangedTask : public enki::ITaskSet {
    std::function<void(uint32_t, uint32_t)> body;

    explicit RangedTask(uint32_t count,
                        std::function<void(uint32_t, uint32_t)> b)
        : enki::ITaskSet(count)
        , body(std::move(b))
    {}

    void ExecuteRange(enki::TaskSetPartition range, uint32_t /*threadnum*/) override {
        body(static_cast<uint32_t>(range.start),
             static_cast<uint32_t>(range.end));
    }
};

// A PinnableTask that wraps a fire-and-forget void().
// TODO(jobs-ccd): query Windows topology (GetLogicalProcessorInformationEx) to
// identify which processor group / indices belong to the 3D V-Cache CCD on
// the 9950X3D, then use enki::PinnableTask and SetPriority / custom thread
// init to pin CacheHot tasks to that CCD.  For now tag is stored but unused.
struct OneshotTask : public enki::ITaskSet {
    std::function<void()> fn;
    TaskTag tag;  // advisory, see TODO above

    OneshotTask(std::function<void()> f, TaskTag t)
        : enki::ITaskSet(1)
        , fn(std::move(f))
        , tag(t)
    {}

    void ExecuteRange(enki::TaskSetPartition /*range*/, uint32_t /*threadnum*/) override {
        fn();
    }
};

#endif  // VOX_ENABLE_JOBS

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void JobScheduler::Init(unsigned worker_threads) {
#if VOX_ENABLE_JOBS
    scheduler_ = std::make_unique<enki::TaskScheduler>();

    if (worker_threads == 0) {
        // enkiTS default: uses hardware_concurrency automatically when passed 0.
        scheduler_->Initialize();
    } else {
        scheduler_->Initialize(worker_threads);
    }

    vox::log::Info("jobs: enkiTS scheduler started ({} threads)",
                   scheduler_->GetNumTaskThreads());
    // TODO(jobs-ccd): detect 9950X3D CCD topology via
    // GetLogicalProcessorInformationEx(RelationAll) and install a custom
    // enki::TaskScheduler::Config::threadNum / profiling callbacks to pin
    // CacheHot workers to the 3D V-Cache CCD (first CCD, lower NUMA node).
#else
    (void)worker_threads;
    vox::log::Info("jobs: enkiTS disabled, using serial fallback");
#endif
    running_ = true;
}

void JobScheduler::Shutdown() {
#if VOX_ENABLE_JOBS
    if (scheduler_) {
        scheduler_->WaitforAllAndShutdown();
        scheduler_.reset();
    }
#endif
    running_ = false;
    vox::log::Trace("jobs: scheduler shut down");
}

void JobScheduler::ParallelFor(uint32_t count,
                                const std::function<void(uint32_t, uint32_t)>& body) {
    if (count == 0) return;

#if VOX_ENABLE_JOBS
    if (scheduler_ && running_) {
        RangedTask task(count, body);
        scheduler_->AddTaskSetToPipe(&task);
        scheduler_->WaitforTask(&task);
        return;
    }
#endif
    // Serial fallback (also used when VOX_ENABLE_JOBS is off or before Init).
    body(0, count);
}

void JobScheduler::Run(TaskTag tag, std::function<void()> fn) {
#if VOX_ENABLE_JOBS
    if (scheduler_ && running_) {
        // Heap-allocate so the task outlives this stack frame.
        // enkiTS guarantees the task completes before the scheduler shuts down
        // (WaitforAllAndShutdown), so ownership is safe here.
        auto* task = new OneshotTask(std::move(fn), tag);
        scheduler_->AddTaskSetToPipe(task);
        // Fire-and-forget: caller does not wait.  Ownership transferred to the
        // lambda below to self-delete on completion via a wrapper approach.
        // Simpler alternative: just wait immediately so we can stack-allocate.
        scheduler_->WaitforTask(task);
        delete task;
        return;
    }
#else
    (void)tag;
#endif
    // Serial fallback.
    fn();
}

}  // namespace vox::jobs
