// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

// The audio thread's helpers: a block's independent jobs (each track's engine,
// then each track's chain) spread over the other cores. run() hands out the
// jobs one at a time, in the order given, to the workers and to the calling
// thread itself, and returns once every one is done, so the caller sums the
// results in its own order and a block sounds the same as with no workers.
//
// Workers run at the audio thread's real-time priority. Between the two runs
// of a block they spin briefly; after that they sleep on a futex and run()
// wakes them, so an idle Melodee keeps its cores idle. Nothing here allocates
// or locks once started. On Linux they sleep on a futex; elsewhere (a
// computer's tests) in short naps. With no workers run() does every job on
// the calling thread.
namespace melodee {

class RenderPool {
public:
    RenderPool() = default;
    ~RenderPool();
    RenderPool(const RenderPool&) = delete;
    RenderPool& operator=(const RenderPool&) = delete;

    // Before the audio thread starts (or while it is stopped): `count` workers, 0 none.
    void start(unsigned count, int priority);
    void stop();
    unsigned workers() const { return unsigned(threads.size()); }

    // Audio thread: job(i) for each i below `count` (at most 65535), then back.
    template <class F> void run(unsigned count, F&& job) {
        using Job = std::remove_reference_t<F>;
        if (threads.empty() || count < 2) {
            for (unsigned i = 0; i < count; ++i) job(i);
            return;
        }
        dispatch(count, [](void* context, unsigned i) { (*static_cast<Job*>(context))(i); }, &job);
    }

private:
    using Call = void (*)(void*, unsigned);
    void dispatch(unsigned count, Call call, void* context);
    bool claim(uint32_t generation); // one job of that run, if any is left; false when none
    void work();

    std::vector<std::thread> threads;
    // A run in one word: its generation (high 32 bits), how many jobs (16) and the next to hand out (low 16),
    // so a worker that wakes late never takes a job of the next run as one of its own, nor reads half of one.
    std::atomic<uint64_t> ticket {0};
    std::atomic<uint32_t> generation {0}; // the futex word: bumped for each run, and to stop
    std::atomic<uint32_t> done {0};
    std::atomic<uint32_t> sleepers {0};
    std::atomic<bool> stopping {false};
    Call call = nullptr;      // the run's, written before its ticket and read only once a job of it is claimed
    void* context = nullptr;
};

} // namespace melodee
