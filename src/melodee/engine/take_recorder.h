// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
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
// as it came, at the device's rate (44.1, 48 or 96 kHz; never converted), and
// taken() and take_rate() say so; frames() and length() count it in 44.1 kHz
// frames (dsp::model_rate), as the project and the pages count everything.
namespace melodee {

class TakeRecorder {
public:
    enum class State : uint8_t { Idle, Armed, Recording, Done, Cued };
    static constexpr uint32_t max_frames = 60 * uint32_t(dsp::model_rate);  // the longest take, 44.1 kHz frames
    static constexpr uint32_t capacity = 60 * uint32_t(dsp::max_rate);      // its buffer: 60 s at the highest rate
    static constexpr unsigned columns = 128;

    // `buffer` false: none made (a print host's, never armed); arm() then does nothing.
    explicit TakeRecorder(bool buffer = true)
        : left_(buffer ? new float[capacity]() : nullptr), right_(buffer ? new float[capacity]() : nullptr) {
        set_rate(dsp::sample_rate()); take_rate_.store(rate);
    }

    // Audio thread (through the engine host's commands). Lengths are device frames.
    // threshold_db -96 or below: at once. cued: wait for go() instead of the level.
    // quiet_end (frames kept, 0 never): the take ends on its own once it has been
    // below silence_db that long after a sound, cut 10 ms after the last one.
    void arm(float threshold_db, uint32_t length, unsigned slot, bool cued = false, uint32_t quiet_end = 0, float silence_db = -60) {
        if (state_.load() != State::Idle || !left_) return;
        threshold = threshold_db <= -96 ? 0 : std::pow(10.0f, threshold_db / 20.0f);
        quiet = quiet_end;
        silence = std::pow(10.0f, silence_db / 20.0f);
        last_loud = 0; heard = false;
        const uint32_t most = std::min(capacity, uint32_t(60 * uint64_t(rate)));
        limit = std::min(length ? length : most, most);
        tail_frames = rate / 100;
        take_rate_.store(rate);
        slot_.store(slot);
        frames_.store(0);
        start_at = 0;
        for (auto& c : peaks) c.store(0, std::memory_order_relaxed);
        limit_.store(limit);
        state_.store(cued ? State::Cued : threshold <= 0 ? State::Recording : State::Armed);
    }
    // Audio thread: the device's rate (EngineHost::set_audio_format), for the takes armed from now on. One
    // recording meanwhile ends there, its frames at the rate it was taken at.
    void set_rate(float device) {
        rate = std::max(8000u, unsigned(std::lround(device)));
        auto s = state_.load();
        if (s == State::Armed || s == State::Cued || s == State::Recording) stop();
    }
    void start_now() { auto s = state_.load(); if (s == State::Armed || s == State::Cued) state_.store(State::Recording); }
    // The pattern came round `offset` frames into this block: a cued take starts there.
    void go(unsigned offset) { if (state_.load() == State::Cued) { start_at = offset; state_.store(State::Recording); } }
    void stop() {
        auto s = state_.load();
        if (s == State::Armed || s == State::Cued) state_.store(State::Idle);
        if (s == State::Recording) state_.store(frames_.load() ? State::Done : State::Idle);
    }
    void process(const float* l, const float* r, unsigned n) {
        float peak_l = 0, peak_r = 0;
        for (unsigned f = 0; f < n; ++f) { peak_l = std::max(peak_l, std::fabs(l[f])); peak_r = std::max(peak_r, std::fabs(r[f])); }
        float peak = std::max(peak_l, peak_r);
        // The meter falls 0.95 a 128-frame block at 44.1 kHz: the same time at any rate and block.
        const float fall = n == 128 && rate == unsigned(dsp::model_rate) ? 0.95f : std::pow(0.95f, float(n) * dsp::model_rate / (128.0f * float(rate)));
        level_.store(std::max(peak, level_.load(std::memory_order_relaxed) * fall), std::memory_order_relaxed);
        level_l_.store(std::max(peak_l, level_l_.load(std::memory_order_relaxed) * fall), std::memory_order_relaxed);
        level_r_.store(std::max(peak_r, level_r_.load(std::memory_order_relaxed) * fall), std::memory_order_relaxed);
        auto s = state_.load();
        if (s == State::Armed && peak >= threshold) s = State::Recording, state_.store(s);
        if (s != State::Recording) return;
        uint32_t at = frames_.load(std::memory_order_relaxed);
        unsigned skip = std::min(start_at, n); // a cued take starts where the pattern came round
        start_at = 0;
        l += skip; r += skip; n -= skip;
        uint32_t kept = at;
        for (unsigned f = 0; f < n && kept < limit; ++f, ++kept) {
            left_[kept] = l[f]; right_[kept] = r[f];
            if (std::fabs(l[f]) >= silence || std::fabs(r[f]) >= silence) { last_loud = kept; heard = true; }
            unsigned c = unsigned(uint64_t(kept) * columns / limit);
            uint8_t v = uint8_t(std::min(1.0f, std::max(std::fabs(l[f]), std::fabs(r[f]))) * 255);
            if (v > peaks[c].load(std::memory_order_relaxed)) peaks[c].store(v, std::memory_order_relaxed);
        }
        if (quiet && heard && kept - last_loud >= quiet) { // quiet long enough: done, ending where the sound did
            kept = std::min(kept, last_loud + 1 + tail_frames);
            frames_.store(kept, std::memory_order_release);
            state_.store(State::Done);
            return;
        }
        frames_.store(kept, std::memory_order_release);
        if (kept >= limit) state_.store(State::Done);
    }

    // Any thread. frames() and length() in 44.1 kHz frames; taken() the take's own (device) frames, at take_rate().
    State state() const { return state_.load(); }
    uint32_t taken() const { return frames_.load(std::memory_order_acquire); }
    unsigned take_rate() const { return take_rate_.load(std::memory_order_relaxed); }
    uint32_t frames() const { return to_model(taken()); }
    uint32_t length() const { return to_model(limit_.load()); }
    float level() const { return level_.load(std::memory_order_relaxed); }
    float level_left() const { return level_l_.load(std::memory_order_relaxed); }   // each side, for the stereo meter
    float level_right() const { return level_r_.load(std::memory_order_relaxed); }
    uint8_t peak(unsigned column) const { return peaks[column % columns].load(std::memory_order_relaxed); }
    unsigned slot() const { return slot_.load(); }

    // Service thread, while Done: taken() frames at take_rate().
    const float* left() const { return left_.get(); }
    const float* right() const { return right_.get(); }
    void release() { if (state_.load() == State::Done) state_.store(State::Idle); }

private:
    uint32_t to_model(uint32_t device) const {
        return uint32_t((uint64_t(device) * uint32_t(dsp::model_rate) + take_rate() / 2) / std::max(1u, take_rate()));
    }
    std::unique_ptr<float[]> left_, right_;
    float threshold = 0.03f;
    unsigned rate = unsigned(dsp::model_rate);
    uint32_t tail_frames = unsigned(dsp::model_rate) / 100; // 10 ms kept after the last sound
    uint32_t quiet = 0, last_loud = 0;
    float silence = 0.001f;
    bool heard = false;
    unsigned start_at = 0;
    uint32_t limit = max_frames;
    std::atomic<State> state_ {State::Idle};
    std::atomic<uint32_t> frames_ {0}, limit_ {max_frames};
    std::atomic<unsigned> slot_ {0}, take_rate_ {unsigned(dsp::model_rate)};
    std::atomic<float> level_ {0}, level_l_ {0}, level_r_ {0};
    std::array<std::atomic<uint8_t>, columns> peaks {};
};

} // namespace melodee
