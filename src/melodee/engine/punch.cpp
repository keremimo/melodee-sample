// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/punch.h"
#include <algorithm>
#include <cmath>

namespace melodee {

const char* punch_name(PunchKind kind) {
    static const char* names[] {"LOOP 4", "LOOP 8", "LOOP 16", "LOOP 32", "STUTTER", "REVERSE", "STOP", "HALF", "LOW", "HIGH",
                                "PHONE", "CRUSH", "ALIAS", "GATE", "ECHO", "WOBBLE",
                                "SWEEP DN", "SWEEP UP", "RING", "FLANGE", "DRIVE", "PAN", "TREMOLO", "DOWNER", "UPPER", "SCRATCH", "VINYL",
                                "HALF SPEED", "CHORUS", "FLANGER", "PHASER", "COMB", "LP FILTER", "HP FILTER", "BP FILTER", "RING MOD",
                                "LOFI", "COLOR", "GRANULATOR", "BEAT REPEAT", "REV STEPPER", "DELAY", "REVERB",
                                "BACKSPIN", "TRANCE GATE", "SHUFFLE", "PUMP"};
    return names[unsigned(kind) % punch_count];
}
const char* punch_title(PunchKind kind) {
    static const char* names[] {"Loop 4", "Loop 8", "Loop 16", "Loop 32", "Stutter", "Reverse", "Stop", "Half", "Low", "High",
                                "Phone", "Crush", "Alias", "Gate", "Echo", "Wobble",
                                "Sweep down", "Sweep up", "Ring", "Flange", "Drive", "Pan", "Tremolo", "Downer", "Upper", "Scratch", "Vinyl",
                                "Half Speed", "Chorus", "Flanger", "Phaser", "Comb Filter", "LP Filter", "HP Filter", "BP Filter", "Ring Mod",
                                "LoFi", "Color", "Granulator", "Beat Repeat", "Rev Stepper", "Delay", "Reverb",
                                "Backspin", "Trance Gate", "Shuffle", "Pump"};
    return names[unsigned(kind) % punch_count];
}

void Punch::clear() {
    std::fill(left.get(), left.get() + history, 0.0f);
    std::fill(right.get(), right.get() + history, 0.0f);
    held = frozen = false; fading = 0;
}

void Punch::start(PunchKind k, float d, double bar_samples, double grid_samples, unsigned tail_mode, const int16_t* values) {
    kind = k; depth = d; bar = std::max(256.0, bar_samples); grid = std::clamp(grid_samples, 64.0, bar);
    set_params(values);
    held = true; fading = 0; age = begun = 0; lag = 0; speed = 1;
    s1[0] = s1[1] = s2[0] = s2[1] = 0;
    switch (k) {
    case PunchKind::Loop4: loop = bar / 4; break;
    case PunchKind::Loop8: loop = bar / 8; break;
    case PunchKind::Loop16: loop = bar / 16; break;
    case PunchKind::Loop32: case PunchKind::Stutter: loop = bar / 32; break;
    case PunchKind::Reverse: case PunchKind::Shuffle: loop = bar / 2; break;
    default: loop = grid; break;
    }
    if (punch_is_mpc(k) || k >= PunchKind::Backspin) begin_mpc(); // may set its own loop
    loop = std::clamp(loop, 64.0, double(history - 2));
    // A repeat's turn crossfades into what came a loop before the slice's start: the history must still hold it.
    seam = std::min(seam_frames(loop), double(history - 2) - loop);
    ease_in = std::max(1.0, seam_seconds * dsp::sample_rate());
    frozen = false; // a repeat first lets one slice through, then freezes the history on it
    relive = false;
    fade_step = tail_mode == 2 ? 1.0f / (1.5f * dsp::sample_rate()) : 1.0f / (0.06f * dsp::sample_rate());
    if (tail_mode == 0) fade_step = float(1.0 / ease_in); // cut: gone within seam_seconds, not at the next sample (a click)
}

void Punch::stop() {
    if (!held) return;
    held = false;
    fading = 1;
}

void Punch::forwards(double since, float& wet_l, float& wet_r) const {
    double in = frozen ? std::fmod(since - loop, loop) : since, back = frozen ? loop - in : 0;
    if (frozen) { wet_l = read(left.get(), back); wet_r = read(right.get(), back); }
    if (seam < 1 || in < loop - seam) return;
    float g = float(std::min(1.0, (in - (loop - seam) + 1) / seam)); // all the way by the turn's last frame
    float pre_l = read(left.get(), back + loop), pre_r = read(right.get(), back + loop);
    wet_l += (pre_l - wet_l) * g; wet_r += (pre_r - wet_r) * g;
}

void Punch::backwards(double since, float& wet_l, float& wet_r) const {
    double after = since - loop, in = std::fmod(after, loop);
    wet_l = read(left.get(), in + 1); wet_r = read(right.get(), in + 1);
    if (seam >= 1) fade(wet_l, wet_r, std::min(1.0, std::min(after >= loop ? in + 1 : loop, loop - in) / seam));
}

float Punch::read(const float* line, double back) const {
    double at = double(write) + history - std::clamp(back, 1.0, double(history - 2));
    unsigned i = unsigned(at);
    float frac = float(at - i);
    unsigned a = i % history, b = (i + 1) % history;
    return line[a] + (line[b] - line[a]) * frac;
}

void Punch::process(float* l, float* r, unsigned frames) {
    constexpr float two_pi = 2 * dsp::pi;
    for (unsigned f = 0; f < frames; ++f) {
        float dry_l = l[f], dry_r = r[f];
        // Held, it comes in over seam_seconds (a filter or a crusher switched in at once would step); let go, it fades.
        float amount = held ? depth * float(std::min(1.0, double(begun + 1) / ease_in)) : fading * depth;
        if (amount <= 0 && frozen) frozen = false; // the release is over: the history runs again
        if (amount > 0 && repeats() && !frozen && double(age) >= loop) frozen = true; // the slice is in
        bool lines = own_lines() && amount > 0; // Reverb: the history is its lines meanwhile
        if (!frozen && !lines) { left[write] = dry_l; right[write] = dry_r; }
        if (amount > 0) {
            float wet_l = dry_l, wet_r = dry_r;
            double since = double(age), after = since - loop; // for repeats: time since the slice was taken
            auto replay = [&](double back) { wet_l = read(left.get(), back); wet_r = read(right.get(), back); };
            switch (kind) {
            // Repeats: the first slice plays as it comes; then the history is
            // frozen with `write` just past it, and the slice plays again. Each
            // time round, the slice's last `seam` frames crossfade into what
            // played a loop earlier, just before the slice began, so the jump back
            // to its start continues the sound instead of clicking.
            case PunchKind::Loop4: case PunchKind::Loop8: case PunchKind::Loop16: case PunchKind::Loop32:
                forwards(since, wet_l, wet_r);
                break;
            case PunchKind::Stutter: { // the slice, a gap as long, the slice again: each edge faded
                if (!frozen) { if (seam >= 1 && since > loop - seam) fade(wet_l, wet_r, (loop - since) / seam); break; }
                double in = std::fmod(after, loop * 2);
                if (in < loop) { wet_l = wet_r = 0; break; }
                double k = in - loop;
                replay(loop - k);
                if (seam >= 1) fade(wet_l, wet_r, std::min(1.0, std::min(k + 1, loop - k) / seam));
                break;
            }
            case PunchKind::Reverse: if (frozen) backwards(since, wet_l, wet_r); break; // the half bar, then backwards through it
            // Tape: the history runs, the read falls behind it by `lag`.
            case PunchKind::Stop: { // slows to a stop over four grid steps, then silence
                speed = std::max(0.0, 1 - since / (grid * 4));
                lag += 1 - speed;
                replay(lag + 1);
                float g = float(std::min(1.0, speed * 4));
                wet_l *= g; wet_r *= g;
                break;
            }
            case PunchKind::Half: case PunchKind::Downer: {
                double rate = kind == PunchKind::Half ? 0.5 : 0.5 + 0.5 * std::exp(-since / (grid * 2)); // Downer glides down an octave
                lag += 1 - rate;
                if (lag > history - 4) lag -= std::floor((history - 4) / loop) * loop; // too far behind: catch up a whole grid at a time
                replay(lag + 1);
                break;
            }
            case PunchKind::Upper: { // an octave up: two taps read twice as fast from a window behind, half a window
                double window = std::min(grid, 4096.0); // apart, each faded in and out (squared sines, summing to one)
                float sum_l = 0, sum_r = 0;
                for (double offset : {0.0, window / 2}) {
                    double ph = std::fmod(since + offset, window);
                    float w = float(std::sin(dsp::pi * ph / window));
                    w *= w;
                    sum_l += read(left.get(), window - ph + 1) * w; sum_r += read(right.get(), window - ph + 1) * w;
                }
                wet_l = sum_l; wet_r = sum_r;
                break;
            }
            case PunchKind::Scratch: { // back and forth over the last grid step
                lag = std::max(0.0, lag + 1 - (1 + 1.6 * std::sin(two_pi * since / grid)));
                lag = std::min(lag, double(history - 4));
                replay(lag + 1);
                break;
            }
            // Filters (a state-variable filter, its cutoff from the kind).
            case PunchKind::Low: case PunchKind::High: case PunchKind::Phone: case PunchKind::Wobble:
            case PunchKind::SweepDown: case PunchKind::SweepUp: {
                float hz = kind == PunchKind::Low ? 400 : kind == PunchKind::High ? 2500 : kind == PunchKind::Phone ? 1600
                    : kind == PunchKind::Wobble ? 300 * std::exp2(3 * (0.5f + 0.5f * std::sin(two_pi * float(since / grid))))
                    : kind == PunchKind::SweepDown ? 12000 * std::exp2(-float(since / bar) * 6) : 120 * std::exp2(float(since / bar) * 6);
                float g = std::tan(dsp::pi * std::clamp(hz, 30.0f, 18000.0f) / dsp::sample_rate()), k = kind == PunchKind::Phone ? 1.2f : 0.7f;
                float* io[2] {&wet_l, &wet_r};
                for (unsigned side = 0; side < 2; ++side) {
                    float x = *io[side];
                    float hp = (x - (k + g) * s1[side] - s2[side]) / (1 + g * (g + k));
                    float bp = g * hp + s1[side], lp = g * bp + s2[side];
                    s1[side] = g * hp + bp; s2[side] = g * bp + lp;
                    *io[side] = kind == PunchKind::High || kind == PunchKind::SweepUp ? hp : kind == PunchKind::Phone ? bp * 2 : lp;
                }
                break;
            }
            case PunchKind::Crush:
                wet_l = std::round(dry_l * 8) / 8; wet_r = std::round(dry_r * 8) / 8;
                break;
            case PunchKind::Alias: // held every twelfth sample: 3.7 kHz
                if (age % 12 == 0) { hold[0] = dry_l; hold[1] = dry_r; }
                wet_l = hold[0]; wet_r = hold[1];
                break;
            case PunchKind::Gate: { // open for the first half of each grid step, each edge faded (the first opening is the start's)
                double pos = std::fmod(since, grid), open = grid / 2, edge = std::max(1.0, std::min(ease_in, open / 4));
                double g = pos < open ? std::min(1.0, (open - pos) / edge) : 0.0;
                if (pos < open && since >= grid) g = std::min(g, (pos + 1) / edge);
                fade(wet_l, wet_r, g);
                break;
            }
            case PunchKind::Tremolo: { float a = 0.5f + 0.5f * std::cos(two_pi * float(since / grid)); wet_l *= a; wet_r *= a; break; }
            case PunchKind::Pan: { float a = std::sin(two_pi * float(since / (grid * 2))); wet_l *= 1 - std::max(0.0f, a); wet_r *= 1 + std::min(0.0f, a); break; }
            case PunchKind::Echo: { // two grid steps, fed back through the history
                double delay = std::min(grid * 2, double(history - 2));
                wet_l = dry_l + read(left.get(), delay) * 0.55f; wet_r = dry_r + read(right.get(), delay) * 0.55f;
                left[write] = wet_l; right[write] = wet_r;
                break;
            }
            case PunchKind::Ring: { float m = std::sin(two_pi * float(since) * 440 / dsp::sample_rate()); wet_l *= m; wet_r *= m; break; }
            case PunchKind::Flange: {
                double back = 44 + 120 * (0.5 + 0.5 * std::sin(two_pi * since / (bar / 2)));
                wet_l = (dry_l + read(left.get(), back)) * 0.6f; wet_r = (dry_r + read(right.get(), back)) * 0.6f;
                break;
            }
            case PunchKind::Drive: wet_l = dsp::soft_clip(dry_l * 4) * 0.5f; wet_r = dsp::soft_clip(dry_r * 4) * 0.5f; break;
            case PunchKind::Vinyl: {
                uint32_t h = uint32_t(age) * 2654435761u;
                float crackle = (h >> 24) % 97 == 0 ? 0.15f * ((h >> 8 & 1) ? 1 : -1) : 0;
                float g = std::tan(dsp::pi * 3500 / dsp::sample_rate()) / (1 + std::tan(dsp::pi * 3500 / dsp::sample_rate()));
                s1[0] += g * (dry_l - s1[0]); s1[1] += g * (dry_r - s1[1]);
                wet_l = s1[0] * 0.85f + crackle; wet_r = s1[1] * 0.85f + crackle;
                break;
            }
            default: mpc(dry_l, dry_r, wet_l, wet_r, since); break; // the MPC's Pad FX and Flex Beat's own (punch_mpc.cpp)
            }
            l[f] = dry_l + (wet_l - dry_l) * amount;
            r[f] = dry_r + (wet_r - dry_r) * amount;
            ++age; ++begun;
            if (!held) fading = std::max(0.0f, fading - fade_step);
        }
        if (!frozen && !lines && ++write >= history) write = 0;
    }
}

} // namespace melodee
