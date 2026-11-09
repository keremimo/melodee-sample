// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine.h"
#include "melodee/engine/engine_info.h"
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
    Count
};
float shape(unsigned wave, float phase, float dcw); // one sample of a wave at a phase (0..1) and bend (0..1)
const EngineInfo& info();
}

class CzEngine final : public Engine {
public:
    static constexpr unsigned max_voices = 8;
    CzEngine();
    void note_on(int key, int velocity) override;
    void note_off(int key) override;
    void all_notes_off() override;
    void silence() override;
    void set_param(int id, int value) override;
    void set_bend(float value) override { bend = value; }
    void set_wheel(float value) override { wheel = value; }
    void render(float* out, unsigned frames) override;
    bool sounding() const override;

private:
    struct Voice {
        int key = -1;
        bool held = false;
        uint32_t age = 0;
        float velocity = 0;
        std::array<float, 2> phase {};
        std::array<bool, 2> second {}; // which of the two waves this cycle
        dsp::Envelope dcw, dca;
    };
    void update();
    std::array<int, cz::Count> p {};
    std::array<Voice, max_voices> voices {};
    dsp::Envelope::Shape dcw_shape, dca_shape;
    float bend = 0, wheel = 0;
    uint32_t clock = 0;
    bool dirty = true;
};

} // namespace melodee
