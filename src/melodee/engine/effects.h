// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/engine/effect_info.h"
#include <array>
#include <cstdint>
#include <memory>

// Melodee's effects, written for sample-base: one Effect object per chain
// slot, changed in place on the audio thread. Effects that need delay
// memory (reverbs, echoes, chorus, flanger, tape) take a block from a pool
// made up front, so nothing allocates while audio runs; when the pool is
// used up the effect passes sound through untouched.
namespace melodee {

class DelayPool {
public:
    static constexpr unsigned blocks = 48;
    static constexpr unsigned half = 144000;          // 1.5 s at 96 kHz (3.3 s at 44.1), one channel
    static constexpr unsigned block_floats = 2 * half;
    DelayPool();
    float* take();       // nullptr when none is free
    void give(float* block);
private:
    std::unique_ptr<float[]> memory;
    std::array<bool, blocks> used {};
};

// The song's clock as tempo-synced effects hear it (Pumper): the host sets
// it at the start of each block, before any effect runs. Audio thread only.
struct EffectClock {
    double quarter = 22050; // samples a quarter note
    double beats = 0;       // quarter notes since Play (running on while stopped), at the block's start
};
inline EffectClock effect_clock;

bool effect_needs_memory(EffectKind kind); // delay memory from the pool

class Effect {
public:
    // How long Bypass takes to fade the effect out or in again (a cut between wet and dry clicks).
    static constexpr float bypass_seconds = 0.01f;
    void set_kind(EffectKind kind, DelayPool& pool); // sets the kind's initial parameters too
    void set_param(unsigned index, int value);
    // Bypassed, the effect fades out over bypass_seconds, then rests; when it comes back it starts from
    // silence (clear()), never from the echoes and tails it held when it stopped.
    void set_bypass(bool on) { bypass = on; }
    EffectKind kind() const { return current; }
    bool bypassed() const { return bypass; }
    int param(unsigned index) const { return index < effect_params ? p[index] : 0; }
    void process(float* left, float* right, unsigned frames);
    // Forget everything it holds (delay lines, filters, envelopes), keeping its kind and parameters.
    void clear();
    // Roadmap P4, delay compensation: frames its output lags its input (a Limiter's LOOKAHEAD; 0 for the rest, and
    // bypassed or out of memory).
    unsigned latency() const { return current == EffectKind::Limiter && memory && !bypass ? delay : 0; }
    // Roadmap P4: what a Compressor's or Gate's detector hears for the next process() only (another track or bus,
    // `frames` of it); nullptr: what goes through it.
    void set_key(const float* left, const float* right) { key_l = left; key_r = right; }

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float run(float x) { float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };
    struct Svf { float s1 = 0, s2 = 0; };
    void update();
    void reset_state(); // the running state back to silence (not the delay memory)
    // This frame's share of the wet sound: the kind's mix, times Bypass's fade as it moves a frame on.
    float blend() {
        float aim = bypass ? 0.0f : 1.0f;
        if (on != aim) on = aim > on ? std::min(aim, on + fade_step) : std::max(aim, on - fade_step);
        return mix * on;
    }
    void update_extra();                                    // the parity phase 7 effects (effects_extra.cpp)
    void process_extra(float* left, float* right, unsigned frames);
    void reverb(float& l, float& r);
    float delayed(const float* line, unsigned at, float back) const; // fractional read behind `at`
    // SYNC (roadmap P4): the delay's time this frame, gliding to the note value's at the clock's tempo (a tempo change
    // bends the echoes as a tape's speed would, never clicks); `limit` the line's reach. Off: the TIME it was given.
    float sync_back(float limit);
    float synced = 0; // frames, as it glides; 0 not yet set

    EffectKind current = EffectKind::None;
    bool bypass = false;
    float on = 1, fade_step = 1;  // Bypass's fade: 1 heard, 0 bypassed; and how far it moves a frame
    bool resting = false;         // bypassed and faded out: not run, and cleared before it runs again
    std::array<int, effect_params> p {};
    float* memory = nullptr;
    DelayPool* home = nullptr; // the pool set_kind() took from: a Limiter takes a block only once it looks ahead
    const float* key_l = nullptr; // set_key(): the detector's for this process() (roadmap P4)
    const float* key_r = nullptr;
    unsigned write = 0;

    // Derived values (update()).
    float mix = 1, a = 0, b = 0, c = 0, d = 0, e = 0;
    unsigned delay = 0;
    std::array<Biquad, 12> eq {}; // EQ: six; Amp sim: its tone stack and cabinet, three each a side
    float svf_g = 0.5f, svf_k = 1;
    // Per-frame constants written for 44.1 kHz frames, at the device's rate (dsp.h; the same at 44.1 kHz):
    // falls and one-pole steps (whose, update() says), white noise's level, the share of a frame's chance
    // kept, Shimmer's window (frames), and the rates' ratio for counts of 44.1 kHz frames (Crush, Spit, Tape).
    std::array<float, 2> fall {1, 1}, glide {0, 0};
    float noise_gain = 1, keep = 1, window = 2048;
    dsp::RateRatio ratio {};

    // Running state.
    float lfo = 0, env = 0, env2 = 0, gain = 1, hold_l = 0, hold_r = 0, tone_l = 0, tone_r = 0;
    unsigned hold_count = 0;
    std::array<Svf, 2> svf {};
    std::array<std::array<float, 6>, 2> allpass {};
    float phaser_l = 0, phaser_r = 0;
    // Shimmer: an octave-up shifter in the reverb's feedback; Spit: its spits; Formant: three band passes a side.
    unsigned shift_at = 0, spit_left = 0;
    float shift_phase = 0, shimmer_l = 0, shimmer_r = 0, spit_gain = 1;
    uint32_t seed = 0x9e3779b9u;
    std::array<Svf, 6> formant {};
    std::array<float, 3> formant_g {};
    float formant_k = 1;
    // Reverb: four combs and two allpasses a side, after a predelay, inside `memory`.
    std::array<unsigned, 8> comb_length {}, comb_at {};
    std::array<float, 8> comb_filter {};
    std::array<unsigned, 4> pass_length {}, pass_at {};
    unsigned predelay = 0;
    float feedback = 0, damp = 0;
    // Parity phase 7: Spring's dispersion (first-order allpasses, twelve a side),
    // Multi-chorus's voices, and small running values the newer effects keep.
    std::array<float, 24> chirp {};
    std::array<float, 6> voice_phase {};
    std::array<float, 8> state {};
};

} // namespace melodee
