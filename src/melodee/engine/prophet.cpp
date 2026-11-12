// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/prophet.h"
#include "melodee/engine/output_trim.h"
#include <algorithm>
#include <cmath>

namespace melodee {
using namespace prophet;

ProphetEngine::ProphetEngine() {
    const auto& info = prophet::info();
    for (unsigned n = 0; n < Count; ++n) p[n] = info.params[n].initial;
    for (unsigned n = 0; n < max_voices; ++n) {
        voices[n].a.reset(0.13f * n); voices[n].b.reset(0.29f * n); // free-running: voices start apart
    }
    update();
}

void ProphetEngine::set_param(int id, int value) {
    if (id < 0 || id >= Count) return;
    step.restart();
    const auto& info = prophet::info().params[id];
    p[size_t(id)] = std::clamp<int>(value, info.min, info.max);
    dirty = true;
}

void ProphetEngine::update() {
    dirty = false;
    using dsp::Envelope;
    amp_shape = {Envelope::coefficient(time_seconds(p[AmpAttack])), Envelope::coefficient(time_seconds(p[AmpDecay])),
                 Envelope::coefficient(time_seconds(p[AmpRelease])), p[AmpSustain] / 100.0f};
    filter_shape = {Envelope::coefficient(time_seconds(p[FilterAttack])), Envelope::coefficient(time_seconds(p[FilterDecay])),
                    Envelope::coefficient(time_seconds(p[FilterRelease])), p[FilterSustain] / 100.0f};
    // Glide: about 2 s at the top, per control block.
    float glide = p[Glide] ? 0.002f * std::pow(1000.0f, p[Glide] / 100.0f) : 0;
    glide_coefficient = glide > 0 ? 1.0f - std::exp(-float(control_block) / (glide * dsp::sample_rate()) * 3.0f) : 1.0f;
}

bool ProphetEngine::sounding() const {
    for (auto& v : voices) if (v.amp_env.active()) return true;
    return false;
}

ProphetEngine::Voice& ProphetEngine::allocate(int key) {
    unsigned limit = std::clamp(p[Voices], 1, int(max_voices));
    for (unsigned n = 0; n < limit; ++n) if (voices[n].key == key) return voices[n];
    for (unsigned n = 0; n < limit; ++n) if (!voices[n].amp_env.active()) return voices[n];
    Voice* oldest = nullptr; // a released voice first, else the oldest
    for (unsigned n = 0; n < limit; ++n)
        if (!voices[n].held && (!oldest || voices[n].age < oldest->age)) oldest = &voices[n];
    if (oldest) return *oldest;
    oldest = &voices[0];
    for (unsigned n = 1; n < limit; ++n) if (voices[n].age < oldest->age) oldest = &voices[n];
    return *oldest;
}

void ProphetEngine::set_pressure(int key, float value) {
    step.restart();
    for (auto& v : voices) if (v.key == key && v.held) v.pressure = value;
}

void ProphetEngine::start(Voice& v, int key, int velocity, bool retrigger) {
    bool was = v.amp_env.active();
    v.pressure = 0; v.touch = 0;
    v.random = noise.next();
    v.key = key; v.held = true; v.age = ++clock;
    v.velocity = velocity / 127.0f;
    v.accent = accent; v.slide = 0;
    v.target = float(key);
    if (!was || glide_coefficient >= 1.0f) v.pitch = float(key);
    if (retrigger || !was) { v.amp_env.gate(true); v.filter_env.gate(true); }
    if (!was) v.filter.reset();
}

void ProphetEngine::note_on(int key, int velocity) {
    step.restart();
    if (dirty) update();
    // Remember the held keys for mono and unison legato.
    if (stacked < stack.size()) {
        unsigned w = 0;
        for (unsigned n = 0; n < stacked; ++n) if (stack[n] != key) stack[w++] = stack[n];
        stacked = w;
        stack[stacked++] = key;
    }
    bool unison = p[Unison] != 0;
    unsigned limit = std::clamp(p[Voices], 1, int(max_voices));
    if (unison || limit == 1) {
        bool legato = false;
        for (unsigned n = 0; n < (unison ? limit : 1u); ++n) legato |= voices[n].held;
        for (unsigned n = 0; n < (unison ? limit : 1u); ++n) {
            auto& v = voices[n];
            start(v, key, velocity, !legato);
            float spread = limit > 1 ? (float(n) - (limit - 1) / 2.0f) / ((limit - 1) / 2.0f) : 0;
            v.detune = unison ? spread * p[Detune] / 100.0f * 0.35f : 0;
        }
        return;
    }
    auto& v = allocate(key);
    v.detune = 0;
    start(v, key, velocity, true);
}

// Slide or Tie from the sequencer (Engine::slide): the held voices of `from` go on as `to`. With the
// GLIDE knob up they glide at its rate, as a played legato does; else a slide takes its own 60 ms.
void ProphetEngine::slide(int from, int to, int velocity, bool glide) {
    step.restart();
    if (dirty) update();
    bool found = false;
    for (auto& v : voices)
        if (v.key == from && v.held) {
            v.slide = glide && glide_coefficient >= 1.0f ? v.pitch - float(to) : 0;
            v.key = to; v.target = float(to); v.age = ++clock; v.accent = accent;
            if (!glide || glide_coefficient >= 1.0f) v.pitch = float(to) + v.slide;
            found = true;
        }
    if (!found) { note_on(to, velocity); return; }
    for (unsigned n = 0; n < stacked; ++n) if (stack[n] == from) stack[n] = to; // mono and unison: `to` is held now
}

void ProphetEngine::note_off(int key) {
    step.restart();
    unsigned w = 0;
    for (unsigned n = 0; n < stacked; ++n) if (stack[n] != key) stack[w++] = stack[n];
    stacked = w;
    unsigned limit = std::clamp(p[Voices], 1, int(max_voices));
    if (p[Unison] || limit == 1) {
        // Back to the previous held key, legato; or release.
        for (unsigned n = 0; n < (p[Unison] ? limit : 1u); ++n) {
            auto& v = voices[n];
            if (v.key != key) continue;
            if (stacked) { v.key = stack[stacked - 1]; v.target = float(v.key); if (glide_coefficient >= 1) v.pitch = v.target; }
            else { v.held = false; v.amp_env.gate(false); v.filter_env.gate(false); }
        }
        return;
    }
    for (auto& v : voices)
        if (v.key == key && v.held) { v.held = false; v.amp_env.gate(false); v.filter_env.gate(false); }
}

void ProphetEngine::all_notes_off() {
    step.restart();
    stacked = 0;
    for (auto& v : voices) if (v.held) { v.held = false; v.amp_env.gate(false); v.filter_env.gate(false); }
}

void ProphetEngine::silence() {
    step.restart();
    stacked = 0;
    for (auto& v : voices) {
        v.key = -1; v.held = false; v.amp_env = {}; v.filter_env = {}; v.filter.reset();
    }
}

void ProphetEngine::control(Voice& v) {
    v.pitch += (v.target - v.pitch) * glide_coefficient;
    if (v.slide != 0) { // a sequenced slide's own glide, over the GLIDE knob's (none: at once)
        v.slide = expression::slide_settle(v.slide, control_block);
        v.pitch = v.target + v.slide;
    }
    // Vintage: each voice wanders a little in pitch.
    if (p[Vintage]) {
        float r = noise.next();
        v.drift = std::clamp(v.drift + r * 0.004f, -1.0f, 1.0f) * 0.999f;
    }
    float lfo_depth = std::min(1.0f, p[LfoAmount] / 100.0f + wheel * p[WheelLfo] / 100.0f);
    float lfo = lfo_value * lfo_depth;
    float poly_env = v.filter_env.level * p[PolyEnv] / 100.0f;
    float bend_st = bend * p[BendRange];
    float drift = v.drift * p[Vintage] / 100.0f * 0.15f;
    // The mod matrix: full amount moves the cutoff 5 octaves, resonance and
    // level fully, pitch 12 semitones, pulse width 0.45, noise fully.
    expression::follow(v.touch, v.pressure, control_block);
    using expression::at;
    auto m = expression::sums(mods, {lfo_value, v.velocity, v.touch, wheel, (v.pitch - 60.0f) / 60.0f, v.random, v.filter_env.level});
    v.noise_mod = at(m, ModDest::Noise);
    // Pressure, as aftertouch on a polysynth: vibrato and a brighter filter.
    float vibrato_st = vibrato_value * expression::vibrato_semitones(v.touch, p[AtVib]);
    float pitch_mod = at(m, ModDest::Pitch) * 12 + vibrato_st;

    float key_a = v.pitch + p[FreqA] + bend_st + drift + v.detune
        + lfo * p[LfoFreq] / 100.0f * 2.0f + wheel * p[WheelFreq] / 100.0f * 2.0f
        + (p[PolyFreqA] ? poly_env * 24.0f : 0) + pitch_mod + at(m, ModDest::OscAPitch) * 12;
    float key_b = (p[KeybB] ? v.pitch : 60.0f) + p[FreqB] + p[FineB] / 100.0f + bend_st + drift * 0.7f + v.detune
        + lfo * p[LfoFreq] / 100.0f * 2.0f + wheel * p[WheelFreq] / 100.0f * 2.0f + pitch_mod + at(m, ModDest::OscBPitch) * 12;
    v.dt_a = std::min(dsp::midi_hz(key_a) / dsp::sample_rate(), 0.45f);
    float hz_b = dsp::midi_hz(key_b);
    if (p[LoFreqB]) hz_b /= 64.0f;
    v.dt_b = std::min(hz_b / dsp::sample_rate(), 0.45f);

    float width_a = p[PwA] / 100.0f + lfo * p[LfoPw] / 100.0f * 0.4f + (p[PolyPwA] ? poly_env * 0.4f : 0) + at(m, ModDest::PulseWidth) * 0.45f;
    v.mix_a = {p[SawA] ? 1.0f : 0.0f, p[PulseA] ? 1.0f : 0.0f, 0, std::clamp(width_a, 0.05f, 0.95f)};
    float width_b = p[PwB] / 100.0f + lfo * p[LfoPw] / 100.0f * 0.4f;
    v.mix_b = {p[SawB] ? 1.0f : 0.0f, p[PulseB] ? 1.0f : 0.0f, p[TriB] ? 1.0f : 0.0f, std::clamp(width_b, 0.05f, 0.95f)};

    // Filter: octaves above 20 Hz, then every modulation in octaves.
    float octaves = p[Cutoff] / 100.0f * 9.97f;
    octaves += v.filter_env.level * p[EnvAmount] / 100.0f * 7.0f;
    octaves += (v.pitch - 60.0f) / 12.0f * (p[KeyTrack] * 0.5f);
    octaves += (v.velocity - 0.5f) * p[VelFilter] / 100.0f * 4.0f;
    octaves += lfo * p[LfoFilter] / 100.0f * 3.0f + wheel * p[WheelFilter] / 100.0f * 4.0f;
    // Poly mod into the filter: the envelope, and oscillator B where it is in its cycle.
    if (p[PolyFilter]) octaves += poly_env * 4.0f + (2.0f * v.b.phase - 1.0f) * p[PolyOscB] / 100.0f * 2.0f;
    octaves += at(m, ModDest::Cutoff) * 5 + v.touch * p[AtBright] / 100.0f * 4.0f;
    v.filter.tune(20.0f * std::exp2(octaves), std::clamp(p[Resonance] / 100.0f + at(m, ModDest::Resonance), 0.0f, 1.0f));
    v.gain = std::max(0.0f, expression::velocity_gain(v.velocity, p[VelAmp] / 100.0f) * (1.0f + at(m, ModDest::AmpLevel))) * v.accent;
}

void ProphetEngine::render(float* out, unsigned frames) {
    if (dirty) update();
    // Unison stacks every voice on one key: their detuned sum, as loud as one voice (output_trim.h).
    float output_gain = 0.7f * output_trim::prophet / (p[Unison] ? std::sqrt(float(std::clamp(p[Voices], 1, int(max_voices)))) : 1.0f);
    float level_a = p[LevelA] / 100.0f, level_b = p[LevelB] / 100.0f, level_n = p[Noise] / 100.0f;
    float poly_b_fm = p[PolyFreqA] ? p[PolyOscB] / 100.0f * 2.0f : 0;
    float poly_b_pw = p[PolyPwA] ? p[PolyOscB] / 100.0f * 0.4f : 0;
    bool sync = p[Sync] != 0;
    for (unsigned start = 0; start < frames;) {
        // A control step on its own frames (engine.h ControlStep), not from this call's start.
        bool stepping = step.starts(control_block);
        if (stepping) {
            unsigned moved = step.take();
            float rate_octaves = expression::lfo_rate_octaves(mods, wheel, lfo_value);
            lfo_value = lfo.next(rate_hertz(p[LfoRate]) * std::exp2(rate_octaves), unsigned(p[LfoShape]), float(moved) / dsp::sample_rate());
            vibrato_value = vibrato.next(rate_hertz(p[VibRate]), moved);
        }
        unsigned n = step.run(frames - start);
        for (auto& v : voices) {
            if (stepping) { // the voices sounding as the step starts play it through
                v.live = v.amp_env.active();
                if (!v.live) { if (v.key >= 0 && !v.held) v.key = -1; continue; }
                control(v);
            } else if (!v.live) {
                continue;
            }
            for (unsigned f = 0; f < n; ++f) {
                bool wrap_b, wrap_a;
                float b = v.b.next(v.dt_b, v.mix_b, wrap_b);
                auto mix_a = v.mix_a;
                if (poly_b_pw != 0) mix_a.width = std::clamp(mix_a.width + b * poly_b_pw, 0.05f, 0.95f);
                float a = v.a.next(v.dt_a * (1.0f + b * poly_b_fm), mix_a, wrap_a);
                if (sync && wrap_b) v.a.reset(v.b.phase * v.dt_a / std::max(v.dt_b, 1e-6f));
                float ln = std::clamp(level_n + v.noise_mod, 0.0f, 1.0f);
                float x = a * level_a + b * level_b + (ln != 0 ? noise.next() * ln : 0);
                x *= 0.5f;
                float y = v.filter.process(x);
                v.filter_env.next(filter_shape);
                out[start + f] += y * v.amp_env.next(amp_shape) * v.gain * output_gain;
            }
        }
        step.played(n);
        start += n;
    }
}

} // namespace melodee
