// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/expression.h"
#include <array>

// SID: a chip voice in the spirit of the C64's, written for sample-base:
// triangle, saw, pulse or noise (noise stepping at the note's rate), pulse
// width swept by its own LFO, ring and sync against a second oscillator, a
// multimode filter (low, band, high pass) with an envelope, and the chip's
// ADSR. MODEL picks the filter's character: the 8580 clean, the 6581 darker
// and gritty. Six voices. Audio thread only.
namespace melodee {

namespace sid {
enum Param : int8_t {
    Wave, PulseWidth, Detune, Ring, Sync, Model,
    PwmDepth, PwmRate, Crush,
    Cutoff, Resonance, Mode, EnvAmount, KeyTrack,
    Attack, Decay, Sustain, Release,
    Velocity, AtVib, AtBright, LfoRate, // velocity, pad pressure and the LFO (phase 1d): last, so older files load
    Count
};
constexpr unsigned legacy_count = Velocity; // what files from before phase 1d carry
const EngineInfo& info();
}

class SidEngine final : public Engine {
public:
    static constexpr unsigned max_voices = 6;
    SidEngine();
    void note_on(int key, int velocity) override;
    void note_off(int key) override;
    void all_notes_off() override;
    void silence() override;
    void set_param(int id, int value) override;
    void set_bend(float value) override { bend = value; }
    void set_wheel(float value) override { wheel = value; }
    void set_mod(unsigned route, const ModRoute& value) override { if (route < mods.size()) mods[route] = value; }
    void set_pressure(int key, float value) override;
    void set_accent(float gain) override { accent = gain; }
    void slide(int from, int to, int velocity, bool glide) override;
    void render(float* out, unsigned frames) override;
    bool sounding() const override;

private:
    struct Voice {
        int key = -1;
        bool held = false;
        uint32_t age = 0;
        float accent = 1, slide = 0; // the sequencer's: a gain, and semitones off the key (expression::slide_settle)
        float velocity = 0, pressure = 0, touch = 0, random = 0, phase = 0, phase2 = 0, noise = 0, s1 = 0, s2 = 0;
        dsp::Envelope env;
    };
    void update();
    std::array<int, sid::Count> p {};
    std::array<Voice, max_voices> voices {};
    dsp::Envelope::Shape shape;
    std::array<ModRoute, mod_routes> mods {};
    expression::Vibrato vibrato; // the pressure vibrato and the matrix's LFO
    dsp::Noise noise;
    float lfo = 0, bend = 0, wheel = 0, lfo_value = 0; // lfo: the pulse width sweep's phase
    uint32_t clock = 0;
    float accent = 1; // for the notes started next (set_accent)
    bool dirty = true;
};

} // namespace melodee
