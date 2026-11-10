// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include <cmath>

// RBJ cookbook biquads for Melodee's effects, written for sample-base: each
// fills b0, b1, b2, a1, a2 (normalised by a0) into `out`.
namespace melodee::rbj {

inline void shelf(float* out, float hz, float gain_db, bool high) {
    const float fs = dsp::sample_rate(); constexpr float two_pi = 2 * dsp::pi;
    float A = std::pow(10.0f, gain_db / 40.0f), w = two_pi * hz / fs, cw = std::cos(w), sw = std::sin(w);
    float alpha = sw / 2 * std::sqrt(2.0f), s = 2 * std::sqrt(A) * alpha;
    float b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
    }
    out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b2 / a0; out[3] = a1 / a0; out[4] = a2 / a0;
}

inline void peak(float* out, float hz, float gain_db, float q) {
    const float fs = dsp::sample_rate(); constexpr float two_pi = 2 * dsp::pi;
    float A = std::pow(10.0f, gain_db / 40.0f), w = two_pi * hz / fs, alpha = std::sin(w) / (2 * q), cw = std::cos(w);
    float a0 = 1 + alpha / A;
    out[0] = (1 + alpha * A) / a0; out[1] = -2 * cw / a0; out[2] = (1 - alpha * A) / a0; out[3] = -2 * cw / a0; out[4] = (1 - alpha / A) / a0;
}

// Low pass (high = false) or high pass at `hz` with quality `q` (0.707: no bump).
inline void pass(float* out, float hz, float q, bool high) {
    const float fs = dsp::sample_rate(); constexpr float two_pi = 2 * dsp::pi;
    hz = std::fmin(hz, fs * 0.45f);
    float w = two_pi * hz / fs, cw = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    float b1 = high ? -(1 + cw) : 1 - cw, b0 = high ? (1 + cw) / 2 : (1 - cw) / 2;
    out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b0 / a0; out[3] = -2 * cw / a0; out[4] = (1 - alpha) / a0;
}

} // namespace melodee::rbj
