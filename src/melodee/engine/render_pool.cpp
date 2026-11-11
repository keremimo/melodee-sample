// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/render_pool.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <pthread.h>
#include <sched.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace melodee {
namespace {
constexpr auto spin = std::chrono::microseconds(200); // a block's second run follows its first within this

void relax() {
#if defined(__aarch64__)
    asm volatile("yield");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

// Denormals to zero, as on the audio thread (EngineHost::render).
void flush_denormals() {
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("msr fpcr, %0" ::"r"(fpcr | (uint64_t(1) << 24)));
#endif
}

#if defined(__linux__)
void futex_wait(std::atomic<uint32_t>& word, uint32_t seen) {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAIT_PRIVATE, seen, nullptr, nullptr, 0);
}
void futex_wake(std::atomic<uint32_t>& word) {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}
#endif
} // namespace

RenderPool::~RenderPool() { stop(); }

void RenderPool::start(unsigned count, int priority) {
    stop();
#if defined(__linux__)
    stopping.store(false);
    for (unsigned n = 0; n < count; ++n) {
        threads.emplace_back([this] { work(); });
        sched_param param {};
        param.sched_priority = priority;
        pthread_setschedparam(threads.back().native_handle(), SCHED_FIFO, &param); // best effort, as the audio thread's
        pthread_setname_np(threads.back().native_handle(), "melodee-render");
    }
#else
    (void)count; (void)priority;
#endif
}

void RenderPool::stop() {
    if (threads.empty()) return;
    stopping.store(true);
    generation.fetch_add(1, std::memory_order_acq_rel);
#if defined(__linux__)
    futex_wake(generation);
#endif
    for (auto& t : threads) t.join();
    threads.clear();
}

bool RenderPool::claim(uint32_t gen) {
    uint64_t t = ticket.load(std::memory_order_acquire);
    while (true) {
        if (uint32_t(t >> 32) != gen || (t & 0xffff) >= ((t >> 16) & 0xffff)) return false;
        if (ticket.compare_exchange_weak(t, t + 1, std::memory_order_acq_rel, std::memory_order_acquire)) break;
    }
    // Claimed: the run cannot end (nor `call`, `context` change) until this job counts as done.
    call(context, unsigned(t & 0xffff));
    done.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void RenderPool::dispatch(unsigned count, Call job, void* job_context) {
    count = std::min(count, 0xffffu);
    call = job;
    context = job_context;
    done.store(0, std::memory_order_relaxed);
    uint32_t gen = generation.load(std::memory_order_relaxed) + 1;
    ticket.store(uint64_t(gen) << 32 | uint64_t(count) << 16, std::memory_order_release);
    // seq_cst with the worker's side: either it sees the new generation or this sees it asleep.
    generation.store(gen, std::memory_order_seq_cst);
#if defined(__linux__)
    if (sleepers.load(std::memory_order_seq_cst)) futex_wake(generation);
#endif
    while (claim(gen)) {}
    while (done.load(std::memory_order_acquire) < count) relax();
}

void RenderPool::work() {
    flush_denormals();
    uint32_t seen = generation.load(std::memory_order_acquire);
    while (true) {
        // Wait for the next run: spin a little (the block's next run is near), then sleep.
        uint32_t gen = generation.load(std::memory_order_acquire);
        if (gen == seen) {
            auto until = std::chrono::steady_clock::now() + spin;
            while ((gen = generation.load(std::memory_order_acquire)) == seen && std::chrono::steady_clock::now() < until) relax();
        }
#if defined(__linux__)
        while (gen == seen) {
            sleepers.fetch_add(1, std::memory_order_seq_cst);
            gen = generation.load(std::memory_order_seq_cst);
            if (gen == seen) futex_wait(generation, seen);
            sleepers.fetch_sub(1, std::memory_order_acq_rel);
            gen = generation.load(std::memory_order_acquire);
        }
#endif
        if (stopping.load(std::memory_order_acquire)) return;
        seen = gen;
        while (claim(gen)) {}
    }
}

} // namespace melodee
