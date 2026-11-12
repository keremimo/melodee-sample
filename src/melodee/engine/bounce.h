// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
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
// the song does while the ring stays small (11 s at 96 kHz, 8 MB, made at start).
//
// States: Idle -> Cued (armed, waiting for Play) -> Running -> (Tail: the
// song ended, on until the mix falls silent) -> Done. Stop meanwhile is
// Cancelled; a ring the service thread could not keep up with is Failed.
// The audio thread moves it on, the service thread puts it back to Idle
// once it has taken everything (release()). The render is kept as it plays,
// at the device's rate (never converted): what pop() gives is at rate(), and
// frames() counts it in 44.1 kHz frames, as the pages count everything.
namespace melodee {

class Bounce {
public:
    enum class State : uint8_t { Idle, Cued, Running, Tail, Done, Cancelled, Failed };
    static constexpr uint32_t ring_frames = 1u << 20;        // 10.9 s at 96 kHz
    static constexpr float silence_seconds = 0.2f;           // a tail ends after 200 ms below -70 dB
    static constexpr uint32_t tail_seconds = 10;             // or after 10 s

    // `ring` false: none made (a print host's, never cued).
    explicit Bounce(bool ring = true) : ring_(ring ? new float[size_t(ring_frames) * 2]() : nullptr) { set_rate(dsp::sample_rate()); }

    // Audio thread (the engine host).
    void cue() {
        auto s = state_.load();
        if (s != State::Idle || !ring_) return;
        read_.store(0); write_.store(0); frames_.store(0);
        start_at = -1; stop_at = -1; tail_frames = 0; quiet = 0;
        render_rate_.store(device_rate, std::memory_order_relaxed);
        state_.store(State::Cued);
    }
    // Audio thread: the device's rate (EngineHost::set_audio_format). A render meanwhile is cancelled: its
    // frames would change rate partway.
    void set_rate(float device) {
        device_rate = std::max(8000u, unsigned(std::lround(device)));
        cancel();
        if (state_.load() == State::Idle) render_rate_.store(device_rate, std::memory_order_relaxed);
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
    // Roadmap P4: the frames of a block of `n` that process() will take (stems are taken by the same bounds), none when
    // no render runs. Audio thread, before process().
    bool span(unsigned n, unsigned& from, unsigned& to) const {
        auto s = state_.load(std::memory_order_relaxed);
        if (s != State::Running && s != State::Tail) return false;
        from = start_at >= 0 ? std::min<unsigned>(unsigned(start_at), n) : 0;
        to = stop_at >= 0 ? std::min<unsigned>(unsigned(stop_at), n) : n;
        return to > from;
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
        uint32_t kept = 0;
        for (unsigned f = from; f < to; ++f, ++kept) {
            loudest = std::max({loudest, std::fabs(l[f]), std::fabs(r[f])});
            size_t i = size_t((w + kept) % ring_frames) * 2;
            ring_[i] = l[f]; ring_[i + 1] = r[f];
        }
        if (kept) {
            write_.store(w + kept, std::memory_order_release);
            frames_.fetch_add(kept, std::memory_order_relaxed);
        }
        if (stop_at >= 0) { stop_at = -1; state_.store(State::Done); return; }
        if (s == State::Tail) {
            tail_frames += kept;
            quiet = loudest < 0.0003f ? quiet + kept : 0;
            if (quiet >= uint32_t(silence_seconds * float(device_rate)) || tail_frames >= tail_seconds * device_rate) state_.store(State::Done);
        }
    }

    // Any thread.
    State state() const { return state_.load(std::memory_order_acquire); }
    unsigned rate() const { return render_rate_.load(std::memory_order_relaxed); } // the frames' rate (the device's at cue())
    uint32_t taken() const { return frames_.load(std::memory_order_relaxed); }    // rendered so far, at rate()
    uint32_t frames() const {                                                      // the same in 44.1 kHz frames
        return uint32_t((uint64_t(taken()) * uint32_t(dsp::model_rate) + rate() / 2) / std::max(1u, rate()));
    }

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
    unsigned device_rate = unsigned(dsp::model_rate); // audio thread: the device's
    std::atomic<unsigned> render_rate_ {unsigned(dsp::model_rate)};
    std::atomic<State> state_ {State::Idle};
    std::atomic<uint64_t> read_ {0}, write_ {0};
    std::atomic<uint32_t> frames_ {0};
    int start_at = -1, stop_at = -1; // audio thread: this block's bounds
    uint32_t tail_frames = 0, quiet = 0;
};

// Roadmap P4, stems to the card: each track (after its chain, level, pan and mute, as it joins the mix) and each bus's
// return, stereo, the same frames as the mixdown beside them; up to stem_count of them, those in `mask` taken. Made by
// the service thread when first wanted (21 MB), handed to the engine host, which writes; the service thread reads.
class StemRing {
public:
    static constexpr unsigned stem_count = 20;      // tracks 0-15, buses A-D
    static constexpr uint32_t ring_frames = 1u << 17; // 2.7 s at 48 kHz: the service thread drains every pass
    StemRing() : ring_(new float[size_t(ring_frames) * stem_count * 2]()) {}
    // Service thread, before the render is armed: which stems, from the start.
    void arm(uint32_t mask) { mask_.store(mask); read_.store(0); write_.store(0); failed_.store(false); }
    uint32_t mask() const { return mask_.load(std::memory_order_relaxed); }
    // Audio thread: frames [from, to) of this block, stem s's left and right at `l[s]`, `r[s]` (nullptr: silence).
    void write(const float* const* l, const float* const* r, unsigned from, unsigned to) {
        uint64_t w = write_.load(std::memory_order_relaxed), rd = read_.load(std::memory_order_acquire);
        if (w + (to - from) - rd > ring_frames) { failed_.store(true, std::memory_order_relaxed); return; }
        uint32_t m = mask_.load(std::memory_order_relaxed);
        for (unsigned f = from; f < to; ++f) {
            float* frame = ring_.get() + size_t((w + f - from) % ring_frames) * stem_count * 2;
            for (unsigned s = 0; s < stem_count; ++s) {
                bool on = ((m >> s) & 1) && l[s];
                frame[2 * s] = on ? l[s][f] : 0.0f; frame[2 * s + 1] = on ? r[s][f] : 0.0f;
            }
        }
        write_.store(w + (to - from), std::memory_order_release);
    }
    // Service thread: up to `max` frames of stem `s` and every other into `out` (a stem's frames apart, interleaved
    // stereo, stem_count blocks of 2 * max floats); the count.
    unsigned pop(float* out, unsigned max) {
        uint64_t rd = read_.load(std::memory_order_relaxed), w = write_.load(std::memory_order_acquire);
        unsigned n = unsigned(std::min<uint64_t>(w - rd, max));
        for (unsigned f = 0; f < n; ++f) {
            const float* frame = ring_.get() + size_t((rd + f) % ring_frames) * stem_count * 2;
            for (unsigned s = 0; s < stem_count; ++s) { out[size_t(s) * 2 * max + 2 * f] = frame[2 * s]; out[size_t(s) * 2 * max + 2 * f + 1] = frame[2 * s + 1]; }
        }
        read_.store(rd + n, std::memory_order_release);
        return n;
    }
    bool drained() const { return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire); }
    bool failed() const { return failed_.load(std::memory_order_relaxed); }
private:
    std::unique_ptr<float[]> ring_;
    std::atomic<uint32_t> mask_ {0};
    std::atomic<uint64_t> read_ {0}, write_ {0};
    std::atomic<bool> failed_ {false};
};

} // namespace melodee
