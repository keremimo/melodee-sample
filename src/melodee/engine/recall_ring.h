// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>

// Audio Recall (Shift + Sample Record): the input source, always going into
// a ring, so the last 25 seconds can be taken back after they were played.
// The audio thread writes every block; the service thread copies the last
// stretch out while it keeps going. The ring holds a second more than is
// ever copied, so the oldest frames copied are never the ones being
// overwritten during the copy (it takes a few milliseconds); a copy checks
// afterwards that the writer stayed clear, and says so when it did not.
// 26 s of stereo float: about 9 MB, made at start.
namespace melodee {

class RecallRing {
public:
    static constexpr uint32_t rate = 44100, seconds = 25, capacity = (seconds + 1) * rate;

    RecallRing() : left_(new float[capacity]()), right_(new float[capacity]()) {}

    // Audio thread.
    void write(const float* l, const float* r, unsigned n) {
        uint64_t at = written_.load(std::memory_order_relaxed);
        uint32_t i = uint32_t(at % capacity);
        for (unsigned f = 0; f < n; ++f) {
            left_[i] = l[f]; right_[i] = r[f];
            if (++i == capacity) i = 0;
        }
        written_.store(at + n, std::memory_order_release);
    }
    // Audio thread: the source changed, so what came before is not recalled with it.
    void restart() { since_.store(written_.load(std::memory_order_relaxed), std::memory_order_release); }

    // Any thread: frames there are to recall, up to 25 s.
    uint32_t filled() const {
        uint64_t w = written_.load(std::memory_order_acquire), s = since_.load(std::memory_order_acquire);
        return uint32_t(std::min<uint64_t>(w - std::min(w, s), uint64_t(seconds) * rate));
    }
    // Service thread: the last `frames` (at most filled()) into l and r; false
    // when the writer caught up with the copy (nothing usable then).
    bool copy_last(uint32_t frames, float* l, float* r) const {
        uint64_t end = written_.load(std::memory_order_acquire);
        frames = std::min<uint32_t>(frames, uint32_t(std::min<uint64_t>(end, uint64_t(seconds) * rate)));
        uint64_t from = end - frames;
        for (uint32_t f = 0; f < frames; ++f) {
            uint32_t i = uint32_t((from + f) % capacity);
            l[f] = left_[i]; r[f] = right_[i];
        }
        // The writer moved on meanwhile; it must not have reached the first frame copied.
        return written_.load(std::memory_order_acquire) - from <= capacity;
    }

private:
    std::unique_ptr<float[]> left_, right_;
    std::atomic<uint64_t> written_ {0}, since_ {0};
};

} // namespace melodee
