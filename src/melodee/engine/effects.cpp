// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/effects.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace melodee {
namespace {
using K = EffectKind;
constexpr float fs = dsp::sample_rate, two_pi = 2 * dsp::pi;
float db(float decibels) { return std::pow(10.0f, decibels / 20.0f); }
float follow(float seconds) { return std::exp(-1.0f / std::max(seconds * fs, 1.0f)); }
bool needs_memory(K k) {
    return k == K::Room || k == K::Reverb || k == K::Plate || k == K::Echo || k == K::TapeEcho || k == K::PingPong ||
           k == K::Chorus || k == K::Flanger || k == K::Tape || k == K::Shimmer || k == K::Looper || k == K::PedalChorus;
}
// Where each part of a reverb sits inside its block (floats).
constexpr unsigned pre_length = 4410, comb_base = 2 * pre_length, comb_room = 1600, pass_base = comb_base + 8 * comb_room, pass_room = 700;
// Shimmer's shifter after the reverb's parts: a short line a side.
constexpr unsigned shift_base = pass_base + 4 * pass_room, shift_room = 4096;
static_assert(shift_base + 2 * shift_room < DelayPool::block_floats, "shimmer fits in its block");
// Formants of five vowels (A E I O U), Hz.
constexpr float vowel_formants[5][3] {{800, 1150, 2900}, {400, 1700, 2600}, {280, 2200, 2950}, {450, 800, 2830}, {325, 700, 2530}};

// RBJ cookbook filters.
void shelf(float* out, float hz, float gain_db, bool high) {
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
void peak(float* out, float hz, float gain_db, float q) {
    float A = std::pow(10.0f, gain_db / 40.0f), w = two_pi * hz / fs, alpha = std::sin(w) / (2 * q), cw = std::cos(w);
    float a0 = 1 + alpha / A;
    out[0] = (1 + alpha * A) / a0; out[1] = -2 * cw / a0; out[2] = (1 - alpha * A) / a0; out[3] = -2 * cw / a0; out[4] = (1 - alpha / A) / a0;
}
} // namespace

DelayPool::DelayPool() : memory(new float[size_t(blocks) * block_floats]()) {}
float* DelayPool::take() {
    for (unsigned n = 0; n < blocks; ++n)
        if (!used[n]) { used[n] = true; float* block = memory.get() + size_t(n) * block_floats; std::fill(block, block + block_floats, 0.0f); return block; }
    return nullptr;
}
void DelayPool::give(float* block) {
    if (!block) return;
    used[size_t((block - memory.get()) / block_floats)] = false;
}

void Effect::set_kind(EffectKind kind, DelayPool& pool) {
    if (memory && !needs_memory(kind)) { pool.give(memory); memory = nullptr; }
    if (!memory && needs_memory(kind)) memory = pool.take(); // clearing a block takes a moment, once per change
    else if (memory) std::fill(memory, memory + DelayPool::block_floats, 0.0f);
    current = kind;
    const auto& info = effect_info(kind);
    for (unsigned n = 0; n < effect_params; ++n) p[n] = info.params[n].label ? info.params[n].initial : 0;
    write = 0; lfo = 0; env = env2 = 0; gain = 1; hold_l = hold_r = tone_l = tone_r = 0; hold_count = 0;
    svf = {}; allpass = {}; phaser_l = phaser_r = 0; comb_filter = {}; comb_at = {}; pass_at = {};
    shift_at = spit_left = 0; shift_phase = shimmer_l = shimmer_r = 0; spit_gain = 1; formant = {};
    for (auto& q : eq) q.z1 = q.z2 = 0;
    update();
}

void Effect::set_param(unsigned index, int value) {
    if (index >= effect_params) return;
    const auto& info = effect_info(current).params[index];
    if (!info.label) return;
    p[index] = std::clamp<int>(value, info.min, info.max);
    update();
}

// Turns the parameters into what process() uses: a..e and the filters.
void Effect::update() {
    auto pct = [&](unsigned n) { return p[n] / 100.0f; };
    mix = 1;
    switch (current) {
    case K::Room: case K::Reverb: case K::Plate: case K::Shimmer: {
        float scale = (current == K::Plate ? 0.55f : current == K::Room ? 0.6f : 0.8f) + pct(0) * 0.4f;
        static const unsigned combs[] {1116, 1188, 1277, 1356}, passes[] {556, 441};
        float spread = 1 + pct(5) * 0.025f;
        for (unsigned n = 0; n < 4; ++n) {
            comb_length[n] = std::min(comb_room - 1, unsigned(combs[n] * scale));
            comb_length[n + 4] = std::min(comb_room - 1, unsigned(combs[n] * scale * spread));
        }
        for (unsigned n = 0; n < 2; ++n) { pass_length[n] = unsigned(passes[n] * scale); pass_length[n + 2] = unsigned(passes[n] * scale * spread); }
        for (unsigned n = 0; n < 8; ++n) comb_at[n] %= std::max(comb_length[n], 1u);
        for (unsigned n = 0; n < 4; ++n) pass_at[n] %= std::max(pass_length[n], 1u);
        // Decay time to comb feedback: -60 dB after `seconds` through the average comb.
        float seconds = std::clamp(time_seconds(p[1]), 0.2f, 12.0f);
        feedback = std::pow(10.0f, -3.0f * (1250 * scale) / (seconds * fs));
        damp = pct(2) * (current == K::Plate ? 0.3f : 0.6f);
        mix = pct(3);
        predelay = std::min(pre_length - 1, unsigned(pct(4) * 0.1f * fs));
        if (current == K::Shimmer) { predelay = 0; d = pct(4) * 0.5f; } // how much of the octave goes round again
        break;
    }
    case K::Looper:
        delay = std::clamp(unsigned(time_seconds(p[0]) * fs), 64u, DelayPool::half - 1);
        write %= delay;
        a = pct(2); mix = pct(3);
        break;
    case K::PedalChorus:
        a = rate_hertz(p[0]) * 0.6f; b = pct(1);
        c = 1 - std::exp(-two_pi * (1500 + pct(2) * 7000) / fs); // the dark wet of a bucket brigade
        mix = pct(3);
        break;
    case K::PedalPhaser: a = rate_hertz(p[0]) * 0.5f; b = pct(1); c = p[2] ? 0.6f : 0; mix = pct(3); break;
    case K::Spit: a = pct(0); b = pct(1); mix = pct(2); break;
    case K::Formant: {
        float at = pct(0) * 4, shift = std::exp2(p[2] / 12.0f);
        unsigned v = std::min(3u, unsigned(at));
        float frac = at - float(v);
        for (unsigned n = 0; n < 3; ++n) {
            float hz = (vowel_formants[v][n] + (vowel_formants[v + 1][n] - vowel_formants[v][n]) * frac) * shift;
            formant_g[n] = std::tan(dsp::pi * std::min(hz, fs * 0.45f) / fs);
        }
        formant_k = 1.2f - pct(1) * 1.1f; // narrower bands with more RESO
        mix = pct(3);
        break;
    }
    case K::Echo: case K::TapeEcho: case K::PingPong:
        delay = std::min(DelayPool::half - 64, unsigned(time_seconds(p[0]) * fs));
        a = pct(1);                                                   // feedback
        mix = pct(2);
        b = 1 - std::exp(-two_pi * (800 + pct(current == K::TapeEcho ? 4 : 3) * 9000) / fs); // tone: lowpass in the loop
        c = current == K::TapeEcho ? pct(3) : 0;                       // wow
        break;
    case K::Chorus: case K::Flanger:
        a = rate_hertz(p[0]); b = pct(1);
        c = current == K::Flanger ? p[2] / 100.0f : 0;
        mix = pct(current == K::Flanger ? 3 : 2);
        break;
    case K::Phaser: a = rate_hertz(p[0]); b = pct(1); c = pct(2); mix = pct(3); break;
    case K::AutoPan: case K::Tremolo: a = rate_hertz(p[0]); b = pct(1); c = float(p[2]); break;
    case K::Drive: case K::Fuzz:
        a = db(pct(0) * (current == K::Fuzz ? 40 : 24));
        b = 1 - std::exp(-two_pi * (1000 + pct(1) * 15000) / fs);
        mix = pct(2);
        break;
    case K::Tape: a = db(pct(0) * 18); c = pct(1); mix = pct(2); break;
    case K::Crush: a = std::exp2(float(p[0] - 1)); delay = 1 + unsigned(pct(1) * 31); mix = pct(2); break;
    case K::Eq: {
        float coefficients[5];
        float mid = 200 * std::pow(25.0f, pct(3));
        for (unsigned side = 0; side < 2; ++side) {
            shelf(coefficients, 120, float(p[0]), false);
            eq[side * 3] = {coefficients[0], coefficients[1], coefficients[2], coefficients[3], coefficients[4], eq[side * 3].z1, eq[side * 3].z2};
            peak(coefficients, mid, float(p[1]), 0.9f);
            eq[side * 3 + 1] = {coefficients[0], coefficients[1], coefficients[2], coefficients[3], coefficients[4], eq[side * 3 + 1].z1, eq[side * 3 + 1].z2};
            shelf(coefficients, 6000, float(p[2]), true);
            eq[side * 3 + 2] = {coefficients[0], coefficients[1], coefficients[2], coefficients[3], coefficients[4], eq[side * 3 + 2].z1, eq[side * 3 + 2].z2};
        }
        break;
    }
    case K::Filter: {
        float hz = 20.0f * std::exp2(pct(0) * 9.97f);
        svf_g = std::tan(dsp::pi * std::min(hz, fs * 0.45f) / fs);
        svf_k = 2 - 1.9f * pct(1);
        break;
    }
    case K::Width: a = p[0] / 100.0f; break;
    case K::Compressor:
        a = float(p[0]); b = float(std::max(p[1], 1)); c = db(float(p[2]));
        d = follow(time_seconds(p[3]) * 0.1f); e = follow(time_seconds(p[4]));
        mix = pct(5);
        break;
    case K::Gate: a = db(float(p[0])); e = follow(time_seconds(p[1])); break;
    case K::Transient: a = p[0] / 100.0f; b = p[1] / 100.0f; break;
    case K::Limiter: a = db(float(p[0])); e = follow(time_seconds(p[1]) * 0.5f); break;
    default: break;
    }
}

float Effect::delayed(const float* line, unsigned at, float back) const {
    float position = float(at) + DelayPool::half - back;
    unsigned i = unsigned(position);
    float frac = position - float(i);
    float x0 = line[i % DelayPool::half], x1 = line[(i + 1) % DelayPool::half];
    return x0 + (x1 - x0) * frac;
}

void Effect::reverb(float& l, float& r) {
    float* pre = memory;
    pre[write % pre_length] = l; pre[pre_length + write % pre_length] = r;
    unsigned back = (write + pre_length - predelay) % pre_length;
    float in[2] {pre[back] * 0.03f, pre[pre_length + back] * 0.03f};
    float out[2] {0, 0};
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned n = side * 4; n < side * 4 + 4; ++n) {
            float* line = memory + comb_base + n * comb_room;
            float y = line[comb_at[n]];
            comb_filter[n] = y * (1 - damp) + comb_filter[n] * damp;
            line[comb_at[n]] = in[side] + comb_filter[n] * feedback;
            if (++comb_at[n] >= comb_length[n]) comb_at[n] = 0;
            out[side] += y;
        }
        for (unsigned n = side * 2; n < side * 2 + 2; ++n) {
            float* line = memory + pass_base + n * pass_room;
            float buffered = line[pass_at[n]];
            float y = -out[side] + buffered;
            line[pass_at[n]] = out[side] + buffered * 0.5f;
            if (++pass_at[n] >= pass_length[n]) pass_at[n] = 0;
            out[side] = y;
        }
    }
    l = out[0]; r = out[1];
}

void Effect::process(float* left, float* right, unsigned frames) {
    if (bypass || current == K::None || !effect_info(current).built) return;
    if (needs_memory(current) && !memory) return; // the pool ran out: sound passes through
    float* line_l = memory;
    float* line_r = memory ? memory + DelayPool::half : nullptr;
    for (unsigned f = 0; f < frames; ++f) {
        float l = left[f], r = right[f], wet_l = l, wet_r = r;
        switch (current) {
        case K::Room: case K::Reverb: case K::Plate:
            reverb(wet_l, wet_r);
            ++write;
            break;
        case K::Shimmer: {
            // The reverb hears the input and its own sound an octave up.
            wet_l = l + shimmer_l * d; wet_r = r + shimmer_r * d;
            reverb(wet_l, wet_r);
            ++write;
            // Octave up: two taps read twice as fast as the line is written,
            // each faded in and out over the window, half a window apart.
            float* sl = memory + shift_base;
            float* sr = sl + shift_room;
            sl[shift_at] = wet_l; sr[shift_at] = wet_r;
            constexpr float window = 2048;
            shift_phase += 1; if (shift_phase >= window) shift_phase -= window;
            float out[2] {0, 0};
            for (float offset : {0.0f, window / 2}) {
                float ph = std::fmod(shift_phase + offset, window), back = window - ph + 1;
                float g = std::sin(dsp::pi * ph / window);
                float at = float(shift_at) + shift_room - back;
                unsigned i = unsigned(at);
                float fr = at - float(i);
                const float* lines[2] {sl, sr};
                for (unsigned side = 0; side < 2; ++side) {
                    float x0 = lines[side][i % shift_room], x1 = lines[side][(i + 1) % shift_room];
                    out[side] += (x0 + (x1 - x0) * fr) * g * g; // sin² windows half a window apart sum to one
                }
            }
            if (++shift_at >= shift_room) shift_at = 0;
            shimmer_l = out[0]; shimmer_r = out[1];
            break;
        }
        case K::Looper: {
            // The loop plays under the input; RECORD on adds the input to it, KEEP fading what was there.
            float loop_l = line_l[write], loop_r = line_r[write];
            wet_l = l + loop_l; wet_r = r + loop_r;
            if (p[1]) { line_l[write] = dsp::soft_clip(loop_l * a + l); line_r[write] = dsp::soft_clip(loop_r * a + r); } // layers never run away
            if (++write >= delay) write = 0;
            break;
        }
        case K::PedalChorus: {
            lfo += a / fs; lfo -= std::floor(lfo);
            float tri = 1 - 4 * std::fabs(lfo - 0.5f); // -1..1
            line_l[write] = (l + r) * 0.5f;            // one line, read twice for width
            float base = 0.009f * fs, depth = 0.0045f * fs * b;
            float w1 = dsp::soft_clip(delayed(line_l, write, base + depth * tri) * 1.3f) / 1.3f;
            float w2 = dsp::soft_clip(delayed(line_l, write, base - depth * tri) * 1.3f) / 1.3f;
            tone_l += (w1 - tone_l) * c; tone_r += (w2 - tone_r) * c;
            wet_l = (l + tone_l) * 0.7f; wet_r = (r + tone_r) * 0.7f;
            if (++write >= DelayPool::half) write = 0;
            break;
        }
        case K::PedalPhaser: {
            lfo += a / fs; lfo -= std::floor(lfo);
            if ((f & 15) == 0) {
                float tri = 1 - 2 * std::fabs(2 * lfo - 1); // 0..1
                float hz = 160 * std::exp2(b * 3.8f * tri);
                float t = std::tan(dsp::pi * hz / fs);
                env2 = (t - 1) / (t + 1);
            }
            float x[2] {l + phaser_l * c, r + phaser_r * c};
            for (unsigned side = 0; side < 2; ++side)
                for (unsigned n = 0; n < 4; ++n) { auto& s = allpass[side][n]; float y = env2 * x[side] + s; s = x[side] - env2 * y; x[side] = y; }
            phaser_l = x[0]; phaser_r = x[1];
            wet_l = (l + x[0]) * 0.5f; wet_r = (r + x[1]) * 0.5f;
            break;
        }
        case K::Spit: {
            // Now and then a spit: a few milliseconds held, crushed and jumping in level.
            seed = seed * 1664525u + 1013904223u;
            if (!spit_left && (seed >> 8) % 100000 < unsigned(a * a * 120)) {
                spit_left = 90 + (seed >> 4) % 900;
                spit_gain = 0.4f + float((seed >> 12) % 100) / 60.0f;
            }
            float steps = std::exp2(10 - b * 7);
            if (spit_left) {
                --spit_left;
                unsigned every = 2 + unsigned(b * 14);
                if (hold_count++ % every == 0) { hold_l = std::round(l * steps) / steps; hold_r = std::round(r * steps) / steps; }
                wet_l = dsp::soft_clip(hold_l * spit_gain * 2) * 0.6f; wet_r = dsp::soft_clip(hold_r * spit_gain * 2) * 0.6f;
            } else {
                wet_l = dsp::soft_clip(l * (1 + b)) / (1 + b * 0.5f); wet_r = dsp::soft_clip(r * (1 + b)) / (1 + b * 0.5f);
            }
            break;
        }
        case K::Formant: {
            static constexpr float gains[3] {1.0f, 0.6f, 0.3f};
            float x[2] {l, r}, y[2] {0, 0};
            for (unsigned side = 0; side < 2; ++side)
                for (unsigned n = 0; n < 3; ++n) {
                    auto& s = formant[side * 3 + n];
                    float g = formant_g[n];
                    float hp = (x[side] - (formant_k + g) * s.s1 - s.s2) / (1 + g * (g + formant_k));
                    float bp = g * hp + s.s1, lp = g * bp + s.s2;
                    s.s1 = g * hp + bp; s.s2 = g * bp + lp;
                    y[side] += bp * formant_k * gains[n] * 1.6f; // band pass at unit peak, then weighted
                }
            wet_l = y[0]; wet_r = y[1];
            break;
        }
        case K::Echo: case K::TapeEcho: case K::PingPong: {
            float back = float(delay);
            if (current == K::TapeEcho) { lfo += 0.6f / fs; lfo -= std::floor(lfo); back *= 1 + c * 0.004f * std::sin(two_pi * lfo); }
            float dl = delayed(line_l, write, back), dr = delayed(line_r, write, back);
            tone_l += (dl - tone_l) * b; tone_r += (dr - tone_r) * b;
            float fl = tone_l * a, fr = tone_r * a;
            if (current == K::TapeEcho) { fl = dsp::soft_clip(fl * 1.2f); fr = dsp::soft_clip(fr * 1.2f); }
            if (current == K::PingPong) { line_l[write] = (l + r) * 0.5f + fr; line_r[write] = fl; }
            else { line_l[write] = l + fl; line_r[write] = r + fr; }
            wet_l = dl; wet_r = dr;
            if (++write >= DelayPool::half) write = 0;
            break;
        }
        case K::Chorus: case K::Flanger: {
            lfo += a / fs; lfo -= std::floor(lfo);
            float base = current == K::Chorus ? 0.015f * fs : 0.003f * fs, depth = current == K::Chorus ? 0.007f * fs : 0.0025f * fs;
            float ml = base + depth * b * std::sin(two_pi * lfo), mr = base + depth * b * std::sin(two_pi * (lfo + 0.25f));
            wet_l = delayed(line_l, write, ml); wet_r = delayed(line_r, write, mr);
            line_l[write] = l + wet_l * c; line_r[write] = r + wet_r * c;
            if (++write >= DelayPool::half) write = 0;
            break;
        }
        case K::Phaser: {
            lfo += a / fs; lfo -= std::floor(lfo);
            if ((f & 15) == 0) {
                float hz = 300 * std::exp2(b * 3.5f * (0.5f + 0.5f * std::sin(two_pi * lfo)));
                float t = std::tan(dsp::pi * hz / fs);
                env2 = (t - 1) / (t + 1); // one allpass coefficient for all stages
            }
            float x[2] {l + phaser_l * c, r + phaser_r * c};
            for (unsigned side = 0; side < 2; ++side)
                for (auto& s : allpass[side]) { float y = env2 * x[side] + s; s = x[side] - env2 * y; x[side] = y; }
            phaser_l = x[0]; phaser_r = x[1];
            wet_l = (l + x[0]) * 0.5f; wet_r = (r + x[1]) * 0.5f;
            break;
        }
        case K::AutoPan: {
            lfo += a / fs; lfo -= std::floor(lfo);
            float s = std::sin(two_pi * lfo) * b;
            wet_l = l * std::min(1.0f, 1 - s); wet_r = r * std::min(1.0f, 1 + s);
            break;
        }
        case K::Tremolo: {
            lfo += a / fs; lfo -= std::floor(lfo);
            float wave = c ? (lfo < 0.5f ? 1.0f : -1.0f) : std::sin(two_pi * lfo);
            gain += ((1 - b * (0.5f - 0.5f * wave)) - gain) * 0.01f; // smoothed, so the square does not click
            wet_l = l * gain; wet_r = r * gain;
            break;
        }
        case K::Drive:
            tone_l += (dsp::soft_clip(l * a) - tone_l) * b; tone_r += (dsp::soft_clip(r * a) - tone_r) * b;
            wet_l = tone_l / std::sqrt(a) * 1.5f; wet_r = tone_r / std::sqrt(a) * 1.5f;
            break;
        case K::Fuzz: {
            auto fuzz = [&](float x) { x *= a; return x > 0 ? std::min(x, 0.6f) : std::max(x, -0.9f); };
            tone_l += (fuzz(l) - tone_l) * b; tone_r += (fuzz(r) - tone_r) * b;
            wet_l = tone_l; wet_r = tone_r;
            break;
        }
        case K::Tape: {
            lfo += 0.8f / fs; lfo -= std::floor(lfo);
            float back = 0.005f * fs * (1 + c * 0.6f * std::sin(two_pi * lfo));
            line_l[write] = l; line_r[write] = r;
            wet_l = dsp::soft_clip(delayed(line_l, write, back) * a) / std::sqrt(a);
            wet_r = dsp::soft_clip(delayed(line_r, write, back) * a) / std::sqrt(a);
            if (++write >= DelayPool::half) write = 0;
            break;
        }
        case K::Crush:
            if (hold_count++ % delay == 0) { hold_l = std::round(l * a) / a; hold_r = std::round(r * a) / a; }
            wet_l = hold_l; wet_r = hold_r;
            break;
        case K::Eq:
            wet_l = eq[2].run(eq[1].run(eq[0].run(l)));
            wet_r = eq[5].run(eq[4].run(eq[3].run(r)));
            break;
        case K::Filter: {
            float* io[2] {&wet_l, &wet_r};
            for (unsigned side = 0; side < 2; ++side) {
                auto& s = svf[side];
                float x = *io[side];
                float hp = (x - (svf_k + svf_g) * s.s1 - s.s2) / (1 + svf_g * (svf_g + svf_k));
                float bp = svf_g * hp + s.s1, lp = svf_g * bp + s.s2;
                s.s1 = svf_g * hp + bp; s.s2 = svf_g * bp + lp;
                *io[side] = p[2] == 0 ? lp : p[2] == 1 ? bp : hp;
            }
            break;
        }
        case K::Width: {
            float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * a;
            wet_l = mid + side; wet_r = mid - side;
            break;
        }
        case K::Compressor: {
            float level = std::max(std::fabs(l), std::fabs(r));
            env = level > env ? level + (env - level) * d : level + (env - level) * e;
            float over = 20 * std::log10(std::max(env, 1e-6f)) - a;
            float reduce = over > 0 ? over * (1 - 1 / b) : 0;
            float g = db(-reduce) * c;
            wet_l = l * g; wet_r = r * g;
            break;
        }
        case K::Gate: {
            float level = std::max(std::fabs(l), std::fabs(r));
            env = level > env ? level : level + (env - level) * 0.999f;
            float target = env > a ? 1.0f : 0.0f;
            gain = target > gain ? gain + (target - gain) * 0.05f : target + (gain - target) * e;
            wet_l = l * gain; wet_r = r * gain;
            break;
        }
        case K::Transient: {
            float level = std::max(std::fabs(l), std::fabs(r));
            env = level > env ? level + (env - level) * 0.98f : level + (env - level) * 0.9995f;   // fast
            env2 = level > env2 ? level + (env2 - level) * 0.9995f : level + (env2 - level) * 0.9995f; // slow
            float attack = (env - env2) / (env2 + 1e-3f), sustain = (env2 - env) / (env + 1e-3f);
            float g = std::clamp(1 + a * std::max(attack, 0.0f) + b * std::max(sustain, 0.0f), 0.0f, 4.0f);
            wet_l = l * g; wet_r = r * g;
            break;
        }
        case K::Limiter: {
            float level = std::max(std::fabs(l), std::fabs(r));
            float target = level > a ? a / level : 1.0f;
            gain = target < gain ? target : target + (gain - target) * e;
            wet_l = l * gain; wet_r = r * gain;
            break;
        }
        default: break;
        }
        left[f] = l + (wet_l - l) * mix;
        right[f] = r + (wet_r - r) * mix;
    }
}

} // namespace melodee
