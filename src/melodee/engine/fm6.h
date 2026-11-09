// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine.h"
#include "melodee/engine/engine_info.h"
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
enum Param : int8_t { Algorithm, Feedback, Volume, OpBase, Count = OpBase + ops * op_params };
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
    Fm6Engine();
    void note_on(int key, int velocity) override;
    void note_off(int key) override;
    void all_notes_off() override;
    void silence() override;
    void set_param(int id, int value) override;
    void set_wheel(float value) override { wheel = value; }
    void set_bend(float value) override { bend = value; }
    void render(float* out, unsigned frames) override;
    bool sounding() const override;

private:
    struct Voice {
        int key = -1;
        bool held = false;
        uint32_t age = 0;
        float velocity = 0;
        std::array<float, fm6::ops> phase {}, out {};
        std::array<dsp::Envelope, fm6::ops> env {};
        float feedback = 0;
    };
    void update();
    std::array<int, fm6::Count> p {};
    std::array<Voice, max_voices> voices {};
    std::array<dsp::Envelope::Shape, fm6::ops> shapes {};
    std::array<float, fm6::ops> ratios {}, levels {};
    uint32_t clock = 0;
    float wheel = 0, bend = 0;
    bool dirty = true;
};

} // namespace melodee
