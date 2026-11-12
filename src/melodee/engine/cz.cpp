// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/cz.h"
#include "melodee/engine/output_trim.h"
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
    P("RELEASE", 0, 100, 40, Format::Time), P("VELOCITY", 0, 100, 70),
    P("AT > VIB", 0, 100, expression::default_at_vib), P("AT > BRIGHT", 0, 100, expression::default_at_bright),
    P("LFO RATE", 0, 100, expression::default_vib_rate, Format::Rate),
};
constexpr int8_t _ = -1;
const EditPage pages[] {
    {"Waves", "WAVES", Picture::Bars, {{{Wave1, Wave2, Dcw}, {_, _, _}, {_, _, _}}}},
    {"Lines", "LINES", Picture::Bars, {{{Line, Detune, Octave}, {_, _, _}, {_, _, _}}}},
    {"DCW envelope", "DCW ENV", Picture::Bars, {{{DcwAmount, DcwAttack, DcwDecay}, {DcwSustain, DcwRelease, KeyFollow}, {_, _, _}}}},
    {"DCA envelope", "DCA ENV", Picture::Bars, {{{DcaAttack, DcaDecay, DcaSustain}, {DcaRelease, Velocity, _}, {_, _, _}}}},
    {"Touch", "TOUCH", Picture::Bars, {{{Velocity, _, _}, {AtVib, AtBright, LfoRate}, {_, _, _}}}},
};
// The mod matrix on CZ: brightness is the DCW (fully at full amount), and the two lines' pitches apart.
const char* const mod_dests[] {"None", "DCW", nullptr, "Amp level", "Pitch", "Line 1 pitch", "Line 2 pitch",
                               nullptr, "LFO rate", nullptr};
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

// Stage: B1 SOUND (preset, the DCW, mix), B2 SHAPE (DCW and DCA envelopes, touch), B3 PLAY (arp, lines, effects).
using K = StageKind;
using S = StagePicture;
const StageGroups stage {{{
    {{{stage_preset, {"TONE", K::Params, S::Bars, {{Dcw, DcwAmount, Wave1}}}, stage_mix, stage_sends}}, 4},
    {{{{"DCW ENV", K::Params, S::Envelope, {{DcwAttack, DcwDecay, DcwSustain}}, {{DcwRelease, DcwAmount, KeyFollow}}},
       {"DCA ENV", K::Params, S::Envelope, {{DcaAttack, DcaDecay, DcaSustain}}, {{DcaRelease, Velocity, _}}},
       {"TOUCH", K::Params, S::Bars, {{AtVib, AtBright, LfoRate}}}}}, 3},
    {{{stage_arp, stage_chord, {"LINES", K::Params, S::ShiftRows, {{Line, Detune, Octave}}, {{Wave1, Wave2, _}}}, stage_fx}}, 4},
}}};

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

namespace {
// Each wave's average over its cycle at each bend (DCW), worked out once: a voice takes it out, as the CZ's own
// output did (AC coupled), so no offset rides the envelopes as a thump or eats the headroom.
constexpr unsigned mean_steps = 64;
constexpr float most_bend = 0.97f; // shape()'s own limit
using MeanTable = std::array<std::array<float, mean_steps + 1>, 8>;
const MeanTable& means() {
    static const MeanTable table = [] {
        MeanTable t {};
        constexpr unsigned points = 1024;
        for (unsigned w = 0; w < 8; ++w)
            for (unsigned k = 0; k <= mean_steps; ++k) {
                double sum = 0;
                for (unsigned n = 0; n < points; ++n) sum += shape(w, (float(n) + 0.5f) / points, most_bend * float(k) / mean_steps);
                t[w][k] = float(sum / points);
            }
        return t;
    }();
    return table;
}
float mean_of(unsigned wave, float d) {
    float at = std::clamp(d, 0.0f, most_bend) / most_bend * mean_steps;
    unsigned i = std::min(unsigned(at), mean_steps - 1);
    const auto& m = means()[wave % 8];
    return m[i] + (m[i + 1] - m[i]) * (at - float(i));
}
// Where the wave, its average taken out, first crosses zero: a new voice starts there, from silence, not from
// the bottom of the cosine (a step the envelope's attack only rounds off: a click on every note).
float zero_crossing(unsigned wave, float d) {
    const float mean = mean_of(wave, d);
    float last = shape(wave, 0, d) - mean;
    if (last == 0) return 0;
    constexpr unsigned steps = 256;
    for (unsigned n = 1; n <= steps; ++n) {
        float x = shape(wave, float(n) / steps, d) - mean;
        if ((x >= 0) != (last >= 0)) return (float(n - 1) + last / (last - x)) / steps; // between the two, linearly
        last = x;
    }
    return 0;
}
} // namespace

const EngineInfo& info() {
    static const EngineInfo engine {EngineKind::Cz, "CZ", true, params, Count, pages, sizeof(pages) / sizeof(pages[0]),
                                    {Dcw, DcwAmount, Wave1}, sounds, sizeof(sounds) / sizeof(sounds[0]), mod_dests, legacy_count, true, &stage};
    return engine;
}
} // namespace cz

using namespace cz;

CzEngine::CzEngine() {
    for (unsigned n = 0; n < Count; ++n) p[n] = info().params[n].initial;
    (void)means(); // worked out now, not on the audio thread
    update();
}
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
    float random = noise.next();
    // A voice still sounding (the same key again, or the oldest taken) goes on from where it is: its envelopes
    // attack from their level now and its lines run on, so what it played never stops dead (a click). A new one
    // starts each line where its wave crosses zero at the bend it starts with.
    if (v->key < 0) {
        *v = Voice {};
        using expression::at;
        auto m = expression::sums(mods, {lfo_value, velocity / 127.0f, 0, wheel, (key - 60.0f) / 60.0f, random, 0});
        float pitch = float(key) + bend * 2 + at(m, ModDest::Pitch) * 12;
        float follow = 1 - p[KeyFollow] / 100.0f * std::clamp((pitch - 48) / 48.0f, 0.0f, 1.0f);
        // The bend at its first frame (the DCW envelope at zero): the DCW then reshapes the wave as it attacks, but
        // from silence, without a step.
        float d = std::clamp((p[Dcw] / 100.0f + wheel * 0.5f + at(m, ModDest::Cutoff)) * follow, 0.0f, 1.0f);
        for (unsigned l = 0; l < 2; ++l) {
            unsigned wave = l == 1 && p[Line] == 3 && p[Wave2] ? unsigned(p[Wave2] - 1) : unsigned(p[Wave1]);
            v->phase[l] = zero_crossing(wave, d);
        }
    }
    v->key = key; v->held = true; v->age = ++clock; v->velocity = velocity / 127.0f; v->accent = accent;
    v->slide = 0; v->pressure = v->touch = 0;
    v->random = random;
    v->dcw.gate(true); v->dca.gate(true);
}

// Slide or Tie from the sequencer (Engine::slide): the held voice of `from` goes on as `to`.
void CzEngine::slide(int from, int to, int velocity, bool glide) {
    bool found = false;
    for (auto& v : voices)
        if (v.key == from && v.held) {
            if (glide && from != to) v.slide += float(from - to);
            v.key = to; v.age = ++clock; v.accent = accent;
            found = true;
        }
    if (!found) note_on(to, velocity);
}
void CzEngine::set_pressure(int key, float value) { for (auto& v : voices) if (v.key == key && v.held) v.pressure = value; }
void CzEngine::note_off(int key) { for (auto& v : voices) if (v.key == key && v.held) { v.held = false; v.dcw.gate(false); v.dca.gate(false); } }
void CzEngine::all_notes_off() { for (auto& v : voices) if (v.held) { v.held = false; v.dcw.gate(false); v.dca.gate(false); } }
void CzEngine::silence() { for (auto& v : voices) v = Voice {}; }
bool CzEngine::sounding() const { for (auto& v : voices) if (v.key >= 0) return true; return false; }

void CzEngine::render(float* out, unsigned frames) {
    if (dirty) update();
    unsigned line = unsigned(p[Line]);
    bool both = line >= 2;
    lfo_value = lfo.next(rate_hertz(p[LfoRate]) * std::exp2(expression::lfo_rate_octaves(mods, wheel, lfo_value)), frames);
    for (auto& v : voices) {
        if (v.key < 0) continue;
        using expression::at;
        expression::follow(v.touch, v.pressure, frames);
        auto m = expression::sums(mods, {lfo_value, v.velocity, v.touch, wheel, (v.key - 60.0f) / 60.0f, v.random, v.dcw.level});
        // Pitch: a slide's way in, the bend, the matrix and pressure's vibrato.
        v.slide = expression::slide_settle(v.slide, frames);
        float key = v.key + v.slide + bend * 2 + at(m, ModDest::Pitch) * 12 + lfo_value * expression::vibrato_semitones(v.touch, p[AtVib]);
        // Line 1 (or 2 alone, a fifth of the detune up); with 1+1' or 1+2' the second line too, detuned.
        float dt0 = dsp::midi_hz(key + (line == 1 ? p[Octave] * 12 : 0) + at(m, line == 1 ? ModDest::OscBPitch : ModDest::OscAPitch) * 12) / dsp::sample_rate();
        float dt1 = dsp::midi_hz(key + p[Octave] * 12 + p[Detune] / 100.0f + at(m, ModDest::OscBPitch) * 12) / dsp::sample_rate();
        float follow = 1 - p[KeyFollow] / 100.0f * std::clamp((key - 48) / 48.0f, 0.0f, 1.0f); // high notes bend less
        float gain = expression::velocity_gain(v.velocity, p[Velocity] / 100.0f) * std::max(0.0f, 1 + at(m, ModDest::AmpLevel))
            * (both ? 0.18f : 0.26f) * output_trim::cz * v.accent;
        // Brightness: pressure and the matrix open the DCW.
        float open = v.touch * p[AtBright] / 100.0f * 0.6f + at(m, ModDest::Cutoff);
        for (unsigned f = 0; f < frames; ++f) {
            float env = v.dcw.next(dcw_shape);
            float d = std::clamp((p[Dcw] / 100.0f + env * p[DcwAmount] / 100.0f + wheel * 0.5f + open) * follow, 0.0f, 1.0f);
            float sum = 0;
            for (unsigned l = 0; l < (both ? 2u : 1u); ++l) {
                float dt = l ? dt1 : dt0;
                unsigned wave = v.second[l] && p[Wave2] ? unsigned(p[Wave2] - 1) : unsigned(p[Wave1]);
                // Line 2 (1+2') plays the second wave throughout.
                if (l == 1 && line == 3 && p[Wave2]) wave = unsigned(p[Wave2] - 1);
                sum += shape(wave, v.phase[l], d) - mean_of(wave, d); // its offset taken out
                v.phase[l] += dt;
                if (v.phase[l] >= 1) { v.phase[l] -= 1; v.second[l] = !v.second[l]; }
            }
            out[f] += sum * v.dca.next(dca_shape) * gain;
        }
        if (!v.dca.active()) v.key = -1;
    }
}

} // namespace melodee
