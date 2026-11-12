// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The effects parity phase 7 added from the MPC's Knob FX list, written for
// sample-base: Diff delay, Sample delay, Spring, Pumper, Amp sim, Tube drive,
// Soft clipper, Ensemble, Multi-chorus, Auto-wah, Vintage, Vinyl and Tape
// emulator. Same rules as effects.cpp: the audio thread only, delay memory
// from the pool, nothing allocated.
#include "melodee/engine/effects.h"
#include "melodee/engine/rbj.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using K = EffectKind;
constexpr float two_pi = 2 * dsp::pi;
float fs() { return dsp::sample_rate(); } // the device's rate now
float db(float decibels) { return std::pow(10.0f, decibels / 20.0f); }
float follow(float seconds) { return std::exp(-1.0f / std::max(seconds * fs(), 1.0f)); }
float one_pole(float hz) { return 1 - std::exp(-two_pi * std::min(hz, fs() * 0.45f) / fs()); }

// A ring of `length` floats at `line`: what was written `back` samples before `at` (fractional).
float tap(const float* line, unsigned length, unsigned at, float back) {
    float position = float(at) + float(length) - std::clamp(back, 1.0f, float(length - 2));
    unsigned i = unsigned(position);
    float frac = position - float(i);
    float x0 = line[i % length], x1 = line[(i + 1) % length];
    return x0 + (x1 - x0) * frac;
}

// Where each effect keeps its lines inside its pool block (floats).
// Room for 96 kHz; lengths written in 44.1 kHz samples are stretched to the device's rate.
float stretch() { return fs() / dsp::model_rate; }
constexpr unsigned diff_line = 96000, diff_room = 1320;         // Diff delay: up to a second a side, then 4 diffusers a side
constexpr unsigned short_line = 24100;                          // Sample delay, Spring's predelay: 250 ms a side
constexpr unsigned spring_base = 2 * short_line, spring_room = 8800;
constexpr unsigned wobble_line = 9000;                          // Ensemble, Multi-chorus (one line), Vinyl, Tape emulator (a side)
static_assert(2 * diff_line + 8 * diff_room <= DelayPool::block_floats, "Diff delay fits in its block");
static_assert(spring_base + 2 * spring_room <= DelayPool::block_floats, "Spring fits in its block");

// Pumper's speeds in quarter notes (effect_info.cpp's pumper_speeds).
constexpr double pumper_quarters[] {4, 2, 4.0 / 3, 1, 2.0 / 3, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125, 1.0 / 12};

// Amp sim's cabinets: high pass, low pass (and its Q), a presence peak.
struct Cab { float hp, lp, lp_q, peak_hz, peak_db; };
constexpr Cab cabs[] {
    {20, 20000, 0.7f, 1000, 0},     // D.I.
    {90, 5000, 0.9f, 2000, 4},      // Brit
    {180, 3500, 1.0f, 1500, 5},     // 1x8"
    {100, 4500, 0.9f, 1800, 3},     // 1x12"
    {110, 5000, 0.8f, 2200, 3},     // 2x10"
    {85, 4800, 0.9f, 1600, 2},      // 2x12"
    {100, 5500, 0.8f, 2500, 2},     // 4x10"
    {75, 4200, 1.0f, 1200, 3},      // 4x12"
    {45, 2500, 0.8f, 500, 3},       // 1x15" Bass
    {55, 3500, 0.8f, 800, 2},       // 4x10" Bass
    {350, 3000, 1.2f, 1200, 6},     // Radio
};

// Vintage: the machines' rate, bits and output filter (the SP1200's Ring: its filter ringing).
struct Machine { float rate, bits, cutoff, reso, drive; };
constexpr Machine machines[] {
    {44100, 16, 18000, 0.0f, 1.1f},  // MPC3000
    {40000, 12, 14000, 0.1f, 1.3f},  // MPC60
    {26040, 12, 9000, 0.15f, 1.3f},  // SP1200
    {26040, 12, 12000, 0.75f, 1.3f}, // SP1200 Ring
};

uint32_t next(uint32_t& seed) { seed = seed * 1664525u + 1013904223u; return seed; }
float noise(uint32_t& seed) { return int32_t(next(seed)) * (1.0f / 2147483648.0f); }
// A first-order allpass's coefficient written for 44.1 kHz, at the device's rate: its corner kept (bilinear).
float allpass_at_rate(float d) {
    const float r = dsp::model_frames();
    if (r == 1.0f) return d;
    const float t = std::tan(std::atan((1 - d) / (1 + d)) * r);
    return (1 - t) / (1 + t);
}
} // namespace

void Effect::update_extra() {
    auto pct = [&](unsigned n) { return p[n] / 100.0f; };
    auto set = [](Biquad& q, const float* c) { q.b0 = c[0]; q.b1 = c[1]; q.b2 = c[2]; q.a1 = c[3]; q.a2 = c[4]; };
    mix = 1;
    switch (current) {
    case K::DiffDelay: {
        glide[1] = dsp::step_per_frame(1 - std::exp(-1.0f / (0.05f * float(dsp::model_rate)))); // SYNC's glide (roadmap P4)
        delay = std::clamp(unsigned(time_seconds(p[0]) * fs()), 2u, diff_line - 2);
        a = pct(1); mix = pct(2);
        b = pct(3) * 0.7f;                             // the diffusers' coefficient
        c = one_pole(18000 * std::exp2(-pct(4) * 5));  // high damping: a low pass in the loop
        static const unsigned lengths[] {142, 107, 379, 277};
        for (unsigned n = 0; n < 4; ++n) {
            comb_length[n] = std::max(1u, unsigned(lengths[n] * stretch()));
            comb_length[n + 4] = std::min(diff_room - 1, unsigned(lengths[n] * 1.13f * stretch()));
        }
        for (unsigned n = 0; n < 8; ++n) comb_at[n] %= comb_length[n];
        break;
    }
    case K::SampleDelay:
        a = std::max(1.0f, p[0] * fs() / 1000); b = std::max(1.0f, p[1] * fs() / 1000); mix = pct(2);
        break;
    case K::Spring: {
        predelay = std::min(short_line - 2, unsigned(p[0] * fs() / 1000));
        float seconds = p[1] / 10.0f;
        comb_length[0] = unsigned(1557 * stretch()); comb_length[1] = unsigned(1789 * stretch()); // the springs' round trips, 35 and 41 ms
        comb_at[0] %= comb_length[0]; comb_at[1] %= comb_length[1];
        a = std::pow(10.0f, -3.0f * comb_length[0] / (seconds * fs()));
        b = std::pow(10.0f, -3.0f * comb_length[1] / (seconds * fs()));
        mix = pct(2);
        c = pct(3);                                    // width
        d = allpass_at_rate(0.45f + pct(4) * 0.35f);   // dispersion: the chirp's allpass coefficient
        e = std::exp(-two_pi * float(p[5]) / fs());      // low cut, a one-pole high pass
        damp = one_pole(4500);
        break;
    }
    case K::Pumper: mix = 1; glide[0] = dsp::step_per_frame(0.05f); break; // the gain's way to its aim
    case K::AmpSim: {
        float coefficients[5];
        for (unsigned side = 0; side < 2; ++side) {
            rbj::shelf(coefficients, 120, float(p[3]), false); set(eq[side * 6], coefficients);
            rbj::peak(coefficients, 700, float(p[4]), 0.7f); set(eq[side * 6 + 1], coefficients);
            rbj::shelf(coefficients, 3000, float(p[5]), true); set(eq[side * 6 + 2], coefficients);
            const Cab& cab = cabs[std::clamp(p[0], 0, 10)];
            rbj::pass(coefficients, cab.hp, 0.7f, true); set(eq[side * 6 + 3], coefficients);
            rbj::pass(coefficients, cab.lp, cab.lp_q, false); set(eq[side * 6 + 4], coefficients);
            rbj::peak(coefficients, cab.peak_hz, cab.peak_db, 1.0f); set(eq[side * 6 + 5], coefficients);
        }
        a = db(p[1] / 110.0f * 36);                    // DRIVE 0-11: up to 36 dB into the valves
        b = 1 / (1 + p[1] / 110.0f);                   // and back down after
        c = pct(2);                                    // soft against hard clipping
        break;
    }
    case K::TubeDrive:
        a = db(pct(0) * 30); b = db(float(p[1])); c = pct(2) * 0.4f; // drive, headroom, the bias that adds even harmonics
        glide[0] = dsp::step_per_frame(0.0015f);                     // the DC blocker, near 10 Hz
        break;
    case K::SoftClip: a = db(float(p[0])); b = db(float(p[3])); mix = pct(2); break;
    case K::Ensemble:
        a = rate_hertz(p[0]); b = p[1] * fs() / 1000; mix = pct(2);
        c = (p[3] + 0.5f) * fs() / 1000; d = pct(4); e = pct(5);
        break;
    case K::MultiChorus:
        a = rate_hertz(p[0]); b = p[1] * fs() / 1000; mix = pct(2);
        c = (p[4] + 0.5f) * fs() / 1000; delay = p[3] == 0 ? 3 : p[3] == 1 ? 4 : 6;
        if (voice_phase[1] == 0) for (unsigned n = 0; n < voice_phase.size(); ++n) voice_phase[n] = n / 6.0f; // spread round the turn
        break;
    case K::AutoWah:
        mix = pct(2);
        d = follow(0.001f + pct(4) * 0.05f); e = follow(0.02f + pct(5) * 0.5f);
        svf_k = 2 - 1.85f * pct(1);
        break;
    case K::Vintage: {
        const Machine& m = machines[std::clamp(p[0], 0, 3)];
        a = m.rate / fs(); b = std::exp2(m.bits - 1); d = m.drive;
        svf_g = std::tan(dsp::pi * std::min(m.cutoff, fs() * 0.45f) / fs());
        svf_k = 2 - 1.9f * m.reso;
        mix = pct(2);
        break;
    }
    case K::Vinyl:
        a = one_pole(2500 * std::exp2(pct(0) * 2.7f)); // tone: 2.5 to 16 kHz
        b = pct(1); c = pct(2);
        fall[0] = dsp::fall_per_frame(0.93f);                                      // a crackle's fall, about 0.3 ms
        glide = {dsp::step_per_frame(0.3f), dsp::step_per_frame(0.004f)};           // the hiss's band; the rumble cut near 30 Hz
        break;
    case K::TapeEmu:
        a = pct(0); b = pct(1); c = pct(2); d = one_pole(12000);
        glide = {dsp::step_per_frame(0.0004f), dsp::step_per_frame(0.5f)};          // the pitch's wander toward its aim; the hiss's band
        break;
    default: break;
    }
}

void Effect::process_extra(float* left, float* right, unsigned frames) {
    float* line_l = memory;
    const EffectClock& clock = *effect_clock_now; // this thread's host's (a print host's own)
    for (unsigned f = 0; f < frames; ++f) {
        float l = left[f], r = right[f], wet_l = l, wet_r = r;
        switch (current) {
        case K::DiffDelay: {
            // Each echo goes through four diffusers before it is heard and fed back, so the repeats smear like echoes in a room.
            float out[2];
            float* lines[2] {memory, memory + diff_line};
            float back = sync_back(float(diff_line - 2)); // SYNC: a note value (roadmap P4); else TIME
            bool on_clock = synced > 0;
            for (unsigned side = 0; side < 2; ++side) {
                // On the clock the echo lands on the note: the four diffusers' own delay is taken off the line's.
                float own = on_clock ? float(comb_length[side * 4] + comb_length[side * 4 + 1] + comb_length[side * 4 + 2] + comb_length[side * 4 + 3]) : 0.0f;
                float x = tap(lines[side], diff_line, write, std::max(2.0f, back - own));
                for (unsigned n = side * 4; n < side * 4 + 4; ++n) {
                    float* ap = memory + 2 * diff_line + n * diff_room;
                    float buffered = ap[comb_at[n]];
                    float y = -b * x + buffered;
                    ap[comb_at[n]] = x + b * y;
                    if (++comb_at[n] >= comb_length[n]) comb_at[n] = 0;
                    x = y;
                }
                float& tone = side ? tone_r : tone_l;
                tone += (x - tone) * c;
                out[side] = tone;
            }
            lines[0][write] = l + out[0] * a;
            lines[1][write] = r + out[1] * a;
            if (++write >= diff_line) write = 0;
            wet_l = out[0]; wet_r = out[1];
            break;
        }
        case K::SampleDelay: {
            line_l[write] = l; line_l[short_line + write] = r;
            wet_l = p[0] ? tap(line_l, short_line, write + 1, a + 1) : l;
            wet_r = p[1] ? tap(line_l + short_line, short_line, write + 1, b + 1) : r;
            if (++write >= short_line) write = 0;
            break;
        }
        case K::Spring: {
            // Predelay, a low cut, then a loop a side: the dispersion chain makes the drip and chirp, a low pass darkens each trip.
            float* pre = memory;
            pre[write] = l; pre[short_line + write] = r;
            unsigned back = (write + short_line - predelay) % short_line;
            float in[2] {pre[back], pre[short_line + back]};
            float out[2];
            for (unsigned side = 0; side < 2; ++side) {
                float& low = state[side];
                low = in[side] + (low - in[side]) * e;
                float x = (in[side] - low) * 0.5f;
                float* loop = memory + spring_base + side * spring_room;
                unsigned& at = comb_at[side];
                float y = loop[at];
                float& dark = side ? tone_r : tone_l;
                dark += (y - dark) * damp;
                float v = x + dark * (side ? b : a);
                for (unsigned n = side * 12; n < side * 12 + 12; ++n) { float o = -d * v + chirp[n]; chirp[n] = v + d * o; v = o; }
                loop[at] = v;
                if (++at >= comb_length[side]) at = 0;
                out[side] = y;
            }
            if (++write >= short_line) write = 0;
            float spread = 0.5f + 0.5f * c;
            wet_l = out[0] * spread + out[1] * (1 - spread);
            wet_r = out[1] * spread + out[0] * (1 - spread);
            break;
        }
        case K::Pumper: {
            // The gain dips on each beat of SPEED: down over ATTACK, held for HOLD, back up over RELEASE, curved by SHAPE.
            double length = pumper_quarters[std::clamp(p[0], 0, 10)];
            double beats = clock.beats + double(f) / clock.quarter;
            float t = float(beats / length - std::floor(beats / length));
            float A = 0.005f + p[3] / 100.0f * 0.2f, H = p[4] / 100.0f * 0.3f, R = std::max(0.02f, p[5] / 100.0f * (1 - A - H));
            float duck = 0;
            if (t < A) duck = t / A;
            else if (t < A + H) duck = 1;
            else if (t < A + H + R) duck = std::pow(1 - (t - A - H) / R, 1 + p[1] / 100.0f * 4);
            float target = 1 - p[2] / 100.0f * duck;
            gain += (target - gain) * glide[0];
            wet_l = l * gain; wet_r = r * gain;
            break;
        }
        case K::AmpSim: {
            float io[2] {l, r};
            for (unsigned side = 0; side < 2; ++side) {
                float x = io[side] * a;
                x = eq[side * 6 + 2].run(eq[side * 6 + 1].run(eq[side * 6].run(x))); // the tone stack, into the valves
                float hard = std::clamp(x, -1.0f, 1.0f), soft = dsp::soft_clip(x);
                x = hard + (soft - hard) * c;
                x = eq[side * 6 + 5].run(eq[side * 6 + 4].run(eq[side * 6 + 3].run(x))); // the cabinet
                io[side] = x * b;
            }
            wet_l = io[0]; wet_r = io[1];
            break;
        }
        case K::TubeDrive: {
            // Clipping starts at HEADROOM; the bias makes it lopsided (even harmonics), a DC blocker takes the offset out.
            float io[2] {l, r};
            for (unsigned side = 0; side < 2; ++side) {
                float u = io[side] * a / b;
                float y = (dsp::soft_clip(u + c) - dsp::soft_clip(c)) * b / std::sqrt(a);
                float& dc = state[side];
                dc += (y - dc) * glide[0];
                io[side] = y - dc;
            }
            wet_l = io[0]; wet_r = io[1];
            break;
        }
        case K::SoftClip: {
            auto shape = [&](float x) {
                x *= a;
                switch (p[1]) {
                case 1: x = std::clamp(x, -1.0f, 1.0f); return std::sin(x * dsp::pi / 2);
                case 2: x = std::clamp(x, -1.0f, 1.0f); return x * (2 - std::fabs(x));
                default: return std::tanh(x);
                }
            };
            wet_l = shape(l) * b; wet_r = shape(r) * b;
            break;
        }
        case K::Ensemble: {
            // Three voices on one line, a third of a turn apart, each with a faster shimmer on top; spread left, centre, right.
            lfo += a / fs(); lfo -= std::floor(lfo);
            env2 += a * 6.3f / fs(); env2 -= std::floor(env2);
            line_l[write] = (l + r) * 0.5f;
            float v[3];
            for (unsigned n = 0; n < 3; ++n) {
                float offset = n / 3.0f;
                float back = c + b * (0.5f + 0.5f * std::sin(two_pi * (lfo + offset))) + d * 0.0006f * fs() * (1 + std::sin(two_pi * (env2 + offset)));
                v[n] = tap(line_l, wobble_line, write, back);
            }
            if (++write >= wobble_line) write = 0;
            float far = 0.8f * (1 - e);
            wet_l = (v[0] * 0.8f + v[1] * 0.5f + v[2] * far) * 0.75f;
            wet_r = (v[2] * 0.8f + v[1] * 0.5f + v[0] * far) * 0.75f;
            break;
        }
        case K::MultiChorus: {
            line_l[write] = (l + r) * 0.5f;
            float out[2] {0, 0};
            for (unsigned n = 0; n < delay; ++n) {
                float& phase = voice_phase[n];
                phase += a * (1 + 0.11f * n) / fs(); phase -= std::floor(phase);
                float x = tap(line_l, wobble_line, write, c + b * (0.5f + 0.5f * std::sin(two_pi * phase)));
                out[n % 2] += x * 0.85f; out[1 - n % 2] += x * 0.35f;
            }
            if (++write >= wobble_line) write = 0;
            float scale = 2.0f / float(delay);
            wet_l = out[0] * scale; wet_r = out[1] * scale;
            break;
        }
        case K::AutoWah: {
            // A resonant low pass whose cutoff follows the input's level.
            float level = std::max(std::fabs(l), std::fabs(r));
            env = level > env ? level + (env - level) * d : level + (env - level) * e;
            if ((f & 7) == 0) {
                float open = std::clamp(env * 4, 0.0f, 1.0f) * p[0] / 100.0f;
                float hz = std::clamp(150 * std::exp2(p[3] / 100.0f * 4 + open * 5), 40.0f, 16000.0f);
                svf_g = std::tan(dsp::pi * hz / fs());
            }
            float* io[2] {&wet_l, &wet_r};
            for (unsigned side = 0; side < 2; ++side) {
                auto& s = svf[side];
                float x = *io[side];
                float hp = (x - (svf_k + svf_g) * s.s1 - s.s2) / (1 + svf_g * (svf_g + svf_k));
                float bp = svf_g * hp + s.s1, lp = svf_g * bp + s.s2;
                s.s1 = svf_g * hp + bp; s.s2 = svf_g * bp + lp;
                *io[side] = lp;
            }
            break;
        }
        case K::Vintage: {
            // Sample and hold at the machine's rate (no filter before it: the aliasing is the sound), its bits, its output filter.
            env += a;
            if (env >= 1) { env -= 1; hold_l = std::round(l * b) / b; hold_r = std::round(r * b) / b; }
            float* io[2] {&wet_l, &wet_r};
            float held[2] {hold_l, hold_r};
            for (unsigned side = 0; side < 2; ++side) {
                auto& s = svf[side];
                float x = held[side];
                float hp = (x - (svf_k + svf_g) * s.s1 - s.s2) / (1 + svf_g * (svf_g + svf_k));
                float bp = svf_g * hp + s.s1, lp = svf_g * bp + s.s2;
                s.s1 = svf_g * hp + bp; s.s2 = svf_g * bp + lp;
                *io[side] = dsp::soft_clip(lp * d) / d;
            }
            break;
        }
        case K::Vinyl: {
            // A slow wow at 33 rpm, the band of a record, crackle and surface noise.
            lfo += 0.55f / fs(); lfo -= std::floor(lfo);
            line_l[write] = l; line_l[wobble_line + write] = r;
            float back = 0.004f * fs() + c * 0.0025f * fs() * std::sin(two_pi * lfo);
            float x[2] {tap(line_l, wobble_line, write, back), tap(line_l + wobble_line, wobble_line, write, back)};
            if (++write >= wobble_line) write = 0;
            tone_l += (x[0] - tone_l) * a; tone_r += (x[1] - tone_r) * a;
            float& pop = state[0];
            float& pop_level = state[1];
            float& hiss = state[2];
            // As many crackles a second, each as long and as loud, and the hiss as loud, at any rate (update()).
            if ((next(seed) >> 8) % 1000000 < unsigned(b * b * 900) && dsp::kept(seed, keep)) { pop = 1; pop_level = (0.1f + float((seed >> 4) % 100) / 220.0f) * b; }
            float crackle = noise(seed) * noise_gain * pop * pop_level; // the noise scaled where it comes in (exact at 44.1 kHz)
            pop *= fall[0];
            hiss += (noise(seed) * noise_gain - hiss) * glide[0];
            float surface = crackle + hiss * b * 0.006f;
            float& rumble_l = state[3];
            float& rumble_r = state[4];
            rumble_l += (tone_l - rumble_l) * glide[1]; rumble_r += (tone_r - rumble_r) * glide[1]; // a high pass near 30 Hz
            wet_l = tone_l - rumble_l + surface; wet_r = tone_r - rumble_r + surface;
            break;
        }
        case K::TapeEmu: {
            // Wow (slow), a wandering pitch, hiss, and the tape's soft saturation and dull top.
            lfo += 0.7f / fs(); lfo -= std::floor(lfo);
            float& drift = state[0];
            float& aim = state[1];
            float& hiss = state[2];
            if (dsp::every_model_frames(hold_count++, 4096, ratio)) aim = noise(seed); // a new aim every 93 ms
            drift += (aim - drift) * glide[0];
            line_l[write] = l; line_l[wobble_line + write] = r;
            float back = 0.008f * fs() + a * 0.003f * fs() * std::sin(two_pi * lfo) + c * 0.004f * fs() * drift;
            float x[2] {tap(line_l, wobble_line, write, back), tap(line_l + wobble_line, wobble_line, write, back)};
            if (++write >= wobble_line) write = 0;
            hiss += (noise(seed) * noise_gain - hiss) * glide[1];
            for (unsigned side = 0; side < 2; ++side) x[side] = dsp::soft_clip(x[side] * 1.4f) / 1.4f + hiss * b * 0.01f;
            tone_l += (x[0] - tone_l) * d; tone_r += (x[1] - tone_r) * d;
            wet_l = tone_l; wet_r = tone_r;
            break;
        }
        default: break;
        }
        float m = blend(); // the mix, under Bypass's fade
        left[f] = l + (wet_l - l) * m;
        right[f] = r + (wet_r - r) * m;
    }
}

} // namespace melodee
