// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

// Audio Recall (Shift + Sample Record): the input source, always going into
// a ring, so the last 25 seconds can be taken back after they were played.
// The audio thread writes every block; the service thread copies the last
// stretch out while it keeps going. The ring holds a second more than is
// ever copied, so the oldest frames copied are never the ones being
// overwritten during the copy (it takes a few milliseconds); a copy checks
// afterwards that the writer stayed clear, and says so when it did not.
// Kept as it comes, at the device's rate (never converted): 26 s of stereo
// float at the highest rate, 20 MB, made at start; frames counted at rate().
namespace melodee {

class RecallRing {
public:
    static constexpr uint32_t seconds = 25, capacity = (seconds + 1) * uint32_t(dsp::max_rate);

    RecallRing() : left_(new float[capacity]()), right_(new float[capacity]()) { set_rate(dsp::sample_rate()); }

    // Audio thread.
    void write(const float* l, const float* r, unsigned n) {
        uint64_t at = written_.load(std::memory_order_relaxed);
        uint32_t i = uint32_t(at % held);
        for (unsigned f = 0; f < n; ++f) {
            left_[i] = l[f]; right_[i] = r[f];
            if (++i == held) i = 0;
        }
        written_.store(at + n, std::memory_order_release);
    }
    // Audio thread: the device's rate changed; what came before is not recalled with it.
    void set_rate(float device) {
        unsigned now = std::max(8000u, unsigned(std::lround(device)));
        restart();
        rate_.store(now, std::memory_order_release);
        held = std::min(capacity, (seconds + 1) * now);
    }
    // Any thread: the frames' rate (the device's).
    unsigned rate() const { return rate_.load(std::memory_order_acquire); }
    // Audio thread: the source changed, so what came before is not recalled with it.
    void restart() { since_.store(written_.load(std::memory_order_relaxed), std::memory_order_release); }

    // Any thread: frames there are to recall, up to 25 s, at rate().
    uint32_t filled() const {
        uint64_t w = written_.load(std::memory_order_acquire), s = since_.load(std::memory_order_acquire);
        return uint32_t(std::min<uint64_t>(w - std::min(w, s), uint64_t(seconds) * rate()));
    }
    // Service thread: the last `frames` (at most filled()) into l and r; false
    // when the writer caught up with the copy (nothing usable then).
    bool copy_last(uint32_t frames, float* l, float* r) const {
        const unsigned at_rate = rate();
        const uint32_t ring = std::min(capacity, (seconds + 1) * at_rate);
        uint64_t end = written_.load(std::memory_order_acquire);
        frames = std::min<uint32_t>(frames, uint32_t(std::min<uint64_t>(end, uint64_t(seconds) * at_rate)));
        uint64_t from = end - frames;
        for (uint32_t f = 0; f < frames; ++f) {
            uint32_t i = uint32_t((from + f) % ring);
            l[f] = left_[i]; r[f] = right_[i];
        }
        // The writer moved on meanwhile; it must not have reached the first frame copied (nor the rate changed).
        return written_.load(std::memory_order_acquire) - from <= ring && rate() == at_rate;
    }

private:
    std::unique_ptr<float[]> left_, right_;
    uint32_t held = capacity; // audio thread: the frames in use, 26 s at the rate
    std::atomic<unsigned> rate_ {unsigned(dsp::model_rate)};
    std::atomic<uint64_t> written_ {0}, since_ {0};
};

// How much of the ring Audio Recall keeps (the Settings page's Recall rows).
struct RecallOptions {
    uint8_t grab = 0;        // RecallGrab: 0 the last phrase, 1 from the first sound, 2 everything
    float gap = 1;           // Last Phrase: a quiet stretch this long (seconds) begins a new phrase
    bool trim_end = true;    // the quiet after the last sound left out
    float silence = 0.001f;  // what counts as quiet (linear; -60 dB)
};

// The part of a copied ring to keep, [first, end): 10 ms before the sound it
// starts on and 10 ms after the last sound (when trimmed). False when there
// is only quiet (never for everything). Frames at `rate`.
inline bool recall_bounds(const float* l, const float* r, uint32_t frames, uint32_t rate, const RecallOptions& o,
                          uint32_t& first, uint32_t& end) {
    first = 0; end = frames;
    if (o.grab == 2) return frames > 0;
    auto loud = [&](uint32_t f) { return std::fabs(l[f]) >= o.silence || std::fabs(r[f]) >= o.silence; };
    uint32_t pad = rate / 100, gap = std::max(1u, uint32_t(std::lround(double(o.gap) * rate)));
    uint32_t last = frames;
    while (last > 0 && !loud(last - 1)) --last; // just after the last sound
    if (!last) return false;
    uint32_t start = 0;
    if (o.grab == 0) { // back from the last sound to a quiet stretch of `gap`
        uint32_t quiet = 0;
        start = last - 1;
        for (uint32_t f = last; f-- > 0;) {
            if (loud(f)) { start = f; quiet = 0; continue; }
            if (++quiet >= gap) break;
        }
        if (quiet < gap) start = 0; // no such stretch: from the first sound
    }
    while (start < last && !loud(start)) ++start;
    first = start > pad ? start - pad : 0;
    end = o.trim_end ? std::min(frames, last + pad) : frames;
    return true;
}

} // namespace melodee
