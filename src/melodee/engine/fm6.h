// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/expression.h"
#include <array>

// FM6: six-operator FM written for sample-base. Each operator is a sine
// at a ratio of the note with its own level and envelope; an algorithm says
// which operators modulate which and which are heard; operator 6 can feed
// back into itself. Eight voices. Audio thread only.
namespace melodee {

namespace fm6 {
constexpr unsigned ops = 6, op_params = 8, algorithms = 8;
// Op n's parameters start at op_base(n): ratio, fine, level, attack, decay, sustain, release, velocity.
enum OpParam : int8_t { Ratio, Fine, Level, Attack, Decay, Sustain, Release, Velocity };
enum Param : int8_t {
    Algorithm, Feedback, Volume, OpBase,
    AtVib = OpBase + ops * op_params, AtBright, LfoRate, // pad pressure and the LFO (phase 1d): last, so older files load
    Count
};
constexpr unsigned legacy_count = AtVib; // what files from before phase 1d carry
constexpr int op_param(unsigned op, OpParam p) { return OpBase + int(op) * int(op_params) + p; }
const char* algorithm_name(unsigned algorithm);
// Which operators an algorithm hears (bit n: op n+1) and which modulate which (mod[n]: bits of ops into op n).
struct Routing { uint8_t carriers; std::array<uint8_t, ops> into; };
const Routing& routing(unsigned algorithm);
float ratio_of(int value);
const EngineInfo& info();
}

class Fm6Engine final : public Engine {
public:
    static constexpr unsigned max_voices = 8;
    static constexpr unsigned control_frames = 128; // modulation's step (ControlStep)
    Fm6Engine();
    void note_on(int key, int velocity) override;
    void note_off(int key) override;
    void all_notes_off() override;
    void silence() override;
    void set_param(int id, int value) override;
    void set_wheel(float value) override { wheel = value; step.restart(); }
    void set_bend(float value) override { bend = value; step.restart(); }
    void set_mod(unsigned route, const ModRoute& value) override { if (route < mods.size()) mods[route] = value; step.restart(); }
    void set_pressure(int key, float value) override;
    void set_accent(float gain) override { accent = gain; }
    void slide(int from, int to, int velocity, bool glide) override;
    void render(float* out, unsigned frames) override;
    bool relieve(bool steal) override;
    bool sounding() const override;

private:
    struct Voice {
        int key = -1;
        bool held = false;
        uint32_t age = 0;
        float accent = 1, slide = 0; // the sequencer's: a gain, and semitones off the key (expression::slide_settle)
        float velocity = 0, pressure = 0, touch = 0, random = 0;
        std::array<float, fm6::ops> phase {}, out {};
        std::array<dsp::Envelope, fm6::ops> env {};
        float feedback = 0;
        // The control step's: each operator's level and phase step, and op 6's feedback amount and level into it.
        std::array<float, fm6::ops> gain {}, phase_step {};
        float feedback_amount = 0, self = 0;
    };
    void update();
    std::array<int, fm6::Count> p {};
    std::array<Voice, max_voices> voices {};
    std::array<dsp::Envelope::Shape, fm6::ops> shapes {};
    std::array<float, fm6::ops> ratios {}, levels {};
    std::array<ModRoute, mod_routes> mods {};
    expression::Vibrato lfo; // the pressure vibrato and the matrix's LFO
    dsp::Noise noise;
    uint32_t clock = 0;
    float accent = 1; // for the notes started next (set_accent)
    float wheel = 0, bend = 0, lfo_value = 0;
    ControlStep step; // the modulation's own steps (roadmap P2)
    bool dirty = true;
};

} // namespace melodee
