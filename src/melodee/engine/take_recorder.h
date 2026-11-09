// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

// One take at a time, for the Record page: armed, it waits for the level to
// pass a threshold (or a second press), records up to a length, and then
// holds the take until the service thread has copied it out. The buffer is
// made up front; process() runs on the audio thread and never allocates.
// State moves Idle -> Armed -> Recording -> Done (audio thread) and
// Done -> Idle (service thread, release()), so the two never write the
// buffer at once.
namespace melodee {

class TakeRecorder {
public:
    enum class State : uint8_t { Idle, Armed, Recording, Done };
    static constexpr uint32_t max_frames = 60 * 44100;
    static constexpr unsigned columns = 128;

    TakeRecorder() : left_(new float[max_frames]()), right_(new float[max_frames]()) {}

    // Audio thread (through the engine host's commands).
    void arm(float threshold_db, uint32_t length, float gain_db, unsigned slot) {
        if (state_.load() != State::Idle) return;
        threshold = std::pow(10.0f, threshold_db / 20.0f);
        limit = std::min(length ? length : max_frames, max_frames);
        gain = std::pow(10.0f, gain_db / 20.0f);
        slot_.store(slot);
        frames_.store(0);
        for (auto& c : peaks) c.store(0, std::memory_order_relaxed);
        limit_.store(limit);
        state_.store(threshold <= 0.0011f ? State::Recording : State::Armed); // -60 dB: at once
    }
    void set_gain(float db) { gain = std::pow(10.0f, db / 20.0f); }
    void start_now() { if (state_.load() == State::Armed) state_.store(State::Recording); }
    void stop() {
        auto s = state_.load();
        if (s == State::Armed) state_.store(State::Idle);
        if (s == State::Recording) state_.store(frames_.load() ? State::Done : State::Idle);
    }
    void process(const float* l, const float* r, unsigned n) {
        float peak = 0;
        for (unsigned f = 0; f < n; ++f) peak = std::max({peak, std::fabs(l[f] * gain), std::fabs(r[f] * gain)});
        float shown = level_.load(std::memory_order_relaxed) * 0.95f;
        level_.store(std::max(peak, shown), std::memory_order_relaxed);
        auto s = state_.load();
        if (s == State::Armed && peak >= threshold) s = State::Recording, state_.store(s);
        if (s != State::Recording) return;
        uint32_t at = frames_.load(std::memory_order_relaxed);
        unsigned take = unsigned(std::min<uint32_t>(n, limit - at));
        for (unsigned f = 0; f < take; ++f) {
            left_[at + f] = l[f] * gain; right_[at + f] = r[f] * gain;
            unsigned c = unsigned(uint64_t(at + f) * columns / limit);
            uint8_t v = uint8_t(std::min(1.0f, std::max(std::fabs(left_[at + f]), std::fabs(right_[at + f]))) * 255);
            if (v > peaks[c].load(std::memory_order_relaxed)) peaks[c].store(v, std::memory_order_relaxed);
        }
        frames_.store(at + take, std::memory_order_release);
        if (at + take >= limit) state_.store(State::Done);
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
    float threshold = 0.03f, gain = 1;
    uint32_t limit = max_frames;
    std::atomic<State> state_ {State::Idle};
    std::atomic<uint32_t> frames_ {0}, limit_ {max_frames};
    std::atomic<unsigned> slot_ {0};
    std::atomic<float> level_ {0};
    std::array<std::atomic<uint8_t>, columns> peaks {};
};

} // namespace melodee
