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

float MasterCompressor::envelope(float level) {
    env = level > env ? level + (env - level) * attack : level + (env - level) * release;
    return env;
}

float MasterCompressor::target(float reduce) const {
    return std::pow(10.0f, -reduce / 20.0f) * (s.sidechain ? 1.0f : makeup);
}

// A slow wobble (a few cents), the lows doubled in parallel, then soft saturation.
void MasterCompressor::colour(float& l, float& r) {
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

void MasterCompressor::note(float env_most, float reduce_most) {
    float db = 20 * std::log10(std::max(env_most, 1e-5f));
    // Kept to the most: a block the screen takes between this load and store is lost, which only drops a column's peak.
    if (db > heard_most.load(std::memory_order_relaxed)) heard_most.store(db, std::memory_order_relaxed);
    if (reduce_most > took_most.load(std::memory_order_relaxed)) took_most.store(reduce_most, std::memory_order_relaxed);
}

void MasterCompressor::take_activity(float& level_db, float& reduce_db) {
    level_db = heard_most.exchange(-90, std::memory_order_relaxed);
    reduce_db = took_most.exchange(0, std::memory_order_relaxed);
}

void MasterCompressor::process(float* left, float* right, unsigned frames) {
    if (!s.on) { shown.store(0, std::memory_order_relaxed); return; }
    if (s.sidechain) { // duck() did the dynamics before the master chain; what is left here is Color
        if (s.color) for (unsigned f = 0; f < frames; ++f) colour(left[f], right[f]);
        return;
    }
    float most = 0, loudest = 0;
    for (unsigned f = 0; f < frames; ++f) {
        float l = left[f] * boost, r = right[f] * boost;
        float level = envelope(std::max(std::fabs(l), std::fabs(r)));
        loudest = std::max(loudest, level);
        if ((f & 3) == 0) { // the gain computer every fourth sample, the gain ramping between
            float over = 20 * std::log10(std::max(level, 1e-6f)) - threshold;
            float reduce = over > 0 ? over * (1 - 1 / ratio) : 0;
            most = std::max(most, reduce);
            step = (target(reduce) - gain) * 0.25f;
        }
        gain += step;
        l *= gain; r *= gain;
        if (s.color) colour(l, r);
        left[f] = l; right[f] = r;
    }
    shown.store(most, std::memory_order_relaxed);
    note(loudest, most);
}

void MasterCompressor::duck(const float* key_left, const float* key_right, float* left, float* right, unsigned frames) {
    if (!keyed()) return;
    float most = 0, loudest = 0;
    for (unsigned f = 0; f < frames; ++f) {
        float level = envelope(std::max(std::fabs(key_left[f]), std::fabs(key_right[f])) * boost);
        loudest = std::max(loudest, level);
        if ((f & 3) == 0) {
            float over = 20 * std::log10(std::max(level, 1e-6f)) - threshold;
            float reduce = over > 0 ? over * (1 - 1 / ratio) : 0;
            most = std::max(most, reduce);
            step = (target(reduce) - gain) * 0.25f;
        }
        gain += step;
        left[f] *= gain; right[f] *= gain;
    }
    shown.store(most, std::memory_order_relaxed);
    note(loudest, most);
}

} // namespace melodee
