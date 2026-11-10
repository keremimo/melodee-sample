// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/master_comp.h"
#include "melodee/engine/dsp.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
float fs() { return dsp::sample_rate(); } // the device's rate now
float follow(float ms) { return std::exp(-1.0f / std::max(ms * 0.001f * fs(), 1.0f)); }
} // namespace

void MasterCompressor::set(const MasterComp& settings) {
    s = settings;
    attack = follow(comp_attack_ms(s.attack));
    release = follow(comp_release_ms(s.release));
    float amount = std::clamp<int>(s.amount, 0, 100) / 100.0f;
    threshold = -amount * 30;           // 0 .. -30 dB
    ratio = 1 + amount * amount * 9;    // 1:1 .. 10:1
    // Makeup: half of what a signal at 0 dB loses.
    makeup = std::pow(10.0f, (-threshold * (1 - 1 / ratio)) * 0.5f / 20.0f);
    boost = std::pow(10.0f, std::clamp<int>(s.boost, 0, 24) / 20.0f);
}

void MasterCompressor::process(float* left, float* right, unsigned frames) {
    if (!s.on) { shown.store(0, std::memory_order_relaxed); return; }
    float most = 0;
    for (unsigned f = 0; f < frames; ++f) {
        float l = left[f] * boost, r = right[f] * boost;
        float level = std::max(std::fabs(l), std::fabs(r));
        env = level > env ? level + (env - level) * attack : level + (env - level) * release;
        if ((f & 3) == 0) { // the gain computer every fourth sample, the gain ramping between
            float over = 20 * std::log10(std::max(env, 1e-6f)) - threshold;
            float reduce = over > 0 ? over * (1 - 1 / ratio) : 0;
            most = std::max(most, reduce);
            float target = std::pow(10.0f, -reduce / 20.0f) * makeup;
            step = (target - gain) * 0.25f;
        }
        gain += step;
        l *= gain; r *= gain;
        if (s.color) {
            // A slow wobble (a few cents), the lows doubled in parallel, then soft saturation.
            wobble += 0.6f / fs(); wobble -= std::floor(wobble);
            delay_l[at] = l; delay_r[at] = r;
            float back = 40 + 6 * std::sin(2 * dsp::pi * wobble);
            float pos = float(at) + line - back;
            unsigned i = unsigned(pos);
            float frac = pos - float(i);
            l = delay_l[i % line] + (delay_l[(i + 1) % line] - delay_l[i % line]) * frac;
            r = delay_r[i % line] + (delay_r[(i + 1) % line] - delay_r[i % line]) * frac;
            if (++at >= line) at = 0;
            constexpr float k = 0.0085f; // a one-pole low pass near 60 Hz
            low_l += (l - low_l) * k; low_r += (r - low_r) * k;
            l = dsp::soft_clip((l + low_l * 0.6f) * 1.1f) / 1.1f;
            r = dsp::soft_clip((r + low_r * 0.6f) * 1.1f) / 1.1f;
        }
        left[f] = l; right[f] = r;
    }
    shown.store(most, std::memory_order_relaxed);
}

} // namespace melodee
