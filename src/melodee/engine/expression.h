// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/model/project.h"
#include <array>
#include <cmath>

// Pad expression, shared by the synth engines (PROPHET, FM6, SID, CZ): the
// mod matrix's sources summed per destination (each engine then decides how
// far a destination moves), the velocity curve, smoothed pressure and the
// pressure vibrato. Pressure does what aftertouch does on synthesizers that
// have it: AT > VIB adds vibrato, AT > BRIGHT opens the sound
// (MELODEE_PARITY.md, phase 1d). Audio thread only.
namespace melodee::expression {

// A voice's mod sources, each about -1..1 (velocity, pressure, envelope 0..1).
struct Sources { float lfo = 0, velocity = 0, pressure = 0, wheel = 0, key = 0, random = 0, env = 0; };

inline float source(ModSource s, const Sources& in) {
    switch (s) {
    case ModSource::Lfo: return in.lfo;
    case ModSource::Velocity: return in.velocity;
    case ModSource::Aftertouch: return in.pressure;
    case ModSource::Wheel: return in.wheel;
    case ModSource::KeyTrack: return in.key;
    case ModSource::Random: return in.random;
    case ModSource::FilterEnv: return in.env;
    default: return 0;
    }
}

// Every route adds source x amount (amount -1..1) to its destination.
using Sums = std::array<float, size_t(ModDest::Count)>;
inline Sums sums(const std::array<ModRoute, mod_routes>& mods, const Sources& in) {
    Sums out {};
    for (const auto& r : mods) {
        if (!r.used() || !r.amount || r.destination >= unsigned(ModDest::Count)) continue;
        out[r.destination] += source(ModSource(r.source), in) * r.amount / float(mod_amount_max);
    }
    return out;
}
inline float at(const Sums& s, ModDest d) { return s[size_t(d)]; }

// Routes into LFO rate: an engine has one LFO, so only voice-free sources
// (the wheel, the LFO itself) move it. Full amount is three octaves.
inline float lfo_rate_octaves(const std::array<ModRoute, mod_routes>& mods, float wheel, float lfo) {
    float octaves = 0;
    for (const auto& r : mods)
        if (r.used() && ModDest(r.destination) == ModDest::LfoRate) {
            float x = ModSource(r.source) == ModSource::Wheel ? wheel : ModSource(r.source) == ModSource::Lfo ? lfo : 0;
            octaves += x * r.amount / float(mod_amount_max) * 3;
        }
    return octaves;
}

// A sequenced note's Accent: about 4 dB over its velocity, which may already be full.
constexpr float accent_gain = 1.6f;

// A slid voice's pitch: `offset` semitones from its new key, closing on it
// with a 20 ms time constant (mostly there in 60 ms, a 303's slide).
inline float slide_settle(float offset, unsigned frames) {
    if (std::fabs(offset) < 0.001f) return 0;
    return offset * std::exp(-float(frames) / (0.02f * dsp::sample_rate()));
}

// Velocity into level, depth 0..1. Squared, so soft hits are clearly
// softer: at depth 0.7 a velocity of 40 plays about 9 dB under a full hit.
inline float velocity_gain(float velocity, float depth) {
    return 1.0f - depth * (1.0f - velocity * velocity);
}

// Pressure arrives in steps from the pads; a voice follows it over about
// 10 ms so vibrato depth and brightness don't click.
inline void follow(float& value, float target, unsigned frames) {
    value += (target - value) * (1.0f - std::exp(-float(frames) / (0.010f * dsp::sample_rate())));
}

// AT > VIB 100 swings the pitch a semitone each way at full pressure.
inline float vibrato_semitones(float pressure, int at_vib) { return pressure * at_vib / 100.0f; }
// AT > BRIGHT's default, and the vibrato's rate (about 5.3 Hz on the Rate scale).
constexpr int default_at_vib = 35, default_at_bright = 50, default_vib_rate = 73;

// The pressure vibrato (and the matrix's LFO on engines without one of
// their own): a sine, -1..1.
struct Vibrato {
    float phase = 0;
    float next(float hz, unsigned frames) {
        phase += hz * float(frames) / dsp::sample_rate();
        phase -= std::floor(phase);
        return std::sin(2 * dsp::pi * phase);
    }
};

} // namespace melodee::expression
