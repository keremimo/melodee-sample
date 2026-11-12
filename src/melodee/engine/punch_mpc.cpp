// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Parity phase 7b: the MPC's sixteen Pad FX as punches (Half Speed, Chorus,
// Flanger, Phaser, Comb, LP / HP / BP with an LFO, Ring Mod, LoFi, Color,
// Granulator, Beat Repeat, Rev Stepper, Delay, Reverb) and Flex Beat's own
// four (Backspin, Trance Gate, Shuffle, Pump), written for sample-base. They
// run inside Punch::process (punch.cpp), one sample at a time, on its
// two-second history; Reverb borrows the history as its lines while it plays.
// Audio thread only, nothing allocated.
#include "melodee/engine/pad_fx_info.h"
#include "melodee/engine/punch.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
constexpr float two_pi = 2 * dsp::pi;
float fs() { return dsp::sample_rate(); } // the device's rate now
using K = PunchKind;
// Where Reverb keeps its parts inside each side's history (floats).
// Room for 96 kHz: Freeverb's 44.1 kHz lengths, stretched to the device's rate.
constexpr unsigned pre_room = 24100, comb_base = pre_room, comb_room = 4400, pass_base = comb_base + 4 * comb_room, pass_room = 1760;
constexpr unsigned reverb_floats = pass_base + 2 * pass_room;
static_assert(reverb_floats < Punch::history, "Reverb fits in the history");

// A state-variable filter step on one side: returns low, band and high pass through the references.
void svf(float x, float g, float k, float& s1, float& s2, float& lp, float& bp, float& hp) {
    hp = (x - (k + g) * s1 - s2) / (1 + g * (g + k));
    bp = g * hp + s1; lp = g * bp + s2;
    s1 = g * hp + bp; s2 = g * bp + lp;
}
float noise(uint32_t& seed) { seed = seed * 1664525u + 1013904223u; return int32_t(seed) * (1.0f / 2147483648.0f); }
} // namespace

void Punch::set_params(const int16_t* values) {
    const auto& info = pad_fx_of(kind);
    for (unsigned n = 0; n < punch_params; ++n) {
        const auto& pi = info.params[n];
        param[n] = !pi.label ? 0 : values ? int16_t(std::clamp<int>(values[n], pi.min, pi.max)) : pi.initial;
    }
}

void Punch::begin_mpc() {
    phase = phase2 = 0; st = {}; ap = {}; grains = {}; next_grain = 0;
    switch (kind) {
    case K::BeatRepeat: loop = bar * pad_fx_divisions[std::clamp<int>(param[0], 0, 7)]; break;
    case K::RevStepper: loop = bar * pad_fx_divisions[std::clamp<int>(param[0], 0, 7)]; break;
    case K::Reverb: {
        std::fill(left.get(), left.get() + reverb_floats, 0.0f);
        std::fill(right.get(), right.get() + reverb_floats, 0.0f);
        static const unsigned combs[] {1116, 1188, 1277, 1356}, passes[] {556, 441};
        float stretch = fs() / dsp::model_rate;
        for (unsigned n = 0; n < 4; ++n) { len[n] = unsigned(combs[n] * stretch); at[n] = 0; }
        for (unsigned n = 0; n < 2; ++n) { len[4 + n] = unsigned(passes[n] * stretch); at[4 + n] = 0; }
        len[6] = unsigned(23 * stretch); len[7] = unsigned(17 * stretch); // the right side's combs and allpasses that much longer
        at[6] = 0; // the predelay's write position
        break;
    }
    default: break;
    }
}

void Punch::mpc(float dry_l, float dry_r, float& wet_l, float& wet_r, double since) {
    auto pct = [&](unsigned n) { return param[n] / 100.0f; };
    auto replay = [&](double back) { wet_l = read(left.get(), back); wet_r = read(right.get(), back); };
    float* io[2] {&wet_l, &wet_r};
    switch (kind) {
    case K::HalfSpeed: { // the tape behind the live sound at 1/1.5, 1/2 or 1/4 speed, mixed
        static const double rates[] {1 / 1.5, 0.5, 0.25};
        lag += 1 - rates[std::clamp<int>(param[0], 0, 2)];
        if (lag > history - 4) lag -= std::floor((history - 4) / loop) * loop;
        replay(lag + 1);
        float mix = pct(1);
        wet_l = dry_l + (wet_l - dry_l) * mix; wet_r = dry_r + (wet_r - dry_r) * mix;
        break;
    }
    case K::Chorus: case K::Flanger: { // a modulated delay on the history, fed back into it
        phase += param[0] / 100.0 / fs(); phase -= std::floor(phase);
        bool chorus = kind == K::Chorus;
        double base = (chorus ? 0.012 : 0.0015) * fs(), depth = (chorus ? 0.006 : 0.004) * fs() * pct(1);
        double back_l = base + depth * (0.5 + 0.5 * std::sin(two_pi * phase));
        double back_r = base + depth * (0.5 + 0.5 * std::sin(two_pi * (phase + 0.25)));
        float ml = read(left.get(), back_l), mr = read(right.get(), back_r);
        float fb = pct(2) * 0.85f;
        left[write] = dry_l + ml * fb; right[write] = dry_r + mr * fb;
        wet_l = (dry_l + ml) * 0.6f; wet_r = (dry_r + mr) * 0.6f;
        break;
    }
    case K::Phaser: { // six allpass stages a side swept by an LFO at SPEED over RANGE, fed back
        phase += 1.0 / (bar * pad_fx_speeds[std::clamp<int>(param[1], 0, 10)]); phase -= std::floor(phase);
        if (unsigned(age) % 16 == 0) {
            float u = 0.5f + 0.5f * std::sin(two_pi * float(phase));
            float hz = 200 * std::exp2(pct(2) * 5.5f * u);
            float t = std::tan(dsp::pi * hz / fs());
            st[2] = (t - 1) / (t + 1);
        }
        float x[2] {dry_l + st[0] * pct(0) * 0.8f, dry_r + st[1] * pct(0) * 0.8f};
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned n = side * 6; n < side * 6 + 6; ++n) { float y = st[2] * x[side] + ap[n]; ap[n] = x[side] - st[2] * y; x[side] = y; }
        st[0] = x[0]; st[1] = x[1];
        wet_l = (dry_l + x[0]) * 0.6f; wet_r = (dry_r + x[1]) * 0.6f;
        break;
    }
    case K::Comb: { // notches: the history added back 1-10 ms late, the delay swept at SPEED
        phase += 1.0 / (bar * pad_fx_speeds[std::clamp<int>(param[0], 0, 10)]); phase -= std::floor(phase);
        double back = (0.001 + 0.009 * (0.5 + 0.5 * std::sin(two_pi * phase))) * fs();
        float cl = read(left.get(), back), cr = read(right.get(), back);
        left[write] = dry_l + cl * 0.6f; right[write] = dry_r + cr * 0.6f;
        wet_l = (dry_l + cl) * 0.55f; wet_r = (dry_r + cr) * 0.55f;
        break;
    }
    case K::LpLfo: case K::HpLfo: case K::BpLfo: { // a resonant filter whose cutoff an LFO at SPEED sweeps over RANGE
        phase += 1.0 / (bar * pad_fx_speeds[std::clamp<int>(param[1], 0, 10)]); phase -= std::floor(phase);
        float u = 0.5f + 0.5f * std::sin(two_pi * float(phase)), octaves = pct(2) * 7;
        float hz = kind == K::LpLfo ? 16000 * std::exp2(-octaves * u) : kind == K::HpLfo ? 40 * std::exp2(octaves * u) : 250 * std::exp2(octaves * 0.8f * u);
        float g = std::tan(dsp::pi * std::clamp(hz, 30.0f, 18000.0f) / fs()), k = 2 - 1.9f * pct(0);
        for (unsigned side = 0; side < 2; ++side) {
            float lp, bp, hp;
            svf(*io[side], g, k, s1[side], s2[side], lp, bp, hp);
            *io[side] = kind == K::LpLfo ? lp : kind == K::HpLfo ? hp : bp * k;
        }
        break;
    }
    case K::RingMod: { // a sine at MAX FREQ, wobbling down to an eighth of it once a bar: slow, it ducks
        phase2 += 1.0 / bar; phase2 -= std::floor(phase2);
        double hz = param[0] * (0.125 + 0.875 * (0.5 + 0.5 * std::cos(two_pi * phase2)));
        phase += hz / fs(); phase -= std::floor(phase);
        float m = float(std::sin(two_pi * phase));
        wet_l *= m; wet_r *= m;
        break;
    }
    case K::LoFi: { // bits down to BITCRUSH, samples held DECIMATOR long
        float steps = std::exp2(float(param[0] - 1));
        unsigned every = 1 + unsigned(pct(1) * 39);
        if (dsp::every_model_frames(unsigned(st[0]++), every, per_frame.ratio)) { hold[0] = std::round(dry_l * steps) / steps; hold[1] = std::round(dry_r * steps) / steps; } // every 44.1 kHz frames
        if (st[0] > 1e6f) st[0] = 0;
        wet_l = hold[0]; wet_r = hold[1];
        break;
    }
    case K::Color: { // vintage gear: cassette, flutter, tube amp, vinyl, saturation, radio
        phase += 1.0 / fs(); phase -= std::floor(phase); // a second's clock for the wobbles
        switch (std::clamp<int>(param[0], 0, 5)) {
        case 0: { // Cassette: wow, a dull top, hiss, a little saturation
            double back = (60 + 30 * std::sin(two_pi * 0.8 * phase)) * per_frame.stretch;
            replay(back);
            float a = 1 - std::exp(-two_pi * 7000 / fs());
            st[0] += (wet_l - st[0]) * a; st[1] += (wet_r - st[1]) * a;
            float hiss = noise(seed) * per_frame.noise * 0.004f;
            wet_l = dsp::soft_clip(st[0] * 1.3f) / 1.3f + hiss; wet_r = dsp::soft_clip(st[1] * 1.3f) / 1.3f + hiss;
            break;
        }
        case 1: { // Flutter: fast and slow pitch wobble
            replay((80 + 25 * std::sin(two_pi * 7 * phase) + 15 * std::sin(two_pi * 0.5 * phase)) * per_frame.stretch);
            break;
        }
        case 2: { // Tube Amp: driven, lopsided, the offset taken out
            for (unsigned side = 0; side < 2; ++side) {
                float y = dsp::soft_clip(*io[side] * 2.5f + 0.2f) - dsp::soft_clip(0.2f);
                st[2 + side] += (y - st[2 + side]) * per_frame.dc;
                *io[side] = (y - st[2 + side]) * 0.6f;
            }
            break;
        }
        case 3: { // Vinyl: the record's band, crackle
            float a = 1 - std::exp(-two_pi * 4500 / fs());
            st[0] += (dry_l - st[0]) * a; st[1] += (dry_r - st[1]) * a;
            st[4] += (st[0] - st[4]) * per_frame.rumble; st[5] += (st[1] - st[5]) * per_frame.rumble; // the rumble cut near 30 Hz
            if ((uint32_t(noise(seed) * 1e6f + 1e6f)) % 3000 == 0 && dsp::kept(seed, per_frame.keep)) st[6] = 0.25f * noise(seed); // 15 a second
            float crackle = st[6]; st[6] *= per_frame.crackle;
            wet_l = (st[0] - st[4]) * 0.9f + crackle; wet_r = (st[1] - st[5]) * 0.9f + crackle;
            break;
        }
        case 4: wet_l = dsp::soft_clip(dry_l * 3) * 0.45f; wet_r = dsp::soft_clip(dry_r * 3) * 0.45f; break; // Saturation
        default: { // Radio: mono, a narrow band, a little broken up
            float x = (dry_l + dry_r) * 0.5f;
            float hp_a = 1 - std::exp(-two_pi * 500 / fs()), lp_a = 1 - std::exp(-two_pi * 3000 / fs());
            st[0] += (x - st[0]) * hp_a;
            st[1] += ((x - st[0]) - st[1]) * lp_a;
            wet_l = wet_r = dsp::soft_clip(st[1] * 2.5f) * 0.7f;
            break;
        }
        }
        break;
    }
    case K::Granulator: { // grains of the last second, DENSITY a second, GRAIN LEN long, fed back
        next_grain -= 1;
        if (next_grain <= 0) {
            next_grain += fs() / std::max(1, int(param[0]));
            for (auto& g : grains)
                if (!g.length) {
                    uint32_t length = uint32_t(param[2] * fs() / 1000);
                    g.length = std::max(length, 64u); g.age = 0;
                    g.back = g.length + (uint32_t(noise(seed) * 0.5f * fs() + 0.5f * fs())); // somewhere in the last second
                    break;
                }
        }
        float out[2] {0, 0};
        unsigned playing = 0;
        for (auto& g : grains) {
            if (!g.length) continue;
            float w = std::sin(dsp::pi * float(g.age) / float(g.length));
            w *= w;
            out[0] += read(left.get(), g.back) * w; out[1] += read(right.get(), g.back) * w;
            ++playing;
            if (++g.age >= g.length) g.length = 0;
        }
        float scale = playing ? 1.0f / std::sqrt(float(playing)) : 0;
        wet_l = out[0] * scale; wet_r = out[1] * scale;
        float fb = pct(1) * 0.7f;
        left[write] = dry_l + wet_l * fb; right[write] = dry_r + wet_r * fb;
        break;
    }
    case K::BeatRepeat: { // the slice of DIVISION again and again, backwards with REVERSE, a resonant filter closing as it goes
        if (!param[1]) forwards(since, wet_l, wet_r); // its turns crossfaded, as Loop's
        else if (frozen) backwards(since, wet_l, wet_r);
        if (!frozen) break;
        double after = since - loop;
        if (param[2] > 0) {
            float closing = float(std::min(1.0, after / bar)); // over a bar
            float g = std::tan(dsp::pi * std::clamp(14000 * std::exp2(-closing * 5), 60.0f, 16000.0f) / fs()), k = 2 - 1.9f * pct(2);
            for (unsigned side = 0; side < 2; ++side) { float lp, bp, hp; svf(*io[side], g, k, s1[side], s2[side], lp, bp, hp); *io[side] = lp; }
        }
        break;
    }
    case K::RevStepper: { // the slice of DELAY TIME played forwards, backwards, ... REPEATS times, then a new slice
        double after = since - loop;
        if (frozen && unsigned(after / loop) >= unsigned(std::max<int>(param[1], 2))) { // let a new slice through
            frozen = false; age = 0; relive = true; since = 0;
            left[write] = dry_l; right[write] = dry_r; // its first frame: the history ran frozen when this one came in
        }
        if (!frozen) { // a slice as it comes: faded out into its first turn, and in after the last turn (no step either way)
            double g = seam >= 1 && since > loop - seam ? (loop - since) / seam : 1.0;
            if (relive) g = std::min(g, (since + 1) / std::max(seam, 1.0));
            fade(wet_l, wet_r, std::min(g, 1.0));
            break;
        }
        unsigned cycle = unsigned(after / loop);
        double in = std::fmod(after, loop);
        replay(cycle % 2 ? in + 1 : loop - in);
        const double turn = 64.0 * per_frame.stretch; // 1.5 ms
        float edge = float(std::min({in, loop - in, turn}) / turn); // no click at the turns
        wet_l *= edge; wet_r *= edge;
        break;
    }
    case K::Delay: { // TIME of a bar, fed back; X-Feedback crosses the sides, Ping-Pong bounces between them
        double back = std::clamp(bar * pad_fx_delay_times[std::clamp<int>(param[0], 0, 16)], 64.0, double(history - 2));
        float dl = read(left.get(), back), dr = read(right.get(), back), fb = pct(1) * 0.9f;
        switch (param[2]) {
        case 1: left[write] = dry_l + dr * fb; right[write] = dry_r + dl * fb; break;
        case 2: left[write] = (dry_l + dry_r) * 0.5f + dr * fb; right[write] = dl * fb; break;
        default: left[write] = dry_l + dl * fb; right[write] = dry_r + dr * fb; break;
        }
        wet_l = dry_l + dl * 0.7f; wet_r = dry_r + dr * 0.7f;
        break;
    }
    case K::Reverb: { // predelay, four combs and two allpasses a side, in the history (not written meanwhile)
        unsigned pre = std::min(pre_room - 1, unsigned(param[0] * fs() / 1000));
        float feedback = 0.7f + pct(1) * 0.27f, diffuse = 0.3f + pct(2) * 0.4f;
        float* lines[2] {left.get(), right.get()};
        float in[2] {dry_l, dry_r};
        unsigned w = at[6];
        for (unsigned side = 0; side < 2; ++side) {
            float* line = lines[side];
            line[w] = in[side];
            float x = line[(w + pre_room - pre) % pre_room] * 0.03f, out = 0;
            for (unsigned n = 0; n < 4; ++n) {
                float* comb = line + comb_base + n * comb_room;
                unsigned length = len[n] + side * len[6], i = at[n] % length;
                float y = comb[i];
                st[side * 4 + n] = y * per_frame.damp_in + st[side * 4 + n] * per_frame.damp; // damping
                comb[i] = x + st[side * 4 + n] * feedback;
                out += y;
            }
            for (unsigned n = 0; n < 2; ++n) {
                float* pass = line + pass_base + n * pass_room;
                unsigned length = len[4 + n] + side * len[7], i = at[4 + n] % length;
                float b = pass[i], y = -out * diffuse + b;
                pass[i] = out + b * diffuse;
                out = y;
            }
            *io[side] = in[side] + out * 1.5f;
        }
        for (unsigned n = 0; n < 6; ++n) ++at[n];
        if (++at[6] >= pre_room) at[6] = 0;
        break;
    }
    // Flex Beat's own.
    case K::Backspin: { // the record spun back fast, slowing to a stop over a beat, then quiet
        double t = since / (bar / 4);
        double rate = std::max(0.0, 3 * (1 - t));
        lag = std::min(lag + 1 + rate, double(history - 4));
        replay(lag + 1);
        float g = float(std::min(1.0, rate));
        wet_l *= g; wet_r *= g;
        break;
    }
    case K::TranceGate: { // a sixteen-step pattern a bar, the edges smoothed
        static const uint16_t pattern = 0b1011011010110110;
        unsigned step = unsigned(std::fmod(since, bar) / (bar / 16)) % 16;
        float target = (pattern >> step) & 1 ? 1.0f : 0.0f;
        st[0] += (target - st[0]) * per_frame.gate;
        wet_l *= st[0]; wet_r *= st[0];
        break;
    }
    case K::Shuffle: { // half a bar let through, then its eight sixteenths in another order, each faded at its edges
        double piece = loop / 8, edge = std::max(1.0, std::min(seam, piece / 4));
        if (!frozen) { if (since > loop - edge) fade(wet_l, wet_r, (loop - since) / edge); break; }
        static const unsigned order[8] {0, 2, 1, 3, 6, 4, 7, 5};
        double after = since - loop, in = std::fmod(after, loop);
        unsigned n = unsigned(in / piece) % 8;
        double within = std::fmod(in, piece), from = order[n] * piece + within;
        replay(loop - from);
        fade(wet_l, wet_r, std::min(1.0, std::min(within + 1, piece - within) / edge));
        break;
    }
    case K::Pump: { // ducked on every beat, back up over it
        double beat = bar / 4, t = std::fmod(since, beat) / beat;
        float g = float(1 - 0.85 * std::exp(-t * 6) * std::min(1.0, t * 200));
        wet_l *= g; wet_r *= g;
        break;
    }
    default: break;
    }
}

} // namespace melodee
