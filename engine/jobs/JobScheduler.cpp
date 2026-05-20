// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#include "jobs/JobScheduler.h"

#include "platform/Log.h"

namespace vox::jobs {

void JobScheduler::Init(unsigned worker_threads) {
    // TODO(jobs): construct enki::TaskScheduler, detect CCD topology, pin
    // CacheHot workers to the X3D CCD. Stub for the skeleton pass.
    (void)worker_threads;
    vox::log::Trace("jobs: scheduler stub init");
    running_ = true;
}

void JobScheduler::Shutdown() { running_ = false; }

}  // namespace vox::jobs
