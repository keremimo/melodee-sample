// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/model/sampler.h"
#include <array>
#include <cstdint>
#include <memory>

// Punch: a performance effect held on a pad over the master, one track or
// one pad, written for sample-base. It keeps the last two seconds of what
// passes through. Repeats (loops, stutter, reverse) let one slice through,
// then freeze the history and play that slice again; tape kinds (stop, half, downer, upper,
// scratch) read it behind the live sound; the rest work on the sound as it
// comes. Audio thread only; the history is made up front.
namespace melodee {

enum class PunchKind : uint8_t {
    Loop4, Loop8, Loop16, Loop32, Stutter, Reverse, Stop, Half, Low, High, Phone, Crush, Alias, Gate, Echo, Wobble, // bank 1
    SweepDown, SweepUp, Ring, Flange, Drive, Pan, Tremolo, Downer, Upper, Scratch, Vinyl,                           // bank 2
    // Parity phase 7b: the MPC's Pad FX, pads 1-16 (punch_mpc.cpp), each with up to three parameters.
    HalfSpeed, Chorus, Flanger, Phaser, Comb, LpLfo, HpLfo, BpLfo, RingMod, LoFi, Color, Granulator, BeatRepeat, RevStepper, Delay, Reverb,
    // and Flex Beat's time effects Melodee had none of (also on bank 2 after Vinyl).
    Backspin, TranceGate, Shuffle, Pump,
    Count
};
constexpr unsigned punch_mpc_first = unsigned(PunchKind::HalfSpeed); // the MPC's sixteen, in pad order
constexpr unsigned punch_params = 3;
inline bool punch_is_mpc(PunchKind k) { return unsigned(k) >= punch_mpc_first && unsigned(k) < punch_mpc_first + 16; }
constexpr unsigned punch_count = unsigned(PunchKind::Count);
static_assert(punch_count == punch_kinds, "the model's count of punches");
const char* punch_name(PunchKind kind);      // as on the pads: "LOOP 4"
const char* punch_title(PunchKind kind);     // as in a sentence: "Loop 4"
constexpr const char* punch_tails[] {"Cut", "Fade", "Ring"};
constexpr const char* punch_snaps[] {"1/4", "1/8", "1/16", "1/32"};
constexpr unsigned punch_snap_count = 4;

class Punch {
public:
    static constexpr unsigned history = 192000; // two seconds at 96 kHz
    // Every edge a punch makes (its start, Cut, a repeat's turn, a gate's) is faded over this long, so none clicks.
    static constexpr double seam_seconds = 0.002;
    Punch() : left(new float[history]()), right(new float[history]()) {}
    // `bar` and `grid` in samples (the song's bar and the snap grid); depth
    // 0..1; tail 0 cut (over seam_seconds), 1 fade (60 ms), 2 ring (a second and a half).
    void start(PunchKind kind, float depth, double bar, double grid, unsigned tail, const int16_t* params = nullptr);
    void set_params(const int16_t* values); // the MPC kinds' K1-K3 (pad_fx_info.h ranges); nullptr: their initial values
    void stop();
    void set_depth(float d) { depth = d; }
    bool on() const { return held; }
    bool active() const { return held || fading > 0; }
    PunchKind kind_now() const { return kind; }
    // Run while it is off too: the history then always holds what just played, for the next start.
    void process(float* l, float* r, unsigned frames);
    // Forget the history (a project switch): what came before reads as silence from here, without wiping its
    // 1.5 MB on the audio thread (a project load clears ten of them).
    void clear();
    // The frames a repeat's turn crossfades over at the device's rate now (the frames a slice is long),
    // shorter for a short slice; for tests.
    static double seam_frames(double slice) { return std::max(1.0, std::min(seam_seconds * dsp::sample_rate(), slice / 4)); }

private:
    bool repeats() const {
        return kind <= PunchKind::Reverse || kind == PunchKind::BeatRepeat || kind == PunchKind::RevStepper || kind == PunchKind::Shuffle;
    }
    bool own_lines() const { return kind == PunchKind::Reverb; } // uses the history as its own delay lines: not written meanwhile
    // A repeat's slice again (`since`: frames since the start): forwards, each turn's end crossfaded into what
    // came a loop before the slice's start (the first pass's too); or backwards, each turn faded out and each
    // but the first in (the first goes on from where the sound was).
    void forwards(double since, float& wet_l, float& wet_r) const;
    void backwards(double since, float& wet_l, float& wet_r) const;
    void begin_mpc();                                            // the MPC kinds' running state, at start
    void mpc(float dry_l, float dry_r, float& wet_l, float& wet_r, double since); // one sample of an MPC or Flex Beat kind
    std::array<int16_t, punch_params> param {};
    // Their running state: phases, filter and allpass stages, grains, the reverb's lines.
    double phase = 0, phase2 = 0;
    std::array<float, 16> st {};
    std::array<float, 12> ap {};
    struct Grain { double back = 0; uint32_t age = 0, length = 0; };
    std::array<Grain, 8> grains {};
    double next_grain = 0;
    uint32_t seed = 0x1234567u;
    std::array<unsigned, 12> at {}, len {};
    float read(const float* line, double back) const;
    std::unique_ptr<float[]> left, right;
    unsigned write = 0;
    unsigned fresh = 0; // frames written into the history since clear() (at most `history`): older ones read as 0
    bool held = false, frozen = false;
    bool relive = false; // Rev Stepper: a new slice let through after its turns, faded in
    PunchKind kind = PunchKind::Loop4;
    float depth = 1, fading = 0, fade_step = 0;
    double loop = 0, grid = 0, bar = 0, lag = 0, speed = 1;
    double seam = 1, ease_in = 1; // a repeat's turn crossfade (seam_frames, room allowing) and the start's fade, frames
    // The wet sound faded by `gain` (0..1) toward silence.
    static void fade(float& l, float& r, double gain) { l *= float(gain); r *= float(gain); }
    uint64_t age = 0;    // samples since it began (Rev Stepper starts it again with each new slice)
    uint64_t begun = 0;  // samples since start(), for its fade in
    float s1[2] {}, s2[2] {}, hold[2] {};
    // Per-frame constants written for 44.1 kHz frames, at the device's rate (start(); dsp.h; the same at 44.1 kHz).
    struct PerFrame {
        double stretch = 1;                    // device frames in a 44.1 kHz frame: lengths written in 44.1 kHz frames
        float noise = 1, click = 1, keep = 1;  // white noise's level, a one-sample click's, the share of a chance kept
        float dc = 0.002f, rumble = 0.004f, crackle = 0.9f, gate = 0.02f; // Color's Tube Amp and Vinyl, Trance Gate
        float damp = 0.4f, damp_in = 0.6f;     // Reverb's damping
        dsp::RateRatio ratio {};               // for counts of 44.1 kHz frames (Alias, LoFi)
    } per_frame;
};

} // namespace melodee
