// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/expression.h"
#include <array>

// CZ: phase distortion, written for sample-base. A cosine read with a bent
// phase: the DCW (its own envelope) bends it from a pure sine into saw,
// square, pulse, double sine or saw-pulse, or windows a higher partial
// into a resonance. A second wave can alternate with the first each cycle;
// a second line, detuned, can play with the first. Eight voices.
namespace melodee {

namespace cz {
enum Param : int8_t {
    Wave1, Wave2, Dcw,
    Line, Detune, Octave,
    DcwAmount, DcwAttack, DcwDecay, DcwSustain, DcwRelease, KeyFollow,
    DcaAttack, DcaDecay, DcaSustain, DcaRelease, Velocity,
    AtVib, AtBright, LfoRate, // pad pressure and the LFO (phase 1d): last, so older files load
    Count
};
constexpr unsigned legacy_count = AtVib; // what files from before phase 1d carry
float shape(unsigned wave, float phase, float dcw); // one sample of a wave at a phase (0..1) and bend (0..1)
const EngineInfo& info();
}

class CzEngine final : public Engine {
public:
    static constexpr unsigned max_voices = 8;
    static constexpr unsigned control_frames = 128; // modulation's step (ControlStep)
    CzEngine();
    void note_on(int key, int velocity) override;
    void note_off(int key) override;
    void all_notes_off() override;
    void silence() override;
    void set_param(int id, int value) override;
    void set_bend(float value) override { bend = value; step.restart(); }
    void set_wheel(float value) override { wheel = value; step.restart(); }
    void set_mod(unsigned route, const ModRoute& value) override { if (route < mods.size()) mods[route] = value; step.restart(); }
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
        float velocity = 0, pressure = 0, touch = 0, random = 0;
        std::array<float, 2> phase {};
        std::array<bool, 2> second {}; // which of the two waves this cycle
        dsp::Envelope dcw, dca;
        std::array<float, 2> dt {};             // the control step's: each line's phase step,
        float follow = 1, gain = 0, open = 0;   // the DCW's key follow, the level and the brightness
    };
    void update();
    std::array<int, cz::Count> p {};
    std::array<Voice, max_voices> voices {};
    dsp::Envelope::Shape dcw_shape, dca_shape;
    std::array<ModRoute, mod_routes> mods {};
    expression::Vibrato lfo; // the pressure vibrato and the matrix's LFO
    dsp::Noise noise;
    float bend = 0, wheel = 0, lfo_value = 0;
    uint32_t clock = 0;
    float accent = 1; // for the notes started next (set_accent)
    bool dirty = true;
    ControlStep step; // the modulation's own steps (roadmap P2)
};

} // namespace melodee
