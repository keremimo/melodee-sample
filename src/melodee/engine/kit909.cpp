// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/kit909.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace kit909 {
namespace {
constexpr ParamInfo P(const char* label, int min, int max, int initial, Format format = Format::Percent) {
    return {label, int16_t(min), int16_t(max), int16_t(initial), format, nullptr};
}
const ParamInfo params[Count] {
    P("TUNE", 0, 100, 40), P("DECAY", 0, 100, 55), P("ATTACK", 0, 100, 50), P("TUNE", 0, 100, 50), P("TONE", 0, 100, 45),
    P("SNAPPY", 0, 100, 60), P("DECAY", 0, 100, 45), P("LEVEL", 0, 100, 80),
    P("LOW", 0, 100, 40), P("MID", 0, 100, 50), P("HIGH", 0, 100, 60), P("DECAY", 0, 100, 50),
    P("CLOSED", 0, 100, 25), P("OPEN", 0, 100, 55), P("TONE", 0, 100, 60), P("TUNE", 0, 100, 50), P("DECAY", 0, 100, 60),
    P("TUNE", 0, 100, 50), P("DECAY", 0, 100, 55),
    P("KICK", 0, 100, 90), P("SNARE", 0, 100, 80), P("CLAP", 0, 100, 75), P("TOMS", 0, 100, 75), P("HATS", 0, 100, 70),
    P("CYMBALS", 0, 100, 60), P("DRIVE", 0, 100, 20),
};
constexpr int8_t _ = -1;
const EditPage pages[] {
    {"Kick", "KICK", Picture::Bars, {{{KickTune, KickDecay, KickAttack}, {_, _, _}, {_, _, _}}}},
    {"Snare and clap", "SNARE", Picture::Bars, {{{SnareTune, SnareTone, SnareSnappy}, {ClapDecay, RimLevel, _}, {_, _, _}}}},
    {"Toms", "TOMS", Picture::Bars, {{{LowTune, MidTune, HighTune}, {TomDecay, _, _}, {_, _, _}}}},
    {"Hats", "HATS", Picture::Bars, {{{ClosedDecay, OpenDecay, HatTone}, {_, _, _}, {_, _, _}}}},
    {"Cymbals", "CYMBALS", Picture::Bars, {{{CrashTune, CrashDecay, RideTune}, {RideDecay, _, _}, {_, _, _}}}},
    {"Levels", "LEVELS", Picture::Bars, {{{KickLevel, SnareLevel, ClapLevel}, {TomLevel, HatLevel, CymbalLevel}, {Drive, _, _}}}},
};
const Sound sounds[] {
    {"CLASSIC 909", {{{-1, 0}}}},
    {"TIGHT KIT", {{{KickDecay, 35}, {SnareSnappy, 75}, {ClosedDecay, 15}, {OpenDecay, 40}, {TomDecay, 35}, {Drive, 10}, {-1, 0}}}},
    {"BOOM KIT", {{{KickTune, 25}, {KickDecay, 85}, {KickAttack, 30}, {SnareTune, 35}, {TomDecay, 75}, {Drive, 40}, {-1, 0}}}},
    {"BRIGHT KIT", {{{KickTune, 55}, {SnareTone, 70}, {HatTone, 85}, {CrashTune, 70}, {RideTune, 70}, {-1, 0}}}},
    {"DIRTY KIT", {{{Drive, 85}, {KickDecay, 60}, {SnareSnappy, 85}, {ClapDecay, 70}, {-1, 0}}}},
};
// Stage: B1 SOUND (preset, the voice the last pad played, levels), B2 MIX, B3 FX. VOICE's
// pages follow the drums (pads 1-11): Snare, Clap and Rim share one, as on Edit.
using K = StageKind;
using S = StagePicture;
const StagePage voices[] {
    {"KICK", K::Params, S::Voice, {{KickTune, KickDecay, KickAttack}}},
    {"SNARE", K::Params, S::Voice, {{SnareTune, SnareTone, SnareSnappy}}, {{ClapDecay, RimLevel, _}}},
    {"TOMS", K::Params, S::Voice, {{LowTune, MidTune, HighTune}}, {{TomDecay, _, _}}},
    {"HATS", K::Params, S::Voice, {{ClosedDecay, OpenDecay, HatTone}}},
    {"CYMBALS", K::Params, S::Voice, {{CrashTune, CrashDecay, RideTune}}, {{_, _, RideDecay}}},
};
const int8_t pad_voice[16] {0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4, -1, -1, -1, -1, -1};
const StageGroups stage {{{
    {{{stage_preset, {"VOICE", K::Voice, S::Voice},
       {"LEVELS", K::Params, S::Levels, {{KickLevel, SnareLevel, ClapLevel}}, {{TomLevel, HatLevel, CymbalLevel}}}}}, 3},
    {{{stage_mix, {}, {}}}, 1},
    {{{stage_fx, {}, {}}}, 1},
}}, voices, pad_voice};
} // namespace

const char* drum_name(unsigned drum) {
    static const char* names[] {"Kick", "Snare", "Clap", "Rim", "Low tom", "Mid tom", "High tom", "Closed hat", "Open hat", "Crash", "Ride"};
    return drum < Drums ? names[drum] : "";
}

const EngineInfo& info() {
    static const EngineInfo engine {EngineKind::Kit909, "909", true, params, Count, pages, sizeof(pages) / sizeof(pages[0]),
                                    {KickTune, SnareTone, ClosedDecay}, sounds, sizeof(sounds) / sizeof(sounds[0]),
                                    nullptr, 0, true, &stage};
    return engine;
}
} // namespace kit909

using namespace kit909;
namespace {
constexpr float two_pi = 2 * dsp::pi;
float fs() { return dsp::sample_rate(); } // the device's rate now
// Down 60 dB in twice `seconds`: the drums ring about as long as their settings say.
float decay_per_sample(float seconds) { return std::exp(-6.9f / (std::max(seconds, 0.005f) * 2 * fs())); }
}

Kit909Engine::Kit909Engine() { for (unsigned n = 0; n < Count; ++n) p[n] = info().params[n].initial; }
void Kit909Engine::set_param(int id, int value) {
    if (id >= 0 && id < Count) p[size_t(id)] = std::clamp<int>(value, info().params[id].min, info().params[id].max);
}
void Kit909Engine::silence() { for (auto& v : voices) v = Voice {}; }
bool Kit909Engine::sounding() const { for (auto& v : voices) if (v.on) return true; return false; }

void Kit909Engine::note_on(int key, int velocity) {
    int d = key - 36;
    if (d < 0 || d >= Drums) return;
    if (d == ClosedHat) voices[OpenHat].on = false; // the closed hat chokes the open one
    auto& v = voices[size_t(d)];
    v = Voice {};
    v.on = true; v.velocity = 0.3f + 0.7f * velocity / 127.0f;
    // Each drum starts only what it uses, so it ends when those have died away.
    v.amp = d == Clap ? 0 : 1; v.noise_amp = d == Snare || d == Clap ? 1 : 0; v.pitch_env = 1;
}

// Six square waves at inharmonic ratios: the metal under hats and cymbals.
float Kit909Engine::metal(Voice& v, float base) {
    static const float ratios[] {1.0f, 1.4471f, 1.6170f, 1.9265f, 2.5028f, 2.6637f};
    float sum = 0;
    for (unsigned n = 0; n < 6; ++n) {
        v.phases[n] += base * ratios[n] / fs();
        v.phases[n] -= std::floor(v.phases[n]);
        sum += v.phases[n] < 0.5f ? 1.0f : -1.0f;
    }
    return sum / 6;
}

float Kit909Engine::drum(unsigned d, Voice& v) {
    auto pct = [&](int id) { return p[size_t(id)] / 100.0f; };
    float out = 0;
    v.age += 1;
    switch (d) {
    case Kick: {
        float base = 40 + pct(KickTune) * 35, sweep = base * (1 + 5 * v.pitch_env);
        v.pitch_env *= decay_per_sample(0.02f + 0.03f * (1 - pct(KickAttack)));
        v.phase += sweep / fs(); v.phase -= std::floor(v.phase);
        v.amp *= decay_per_sample(0.08f + pct(KickDecay) * 0.9f);
        float click = v.age < 60 ? noise.next() * (1 - v.age / 60) * pct(KickAttack) : 0;
        out = (std::sin(two_pi * v.phase) * v.amp + click * 0.6f) * pct(KickLevel) * 1.4f;
        break;
    }
    case Snare: {
        float base = 160 + pct(SnareTune) * 120;
        v.phase += base / fs(); v.phase2 += base * 1.87f / fs();
        v.phase -= std::floor(v.phase); v.phase2 -= std::floor(v.phase2);
        v.amp *= decay_per_sample(0.08f + 0.1f * pct(SnareTone));
        v.noise_amp *= decay_per_sample(0.05f + 0.25f * pct(SnareSnappy));
        float tone = (std::sin(two_pi * v.phase) + 0.6f * std::sin(two_pi * v.phase2)) * v.amp * (1 - 0.5f * pct(SnareTone));
        float n = noise.next();
        float hp = n - v.hp_last; v.hp_last = n;           // the snares: noise above the drum
        out = (tone + hp * v.noise_amp * (0.5f + 0.5f * pct(SnareTone))) * pct(SnareLevel);
        break;
    }
    case Clap: {
        // Three quick slaps, then the room.
        float t = v.age / fs(), gate = 0;
        for (float at : {0.0f, 0.011f, 0.022f}) if (t >= at && t < at + 0.009f) gate = std::exp(-(t - at) * 300);
        v.noise_amp *= decay_per_sample(0.08f + 0.4f * pct(ClapDecay));
        float n = noise.next();
        v.bp += 0.35f * (n - v.bp - 0.6f * v.lp); v.lp += 0.35f * v.bp; // a band of noise round 1 kHz
        out = v.bp * (t < 0.03f ? gate : v.noise_amp) * pct(ClapLevel) * 2.5f;
        break;
    }
    case Rim: {
        float t = v.age / fs();
        out = (std::sin(two_pi * 1700 * t) * 0.6f + std::sin(two_pi * 480 * t)) * std::exp(-t * 180) * pct(RimLevel);
        if (t > 0.05f) v.on = false;
        break;
    }
    case LowTom: case MidTom: case HighTom: {
        int tune = d == LowTom ? LowTune : d == MidTom ? MidTune : HighTune;
        float base = (d == LowTom ? 80 : d == MidTom ? 120 : 170) * (0.7f + 0.6f * pct(tune));
        v.pitch_env *= decay_per_sample(0.08f);
        v.phase += base * (1 + 0.6f * v.pitch_env) / fs(); v.phase -= std::floor(v.phase);
        v.amp *= decay_per_sample(0.15f + 0.6f * pct(TomDecay));
        out = (std::sin(two_pi * v.phase) * v.amp + noise.next() * v.amp * 0.05f) * pct(TomLevel);
        break;
    }
    case ClosedHat: case OpenHat: case Crash: case Ride: {
        bool hat = d == ClosedHat || d == OpenHat;
        float base = hat ? 300 + 300 * pct(HatTone) : d == Crash ? 260 + 200 * pct(CrashTune) : 360 + 240 * pct(RideTune);
        float seconds = d == ClosedHat ? 0.03f + 0.15f * pct(ClosedDecay) : d == OpenHat ? 0.15f + 0.8f * pct(OpenDecay)
                      : d == Crash ? 0.6f + 2.5f * pct(CrashDecay) : 0.5f + 2.0f * pct(RideDecay);
        v.amp *= decay_per_sample(seconds);
        float m = metal(v, base) * 0.7f + noise.next() * (d == Crash ? 0.5f : 0.25f);
        // High pass twice: only the shimmer stays.
        float hp1 = m - v.r1; v.r1 = m;
        float hp2 = hp1 - v.r2 * 0.6f; v.r2 = hp1;
        out = hp2 * v.amp * (hat ? pct(HatLevel) : pct(CymbalLevel)) * (d == Ride ? 0.7f : 1.0f);
        break;
    }
    default: break;
    }
    if (v.amp < 0.0005f && v.noise_amp < 0.0005f && v.age > 100) v.on = false;
    if (d == Clap && v.noise_amp < 0.0005f && v.age > 2000) v.on = false;
    return out * v.velocity;
}

void Kit909Engine::render(float* out, unsigned frames) {
    float drive = 1 + 4 * p[Drive] / 100.0f;
    for (unsigned f = 0; f < frames; ++f) {
        float sum = 0;
        for (unsigned d = 0; d < Drums; ++d) if (voices[d].on) sum += drum(d, voices[d]);
        out[f] += dsp::soft_clip(sum * 0.6f * drive) / std::sqrt(drive);
    }
}

} // namespace melodee
