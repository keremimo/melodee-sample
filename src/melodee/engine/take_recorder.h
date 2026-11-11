// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/model_rate.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

// One take at a time, for the Record page: armed, it waits for the level to
// pass a threshold (or a second press), records up to a length, and then
// holds the take until the service thread has copied it out. Cued instead
// (Rec length Seq while playing), it waits for the engine host to say the
// pattern came round, at a frame of the block. The buffer is made up front;
// process() runs on the audio thread and never allocates. The engine host
// applies the gain before it. State moves Idle -> Armed or Cued ->
// Recording -> Done (audio thread) and Done -> Idle (service thread,
// release()), so the two never write the buffer at once. The take is kept
// at 44.1 kHz (ToModelRate), so frames(), length() and peak() count those.
namespace melodee {

class TakeRecorder {
public:
    enum class State : uint8_t { Idle, Armed, Recording, Done, Cued };
    static constexpr uint32_t max_frames = 60 * uint32_t(dsp::model_rate);
    static constexpr unsigned columns = 128;

    TakeRecorder() : left_(new float[max_frames]()), right_(new float[max_frames]()) {}

    // Audio thread (through the engine host's commands).
    // threshold_db -96 or below: at once. cued: wait for go() instead of the level.
    // quiet_end (frames kept, 0 never): the take ends on its own once it has been
    // below silence_db that long after a sound, cut 10 ms after the last one.
    void arm(float threshold_db, uint32_t length, unsigned slot, bool cued = false, uint32_t quiet_end = 0, float silence_db = -60) {
        if (state_.load() != State::Idle) return;
        threshold = threshold_db <= -96 ? 0 : std::pow(10.0f, threshold_db / 20.0f);
        quiet = quiet_end;
        silence = std::pow(10.0f, silence_db / 20.0f);
        last_loud = 0; heard = false;
        limit = std::min(length ? length : max_frames, max_frames);
        slot_.store(slot);
        frames_.store(0);
        start_at = 0;
        for (auto& c : peaks) c.store(0, std::memory_order_relaxed);
        limit_.store(limit);
        convert.reset();
        state_.store(cued ? State::Cued : threshold <= 0 ? State::Recording : State::Armed);
    }
    // Audio thread: the device's rate (EngineHost::set_audio_format).
    void set_rate(float device) { convert.set_rate(device); }
    void start_now() { auto s = state_.load(); if (s == State::Armed || s == State::Cued) state_.store(State::Recording); }
    // The pattern came round `offset` frames into this block: a cued take starts there.
    void go(unsigned offset) { if (state_.load() == State::Cued) { start_at = offset; state_.store(State::Recording); } }
    void stop() {
        auto s = state_.load();
        if (s == State::Armed || s == State::Cued) state_.store(State::Idle);
        if (s == State::Recording) state_.store(frames_.load() ? State::Done : State::Idle);
    }
    void process(const float* l, const float* r, unsigned n) {
        float peak = 0;
        for (unsigned f = 0; f < n; ++f) peak = std::max({peak, std::fabs(l[f]), std::fabs(r[f])});
        float shown = level_.load(std::memory_order_relaxed) * 0.95f;
        level_.store(std::max(peak, shown), std::memory_order_relaxed);
        auto s = state_.load();
        if (s == State::Armed && peak >= threshold) s = State::Recording, state_.store(s);
        if (s != State::Recording) return;
        uint32_t at = frames_.load(std::memory_order_relaxed);
        unsigned skip = std::min(start_at, n); // a cued take starts where the pattern came round
        start_at = 0;
        l += skip; r += skip; n -= skip;
        uint32_t kept = at;
        for (unsigned f = 0; f < n && kept < limit; ++f) convert.feed(l[f], r[f], [&](float cl, float cr) {
            if (kept >= limit) return;
            left_[kept] = cl; right_[kept] = cr;
            if (std::fabs(cl) >= silence || std::fabs(cr) >= silence) { last_loud = kept; heard = true; }
            unsigned c = unsigned(uint64_t(kept) * columns / limit);
            uint8_t v = uint8_t(std::min(1.0f, std::max(std::fabs(cl), std::fabs(cr))) * 255);
            if (v > peaks[c].load(std::memory_order_relaxed)) peaks[c].store(v, std::memory_order_relaxed);
            ++kept;
        });
        if (quiet && heard && kept - last_loud >= quiet) { // quiet long enough: done, ending where the sound did
            kept = std::min(kept, last_loud + 1 + tail_frames);
            frames_.store(kept, std::memory_order_release);
            state_.store(State::Done);
            return;
        }
        frames_.store(kept, std::memory_order_release);
        if (kept >= limit) state_.store(State::Done);
    }

    // Any thread.
    State state() const { return state_.load(); }
    uint32_t frames() const { return frames_.load(std::memory_order_acquire); }
    uint32_t length() const { return limit_.load(); }
    float level() const { return level_.load(std::memory_order_relaxed); }
    uint8_t peak(unsigned column) const { return peaks[column % columns].load(std::memory_order_relaxed); }
    unsigned slot() const { return slot_.load(); }

    // Service thread, while Done.
    const float* left() const { return left_.get(); }
    const float* right() const { return right_.get(); }
    void release() { if (state_.load() == State::Done) state_.store(State::Idle); }

private:
    std::unique_ptr<float[]> left_, right_;
    ToModelRate convert;
    float threshold = 0.03f;
    static constexpr uint32_t tail_frames = uint32_t(dsp::model_rate) / 100; // 10 ms kept after the last sound
    uint32_t quiet = 0, last_loud = 0;
    float silence = 0.001f;
    bool heard = false;
    unsigned start_at = 0;
    uint32_t limit = max_frames;
    std::atomic<State> state_ {State::Idle};
    std::atomic<uint32_t> frames_ {0}, limit_ {max_frames};
    std::atomic<unsigned> slot_ {0};
    std::atomic<float> level_ {0};
    std::array<std::atomic<uint8_t>, columns> peaks {};
};

} // namespace melodee
