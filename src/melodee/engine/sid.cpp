// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/sid.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace sid {
namespace {
const char* const waves[] {"Triangle", "Saw", "Pulse", "Noise"};
const char* const models[] {"8580", "6581"};
const char* const modes[] {"Low pass", "Band pass", "High pass", "Off"};
constexpr ParamInfo P(const char* label, int min, int max, int initial, Format format = Format::Percent, const char* const* choices = nullptr) {
    return {label, int16_t(min), int16_t(max), int16_t(initial), format, choices};
}
const ParamInfo params[Count] {
    P("WAVE", 0, 3, 2, Format::Choice, waves), P("PULSE W", 5, 95, 30), P("DETUNE", -24, 24, 0, Format::Semitones),
    P("RING", 0, 1, 0, Format::Switch), P("SYNC", 0, 1, 0, Format::Switch), P("MODEL", 0, 1, 0, Format::Choice, models),
    P("PWM", 0, 100, 20), P("PWM RATE", 0, 100, 35, Format::Rate), P("CRUSH", 0, 100, 0),
    P("CUTOFF", 0, 100, 48), P("RESO", 0, 100, 55), P("MODE", 0, 3, 0, Format::Choice, modes), P("ENV > FILT", 0, 100, 40),
    P("KEY TRACK", 0, 100, 50),
    P("ATTACK", 0, 100, 0, Format::Time), P("DECAY", 0, 100, 45, Format::Time), P("SUSTAIN", 0, 100, 60), P("RELEASE", 0, 100, 30, Format::Time),
    P("VELOCITY", 0, 100, 70), P("AT > VIB", 0, 100, expression::default_at_vib), P("AT > BRIGHT", 0, 100, expression::default_at_bright),
    P("LFO RATE", 0, 100, expression::default_vib_rate, Format::Rate),
};
constexpr int8_t _ = -1;
const EditPage pages[] {
    {"Oscillator", "OSC", Picture::Bars, {{{Wave, PulseWidth, Detune}, {Ring, Sync, Model}, {_, _, _}}}},
    {"Pulse and grit", "PWM", Picture::Bars, {{{PwmDepth, PwmRate, Crush}, {_, _, _}, {_, _, _}}}},
    {"Filter", "FILTER", Picture::Bars, {{{Cutoff, Resonance, Mode}, {EnvAmount, KeyTrack, _}, {_, _, _}}}},
    {"Envelope", "ENVELOPE", Picture::Bars, {{{Attack, Decay, Sustain}, {Release, Velocity, _}, {_, _, _}}}},
    {"Touch", "TOUCH", Picture::Bars, {{{Velocity, _, _}, {AtVib, AtBright, LfoRate}, {_, _, _}}}},
};
// The mod matrix on SID: oscillator pitch is the voice's own, Osc 2 the
// partner ring and sync listen to.
const char* const mod_dests[] {"None", "Cutoff", "Resonance", "Amp level", "Pitch", "Osc pitch", "Osc 2 pitch",
                               "Pulse width", "LFO rate", nullptr};
const Sound sounds[] {
    {"INIT VOICE", {{{-1, 0}}}},
    {"RUBBER BASS", {{{Wave, 2}, {PulseWidth, 30}, {Cutoff, 35}, {Resonance, 65}, {EnvAmount, 60}, {Decay, 40}, {Sustain, 30}, {Model, 1}, {-1, 0}}}},
    {"CHIP LEAD", {{{Wave, 2}, {PulseWidth, 50}, {PwmDepth, 45}, {Cutoff, 80}, {Resonance, 20}, {Sustain, 85}, {-1, 0}}}},
    {"ARP PLUCK", {{{Wave, 1}, {Cutoff, 40}, {EnvAmount, 70}, {Decay, 30}, {Sustain, 0}, {Release, 20}, {-1, 0}}}},
    {"PWM PAD", {{{Wave, 2}, {PwmDepth, 70}, {PwmRate, 25}, {Attack, 60}, {Release, 65}, {Sustain, 90}, {Cutoff, 60}, {-1, 0}}}},
    {"SYNC SCREAM", {{{Wave, 1}, {Sync, 1}, {Detune, 12}, {Cutoff, 75}, {Resonance, 40}, {Sustain, 90}, {Model, 1}, {-1, 0}}}},
    {"RING BELL", {{{Wave, 0}, {Ring, 1}, {Detune, 7}, {Mode, 3}, {Decay, 70}, {Sustain, 0}, {Release, 60}, {-1, 0}}}},
    {"NOISE HIT", {{{Wave, 3}, {Mode, 2}, {Cutoff, 60}, {Decay, 25}, {Sustain, 0}, {Release, 15}, {-1, 0}}}},
    {"LO-FI KEYS", {{{Wave, 0}, {Crush, 70}, {Cutoff, 70}, {Decay, 55}, {Sustain, 40}, {-1, 0}}}},
};
// Stage: B1 SOUND (preset, the filter, mix), B2 SHAPE (envelope, pulse and grit, touch), B3 PLAY (arp, oscillator, effects).
using K = StageKind;
using S = StagePicture;
const StageGroups stage {{{
    {{{stage_preset, {"TONE", K::Params, S::Filter, {{Cutoff, Resonance, PulseWidth}}}, stage_mix}}, 3},
    {{{{"ENVELOPE", K::Params, S::Envelope, {{Attack, Decay, Sustain}}, {{Release, Velocity, _}}},
       {"PWM", K::Params, S::Bars, {{PwmDepth, PwmRate, Crush}}},
       {"TOUCH", K::Params, S::Bars, {{AtVib, AtBright, LfoRate}}}}}, 3},
    {{{stage_arp, {"OSC", K::Params, S::ShiftRows, {{Wave, PulseWidth, Detune}}, {{Ring, Sync, Model}}}, stage_fx}}, 3},
}}};
} // namespace

const EngineInfo& info() {
    static const EngineInfo engine {EngineKind::Sid, "SID", true, params, Count, pages, sizeof(pages) / sizeof(pages[0]),
                                    {Cutoff, Resonance, PulseWidth}, sounds, sizeof(sounds) / sizeof(sounds[0]), mod_dests, legacy_count, true, &stage};
    return engine;
}
} // namespace sid

using namespace sid;

SidEngine::SidEngine() { for (unsigned n = 0; n < Count; ++n) p[n] = info().params[n].initial; update(); }
void SidEngine::set_param(int id, int value) {
    if (id < 0 || id >= Count) return;
    p[size_t(id)] = std::clamp<int>(value, info().params[id].min, info().params[id].max);
    dirty = true;
}
void SidEngine::update() {
    dirty = false;
    using dsp::Envelope;
    shape = {Envelope::coefficient(time_seconds(p[Attack])), Envelope::coefficient(time_seconds(p[Decay])),
             Envelope::coefficient(time_seconds(p[Release])), p[Sustain] / 100.0f};
}

void SidEngine::note_on(int key, int velocity) {
    if (dirty) update();
    Voice* v = nullptr;
    for (auto& x : voices) if (x.key == key) { v = &x; break; }
    if (!v) for (auto& x : voices) if (x.key < 0) { v = &x; break; }
    if (!v) { v = &voices[0]; for (auto& x : voices) if (x.age < v->age) v = &x; }
    float s1 = v->s1, s2 = v->s2;
    *v = Voice {};
    v->s1 = s1 * 0.5f; v->s2 = s2 * 0.5f;
    v->key = key; v->held = true; v->age = ++clock; v->velocity = velocity / 127.0f; v->accent = accent;
    v->random = noise.next();
    v->env.gate(true);
}

// Slide or Tie from the sequencer (Engine::slide): the held voice of `from` goes on as `to`.
void SidEngine::slide(int from, int to, int velocity, bool glide) {
    bool found = false;
    for (auto& v : voices)
        if (v.key == from && v.held) {
            if (glide && from != to) v.slide += float(from - to);
            v.key = to; v.age = ++clock; v.accent = accent;
            found = true;
        }
    if (!found) note_on(to, velocity);
}
void SidEngine::set_pressure(int key, float value) { for (auto& v : voices) if (v.key == key && v.held) v.pressure = value; }
void SidEngine::note_off(int key) { for (auto& v : voices) if (v.key == key && v.held) { v.held = false; v.env.gate(false); } }
void SidEngine::all_notes_off() { for (auto& v : voices) if (v.held) { v.held = false; v.env.gate(false); } }
void SidEngine::silence() { for (auto& v : voices) v = Voice {}; }
bool SidEngine::sounding() const { for (auto& v : voices) if (v.key >= 0) return true; return false; }

void SidEngine::render(float* out, unsigned frames) {
    if (dirty) update();
    bool old_chip = p[Model] == 1;
    float crush = p[Crush] / 100.0f, steps = std::exp2(12 - crush * 9); // down to three bits at the top
    for (unsigned start = 0; start < frames; start += 16) {
        unsigned n = std::min(16u, frames - start);
        lfo += rate_hertz(p[PwmRate]) * n / dsp::sample_rate();
        lfo -= std::floor(lfo);
        float sweep = p[PulseWidth] / 100.0f + p[PwmDepth] / 100.0f * 0.45f * std::sin(2 * dsp::pi * lfo);
        lfo_value = vibrato.next(rate_hertz(p[LfoRate]) * std::exp2(expression::lfo_rate_octaves(mods, wheel, lfo_value)), n);
        for (auto& v : voices) {
            if (v.key < 0) continue;
            using expression::at;
            expression::follow(v.touch, v.pressure, n);
            auto m = expression::sums(mods, {lfo_value, v.velocity, v.touch, wheel, (v.key - 60.0f) / 60.0f, v.random, v.env.level});
            // Pitch: a slide's way in, the bend, the matrix and pressure's vibrato.
            v.slide = expression::slide_settle(v.slide, n);
            float key = v.key + v.slide + bend * 2 + at(m, ModDest::Pitch) * 12 + lfo_value * expression::vibrato_semitones(v.touch, p[AtVib]);
            float dt = std::min(dsp::midi_hz(key + at(m, ModDest::OscAPitch) * 12) / dsp::sample_rate(), 0.45f);
            float dt2 = std::min(dsp::midi_hz(key + p[Detune] + at(m, ModDest::OscBPitch) * 12) / dsp::sample_rate(), 0.45f);
            float width = std::clamp(sweep + at(m, ModDest::PulseWidth) * 0.45f, 0.03f, 0.97f);
            // The filter at this block: cutoff, the envelope's push, key tracking, pressure and the matrix, in octaves.
            float octaves = p[Cutoff] / 100.0f * (old_chip ? 8.5f : 9.6f) + v.env.level * p[EnvAmount] / 100.0f * 5 + (key - 60) / 12.0f * p[KeyTrack] / 100.0f
                + v.touch * p[AtBright] / 100.0f * 4 + at(m, ModDest::Cutoff) * 5;
            float g = std::tan(dsp::pi * std::min(30.0f * std::exp2(octaves), dsp::sample_rate() * 0.45f) / dsp::sample_rate());
            float k = 2 - 1.95f * std::clamp(p[Resonance] / 100.0f + at(m, ModDest::Resonance), 0.0f, 1.0f);
            float gain = expression::velocity_gain(v.velocity, p[Velocity] / 100.0f) * std::max(0.0f, 1 + at(m, ModDest::AmpLevel)) * 0.3f * v.accent;
            for (unsigned f = 0; f < n; ++f) {
                v.phase2 += dt2;
                bool wrap2 = v.phase2 >= 1;
                if (wrap2) v.phase2 -= 1;
                v.phase += dt;
                if (v.phase >= 1 || (p[Sync] && wrap2)) v.phase -= std::floor(v.phase);
                // Noise steps sixteen times a cycle, so higher notes hiss brighter.
                if (p[Wave] == 3 && unsigned(v.phase * 16) != unsigned((v.phase - dt) * 16)) v.noise = noise.next();
                float x = 0;
                switch (p[Wave]) {
                case 0: x = 1 - 4 * std::fabs(v.phase - 0.5f); if (p[Ring] && v.phase2 >= 0.5f) x = -x; break; // ring: the second's sign flips it
                case 1: x = 2 * v.phase - 1 - dsp::blep(v.phase, dt); break;
                case 2: { float p2 = v.phase + 1 - width; p2 -= std::floor(p2);
                          x = (v.phase < width ? 1.0f : -1.0f) + dsp::blep(v.phase, dt) - dsp::blep(p2, dt); break; }
                default: x = v.noise; break;
                }
                if (crush > 0) x = std::round(x * steps) / steps;
                if (old_chip) x = dsp::soft_clip(x * 1.6f) * 0.75f;
                float y = x;
                if (p[Mode] < 3) {
                    float hp = (x - (k + g) * v.s1 - v.s2) / (1 + g * (g + k));
                    float bp = g * hp + v.s1, lp = g * bp + v.s2;
                    v.s1 = g * hp + bp; v.s2 = g * bp + lp;
                    y = p[Mode] == 0 ? lp : p[Mode] == 1 ? bp : hp;
                }
                out[start + f] += y * v.env.next(shape) * gain;
            }
            if (!v.env.active()) v.key = -1;
        }
    }
}

} // namespace melodee
