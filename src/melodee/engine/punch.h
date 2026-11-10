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
    static constexpr unsigned history = 88200; // two seconds
    Punch() : left(new float[history]()), right(new float[history]()) {}
    // `bar` and `grid` in samples (the song's bar and the snap grid); depth
    // 0..1; tail 0 cut, 1 fade (60 ms), 2 ring (a second and a half).
    void start(PunchKind kind, float depth, double bar, double grid, unsigned tail, const int16_t* params = nullptr);
    void set_params(const int16_t* values); // the MPC kinds' K1-K3 (pad_fx_info.h ranges); nullptr: their initial values
    void stop();
    void set_depth(float d) { depth = d; }
    bool on() const { return held; }
    bool active() const { return held || fading > 0; }
    PunchKind kind_now() const { return kind; }
    void process(float* l, float* r, unsigned frames);
    void clear(); // forget the history (a project switch)

private:
    bool repeats() const {
        return kind <= PunchKind::Reverse || kind == PunchKind::BeatRepeat || kind == PunchKind::RevStepper || kind == PunchKind::Shuffle;
    }
    bool own_lines() const { return kind == PunchKind::Reverb; } // uses the history as its own delay lines: not written meanwhile
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
    bool held = false, frozen = false;
    PunchKind kind = PunchKind::Loop4;
    float depth = 1, fading = 0, fade_step = 0;
    double loop = 0, grid = 0, bar = 0, lag = 0, speed = 1;
    uint64_t age = 0; // samples since it began
    float s1[2] {}, s2[2] {}, hold[2] {};
};

} // namespace melodee
