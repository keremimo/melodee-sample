// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "dsp/frame_ring.h"

// What is recorded or rendered at the device's rate (a take, Audio Recall,
// Resample and the song's mixdown) is kept at 44.1 kHz, the rate every
// sample and frame count of a project is at (dsp::model_rate). At 44.1 kHz
// frames pass straight through; at 48 kHz they are converted linearly
// (what falls above 22 kHz there is under 2 kHz wide and folds above 20 kHz);
// at 96 kHz two Butterworth low passes at 19 kHz go first, so the octave
// above does not fold down into what is kept. Audio thread only.
namespace melodee {

class ToModelRate {
public:
    void set_rate(float device) {
        rate = device;
        through = device == dsp::model_rate;
        filtered = device > 50000.0f;
        // Two RBJ low passes (Q of a fourth-order Butterworth's pairs) at 19 kHz.
        const float q[2] {0.5412f, 1.3066f};
        float w = 2 * dsp::pi * 19000.0f / device, cw = std::cos(w);
        for (unsigned n = 0; n < 2; ++n) {
            float alpha = std::sin(w) / (2 * q[n]), a0 = 1 + alpha;
            stage[n] = {(1 - cw) / 2 / a0, (1 - cw) / a0, (1 - cw) / 2 / a0, -2 * cw / a0, (1 - alpha) / a0};
        }
        reset();
    }
    // A fresh take: no state carried over from the last one.
    void reset() {
        for (auto& s : state) s = {};
        resampler.reset(unsigned(rate), unsigned(dsp::model_rate));
    }
    // One device frame in; `emit(l, r)` for the 44.1 kHz frame it makes, if any.
    template <class Emit> void feed(float l, float r, Emit&& emit) {
        if (through) { emit(l, r); return; }
        if (filtered) {
            for (unsigned n = 0; n < 2; ++n) { l = run(stage[n], state[n], l); r = run(stage[n], state[2 + n], r); }
        }
        resampler.feed(l, r, emit);
    }

private:
    struct Coefficients { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct State { float z1 = 0, z2 = 0; };
    static float run(const Coefficients& c, State& s, float x) {
        float y = c.b0 * x + s.z1;
        s.z1 = c.b1 * x - c.a1 * y + s.z2;
        s.z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    LinearResampler resampler;
    Coefficients stage[2] {};
    State state[4] {};
    bool through = true, filtered = false;
    float rate = dsp::model_rate;
};

} // namespace melodee
