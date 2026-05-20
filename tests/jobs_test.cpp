// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "jobs/JobScheduler.h"

#include <atomic>
#include <cstdint>
#include <vector>

using vox::jobs::JobScheduler;
using vox::jobs::TaskTag;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr uint32_t kCount = 1'000'000u;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("JobScheduler: init and shutdown") {
    JobScheduler sched;
    CHECK_FALSE(sched.Running());
    sched.Init();
    CHECK(sched.Running());
    sched.Shutdown();
    CHECK_FALSE(sched.Running());
}

TEST_CASE("JobScheduler: ParallelFor writes index*2 correctly") {
    JobScheduler sched;
    sched.Init();

    std::vector<uint32_t> data(kCount, 0u);

    // Each element [i] must be written by exactly one worker as i * 2.
    // No two ranges overlap, so no atomics are needed.
    sched.ParallelFor(kCount, [&data](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            data[i] = i * 2u;
        }
    });

    for (uint32_t i = 0; i < kCount; ++i) {
        REQUIRE(data[i] == i * 2u);
    }

    sched.Shutdown();
}

TEST_CASE("JobScheduler: ParallelFor atomic sum proves multithreaded work") {
    JobScheduler sched;
    sched.Init();

    // Sum all integers 0..kCount-1 atomically across threads.
    // Expected = kCount * (kCount - 1) / 2.
    std::atomic<uint64_t> total{0};
    sched.ParallelFor(kCount, [&total](uint32_t begin, uint32_t end) {
        uint64_t local = 0;
        for (uint32_t i = begin; i < end; ++i) {
            local += i;
        }
        total.fetch_add(local, std::memory_order_relaxed);
    });

    const uint64_t expected = static_cast<uint64_t>(kCount) *
                              (static_cast<uint64_t>(kCount) - 1u) / 2u;
    CHECK(total.load() == expected);

    sched.Shutdown();
}

TEST_CASE("JobScheduler: Run executes a one-shot task") {
    JobScheduler sched;
    sched.Init();

    std::atomic<int> ran{0};
    sched.Run(TaskTag::Default, [&ran]() {
        ran.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(ran.load() == 1);

    // Exercise all tags for compilation coverage.
    sched.Run(TaskTag::CacheHot, [&ran]() { ran.fetch_add(1); });
    sched.Run(TaskTag::IOBound,  [&ran]() { ran.fetch_add(1); });
    sched.Run(TaskTag::AudioRT,  [&ran]() { ran.fetch_add(1); });
    CHECK(ran.load() == 4);

    sched.Shutdown();
}

TEST_CASE("JobScheduler: ParallelFor with count=0 is a no-op") {
    JobScheduler sched;
    sched.Init();

    bool called = false;
    sched.ParallelFor(0, [&called](uint32_t, uint32_t) { called = true; });
    CHECK_FALSE(called);

    sched.Shutdown();
}

TEST_CASE("JobScheduler: Init with explicit thread count") {
    JobScheduler sched;
    sched.Init(2);
    CHECK(sched.Running());
    sched.Shutdown();
    CHECK_FALSE(sched.Running());
}
