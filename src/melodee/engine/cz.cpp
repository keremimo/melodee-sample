// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/cz.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace cz {
namespace {
const char* const waves[] {"Saw", "Square", "Pulse", "Double sine", "Saw-pulse", "Reso 1", "Reso 2", "Reso 3"};
const char* const second_waves[] {"None", "Saw", "Square", "Pulse", "Double sine", "Saw-pulse", "Reso 1", "Reso 2", "Reso 3"};
const char* const lines[] {"1", "2", "1+1'", "1+2'"};
constexpr ParamInfo P(const char* label, int min, int max, int initial, Format format = Format::Percent, const char* const* choices = nullptr) {
    return {label, int16_t(min), int16_t(max), int16_t(initial), format, choices};
}
const ParamInfo params[Count] {
    P("WAVE 1", 0, 7, 0, Format::Choice, waves), P("WAVE 2", 0, 8, 0, Format::Choice, second_waves), P("DCW", 0, 100, 40),
    P("LINE", 0, 3, 0, Format::Choice, lines), P("DETUNE", -50, 50, 8, Format::Cents), P("OCTAVE", -2, 2, 0, Format::Number),
    P("DCW ENV", 0, 100, 50), P("ATTACK", 0, 100, 0, Format::Time), P("DECAY", 0, 100, 50, Format::Time), P("SUSTAIN", 0, 100, 30),
    P("RELEASE", 0, 100, 35, Format::Time), P("KEY FOLLOW", 0, 100, 30),
    P("ATTACK", 0, 100, 0, Format::Time), P("DECAY", 0, 100, 60, Format::Time), P("SUSTAIN", 0, 100, 75),
    P("RELEASE", 0, 100, 40, Format::Time), P("VELOCITY", 0, 100, 50),
};
constexpr int8_t _ = -1;
const EditPage pages[] {
    {"Waves", "WAVES", Picture::Bars, {{{Wave1, Wave2, Dcw}, {_, _, _}, {_, _, _}}}},
    {"Lines", "LINES", Picture::Bars, {{{Line, Detune, Octave}, {_, _, _}, {_, _, _}}}},
    {"DCW envelope", "DCW ENV", Picture::Bars, {{{DcwAmount, DcwAttack, DcwDecay}, {DcwSustain, DcwRelease, KeyFollow}, {_, _, _}}}},
    {"DCA envelope", "DCA ENV", Picture::Bars, {{{DcaAttack, DcaDecay, DcaSustain}, {DcaRelease, Velocity, _}, {_, _, _}}}},
};
const Sound sounds[] {
    {"INIT VOICE", {{{-1, 0}}}},
    {"GLASS PAD", {{{Wave1, 5}, {Dcw, 30}, {Line, 2}, {Detune, 12}, {DcwAmount, 40}, {DcwAttack, 65}, {DcaAttack, 60}, {DcaSustain, 90}, {DcaRelease, 70}, {-1, 0}}}},
    {"CZ BASS", {{{Wave1, 0}, {Dcw, 20}, {DcwAmount, 75}, {DcwDecay, 40}, {DcwSustain, 10}, {DcaDecay, 50}, {DcaSustain, 60}, {Octave, -1}, {-1, 0}}}},
    {"RESO SWEEP", {{{Wave1, 6}, {Dcw, 10}, {DcwAmount, 90}, {DcwAttack, 60}, {DcwDecay, 75}, {DcwSustain, 50}, {DcaSustain, 90}, {-1, 0}}}},
    {"BRASS", {{{Wave1, 1}, {Wave2, 1}, {Dcw, 25}, {DcwAmount, 60}, {DcwAttack, 35}, {DcwSustain, 60}, {DcaAttack, 20}, {DcaSustain, 85}, {-1, 0}}}},
    {"ELECTRIC PIANO", {{{Wave1, 3}, {Dcw, 15}, {DcwAmount, 55}, {DcwDecay, 55}, {DcwSustain, 0}, {DcaDecay, 75}, {DcaSustain, 20}, {Velocity, 80}, {-1, 0}}}},
    {"SAW STRINGS", {{{Wave1, 0}, {Line, 2}, {Detune, 18}, {Dcw, 55}, {DcwAmount, 10}, {DcaAttack, 55}, {DcaSustain, 95}, {DcaRelease, 65}, {-1, 0}}}},
    {"PULSE ORGAN", {{{Wave1, 2}, {Wave2, 4}, {Dcw, 60}, {DcwAmount, 0}, {DcaSustain, 100}, {DcaRelease, 25}, {-1, 0}}}},
};

float knee(float phase, float m) { return phase < m ? phase * 0.5f / m : 0.5f + (phase - m) * 0.5f / (1 - m); }
} // namespace

float shape(unsigned wave, float phase, float d) {
    d = std::clamp(d, 0.0f, 0.97f);
    constexpr float tau = 2 * dsp::pi;
    float m = 0.5f - d * 0.49f; // the knee the bend moves
    switch (wave % 8) {
    case 0: return -std::cos(tau * knee(phase, m)); // saw
    case 1: { // square: each half bent the same way
        float half = phase < 0.5f ? phase * 2 : (phase - 0.5f) * 2;
        float bent = knee(half, m);
        return -std::cos(tau * (phase < 0.5f ? bent * 0.5f : 0.5f + bent * 0.5f));
    }
    case 2: { // pulse: the cycle squeezed into its start
        float w = 1 - d * 0.9f;
        return -std::cos(tau * std::min(phase / w, 1.0f));
    }
    case 3: { // double sine: two cycles, the second turned over
        float bent = knee(std::fmod(phase * 2, 1.0f), m);
        return -std::cos(tau * bent) * (phase < 0.5f ? 1.0f : -1.0f);
    }
    case 4: { // saw-pulse
        float bent = phase < 0.5f ? knee(phase * 2, m) * 0.5f : 0.5f + std::min((phase - 0.5f) * 2 / std::max(1 - d, 0.03f), 1.0f) * 0.5f;
        return -std::cos(tau * bent);
    }
    default: { // resonance: a higher partial under a window (saw, triangle, trapezoid)
        float partial = 1 + d * 15, carrier = -std::cos(tau * phase * partial);
        float window = wave % 8 == 5 ? 1 - phase : wave % 8 == 6 ? 1 - std::fabs(2 * phase - 1) : std::min(1.0f, 2 * (1 - phase));
        return (carrier + 1) * window - 1;
    }
    }
}

const EngineInfo& info() {
    static const EngineInfo engine {EngineKind::Cz, "CZ", true, params, Count, pages, sizeof(pages) / sizeof(pages[0]),
                                    {Dcw, DcwAmount, Wave1}, sounds, sizeof(sounds) / sizeof(sounds[0])};
    return engine;
}
} // namespace cz

using namespace cz;

CzEngine::CzEngine() { for (unsigned n = 0; n < Count; ++n) p[n] = info().params[n].initial; update(); }
void CzEngine::set_param(int id, int value) {
    if (id < 0 || id >= Count) return;
    p[size_t(id)] = std::clamp<int>(value, info().params[id].min, info().params[id].max);
    dirty = true;
}
void CzEngine::update() {
    dirty = false;
    using dsp::Envelope;
    dcw_shape = {Envelope::coefficient(time_seconds(p[DcwAttack])), Envelope::coefficient(time_seconds(p[DcwDecay])),
                 Envelope::coefficient(time_seconds(p[DcwRelease])), p[DcwSustain] / 100.0f};
    dca_shape = {Envelope::coefficient(time_seconds(p[DcaAttack])), Envelope::coefficient(time_seconds(p[DcaDecay])),
                 Envelope::coefficient(time_seconds(p[DcaRelease])), p[DcaSustain] / 100.0f};
}

void CzEngine::note_on(int key, int velocity) {
    if (dirty) update();
    Voice* v = nullptr;
    for (auto& x : voices) if (x.key == key) { v = &x; break; }
    if (!v) for (auto& x : voices) if (x.key < 0) { v = &x; break; }
    if (!v) { v = &voices[0]; for (auto& x : voices) if (x.age < v->age) v = &x; }
    *v = Voice {};
    v->key = key; v->held = true; v->age = ++clock; v->velocity = velocity / 127.0f;
    v->dcw.gate(true); v->dca.gate(true);
}
void CzEngine::note_off(int key) { for (auto& v : voices) if (v.key == key && v.held) { v.held = false; v.dcw.gate(false); v.dca.gate(false); } }
void CzEngine::all_notes_off() { for (auto& v : voices) if (v.held) { v.held = false; v.dcw.gate(false); v.dca.gate(false); } }
void CzEngine::silence() { for (auto& v : voices) v = Voice {}; }
bool CzEngine::sounding() const { for (auto& v : voices) if (v.key >= 0) return true; return false; }

void CzEngine::render(float* out, unsigned frames) {
    if (dirty) update();
    unsigned line = unsigned(p[Line]);
    bool both = line >= 2;
    for (auto& v : voices) {
        if (v.key < 0) continue;
        float key = v.key + bend * 2;
        // Line 1 (or 2 alone, a fifth of the detune up); with 1+1' or 1+2' the second line too, detuned.
        float dt0 = dsp::midi_hz(key + (line == 1 ? p[Octave] * 12 : 0)) / dsp::sample_rate;
        float dt1 = dsp::midi_hz(key + p[Octave] * 12 + p[Detune] / 100.0f) / dsp::sample_rate;
        float follow = 1 - p[KeyFollow] / 100.0f * std::clamp((key - 48) / 48.0f, 0.0f, 1.0f); // high notes bend less
        float gain = (1 - p[Velocity] / 100.0f * (1 - v.velocity)) * (both ? 0.18f : 0.26f);
        for (unsigned f = 0; f < frames; ++f) {
            float env = v.dcw.next(dcw_shape);
            float d = std::clamp((p[Dcw] / 100.0f + env * p[DcwAmount] / 100.0f + wheel * 0.5f) * follow, 0.0f, 1.0f);
            float sum = 0;
            for (unsigned l = 0; l < (both ? 2u : 1u); ++l) {
                float dt = l ? dt1 : dt0;
                unsigned wave = v.second[l] && p[Wave2] ? unsigned(p[Wave2] - 1) : unsigned(p[Wave1]);
                // Line 2 (1+2') plays the second wave throughout.
                if (l == 1 && line == 3 && p[Wave2]) wave = unsigned(p[Wave2] - 1);
                sum += shape(wave, v.phase[l], d);
                v.phase[l] += dt;
                if (v.phase[l] >= 1) { v.phase[l] -= 1; v.second[l] = !v.second[l]; }
            }
            out[f] += sum * v.dca.next(dca_shape) * gain;
        }
        if (!v.dca.active()) v.key = -1;
    }
}

} // namespace melodee
