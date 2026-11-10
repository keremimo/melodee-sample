// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

// Rendering the mix as it plays (Resample, Shift + Pad 11, and the song's
// audio mixdown): the engine host starts it with Play and ends it on a
// clock tick, sample for sample; render() hands each block of the master
// (before main volume) to process(), which copies the frames inside those
// bounds into a ring. The service thread drains the ring as it fills, into
// a sample or straight into a WAV file, so a render may run for as long as
// the song does while the ring stays small (12 s, 4 MB, made at start).
//
// States: Idle -> Cued (armed, waiting for Play) -> Running -> (Tail: the
// song ended, on until the mix falls silent) -> Done. Stop meanwhile is
// Cancelled; a ring the service thread could not keep up with is Failed.
// The audio thread moves it on, the service thread puts it back to Idle
// once it has taken everything (release()).
namespace melodee {

class Bounce {
public:
    enum class State : uint8_t { Idle, Cued, Running, Tail, Done, Cancelled, Failed };
    static constexpr uint32_t ring_frames = 1u << 19;        // 11.9 s
    static constexpr uint32_t silence_frames = 8820;         // a tail ends after 200 ms below -70 dB
    static constexpr uint32_t tail_limit = 10 * 44100;       // or after 10 s

    Bounce() : ring_(new float[size_t(ring_frames) * 2]()) {}

    // Audio thread (the engine host).
    void cue() {
        auto s = state_.load();
        if (s != State::Idle) return;
        read_.store(0); write_.store(0); frames_.store(0);
        start_at = -1; stop_at = -1; tail_frames = 0; quiet = 0;
        state_.store(State::Cued);
    }
    bool cued() const { return state_.load(std::memory_order_relaxed) == State::Cued; }
    bool running() const { return state_.load(std::memory_order_relaxed) == State::Running; }
    void begin(unsigned offset) { if (cued()) { start_at = int(offset); state_.store(State::Running); } }
    void finish(unsigned offset) { if (running()) stop_at = int(offset); }
    void tail() { if (running()) { state_.store(State::Tail); tail_frames = 0; quiet = 0; } }
    void cancel() { // a tail is cut short, but kept
        auto s = state_.load();
        if (s == State::Tail) state_.store(State::Done);
        else if (s == State::Cued || s == State::Running) state_.store(State::Cancelled);
    }
    // After each block is mixed: the frames of it that belong to the render.
    void process(const float* l, const float* r, unsigned n) {
        auto s = state_.load(std::memory_order_relaxed);
        if (s != State::Running && s != State::Tail) return;
        unsigned from = start_at >= 0 ? std::min<unsigned>(unsigned(start_at), n) : 0;
        unsigned to = stop_at >= 0 ? std::min<unsigned>(unsigned(stop_at), n) : n;
        start_at = -1;
        uint64_t w = write_.load(std::memory_order_relaxed), rd = read_.load(std::memory_order_acquire);
        if (to > from && w + (to - from) - rd > ring_frames) { state_.store(State::Failed); return; }
        float loudest = 0;
        for (unsigned f = from; f < to; ++f) {
            size_t i = size_t((w + f - from) % ring_frames) * 2;
            ring_[i] = l[f]; ring_[i + 1] = r[f];
            loudest = std::max({loudest, std::fabs(l[f]), std::fabs(r[f])});
        }
        if (to > from) {
            write_.store(w + (to - from), std::memory_order_release);
            frames_.fetch_add(to - from, std::memory_order_relaxed);
        }
        if (stop_at >= 0) { stop_at = -1; state_.store(State::Done); return; }
        if (s == State::Tail) {
            tail_frames += n;
            quiet = loudest < 0.0003f ? quiet + n : 0;
            if (quiet >= silence_frames || tail_frames >= tail_limit) state_.store(State::Done);
        }
    }

    // Any thread.
    State state() const { return state_.load(std::memory_order_acquire); }
    uint32_t frames() const { return frames_.load(std::memory_order_relaxed); } // rendered so far

    // Service thread: up to `max` frames, interleaved, as they come; the count.
    unsigned pop(float* interleaved, unsigned max) {
        uint64_t rd = read_.load(std::memory_order_relaxed), w = write_.load(std::memory_order_acquire);
        unsigned n = unsigned(std::min<uint64_t>(w - rd, max));
        for (unsigned f = 0; f < n; ++f) {
            size_t i = size_t((rd + f) % ring_frames) * 2;
            interleaved[2 * f] = ring_[i]; interleaved[2 * f + 1] = ring_[i + 1];
        }
        read_.store(rd + n, std::memory_order_release);
        return n;
    }
    bool drained() const { return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire); }
    // Service thread, once Done, Cancelled or Failed and taken: ready for the next.
    void release() {
        auto s = state_.load();
        if (s == State::Done || s == State::Cancelled || s == State::Failed) state_.store(State::Idle);
    }

private:
    std::unique_ptr<float[]> ring_;
    std::atomic<State> state_ {State::Idle};
    std::atomic<uint64_t> read_ {0}, write_ {0};
    std::atomic<uint32_t> frames_ {0};
    int start_at = -1, stop_at = -1; // audio thread: this block's bounds
    uint32_t tail_frames = 0, quiet = 0;
};

} // namespace melodee
