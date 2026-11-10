// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/sampler.h"
#include "melodee/engine/engine_info.h"
#include <algorithm>
#include <cmath>

namespace melodee {
using namespace sampler;

SamplerEngine::SamplerEngine() {
    const auto& info = engine_info(EngineKind::Sampler);
    for (unsigned n = 0; n < Count; ++n) set_param(int(n), info.params[n].initial);
}

void SamplerEngine::set_param(int id, int value) {
    if (id < 0 || id >= Count) return;
    const auto& info = engine_info(EngineKind::Sampler).params[id];
    p[size_t(id)] = std::clamp<int>(value, info.min, info.max);
    // The kit filter: a low pass that is out of the way when fully open.
    float hz = 20.0f * std::exp2(p[Filter] / 100.0f * 9.97f);
    filtered = p[Filter] < 99 || p[FilterType] != 0;
    g = std::tan(dsp::pi * std::min(hz, dsp::sample_rate * 0.45f) / dsp::sample_rate);
    k = 2 - 1.9f * p[Resonance] / 100.0f;
}

SamplerEngine::Voice& SamplerEngine::free_voice() {
    for (auto& v : voices) if (v.pad < 0) return v;
    Voice* oldest = &voices[0];
    for (auto& v : voices) if (v.age < oldest->age) oldest = &v;
    return *oldest;
}

void SamplerEngine::start(Voice& v, const PadSound& sound, int pad, int velocity, int semitones) {
    if (!samples || !sound.used() || sound.sample >= sample_slots) return;
    const SampleData* data = (*samples)[sound.sample];
    if (!data || !data->frames) return;
    // A pad in a mute group cuts the others in it.
    if (sound.group)
        for (auto& o : voices) if (o.pad >= 0 && o.group == sound.group && o.pad != pad) o.choked = true;
    v = Voice {};
    v.pad = pad; v.sample = data; v.slot = sound.sample;
    v.start = std::min(sound.start, data->frames - 1);
    v.end = sound.end && sound.end <= data->frames ? sound.end : data->frames;
    if (v.end <= v.start + 1) { v.pad = -1; return; }
    v.reverse = sound.reverse;
    v.position = v.reverse ? v.end - 1 : v.start;
    v.step = std::exp2((sound.pitch + semitones + sound.fine / 100.0 + p[Tune]) / 12.0);
    v.cloud = p[Cloud] != 0 && pad < int(sampler_pads); // not for Chop's audition
    // Follow: the project's tempo from the pad's own. By Tape, speed and pitch
    // move together; by Beats or Tones the sound is stretched, its pitch kept.
    // PITCH Stretch keeps a pitch change from changing the length too.
    double rate = p[Follow] && sound.own_tempo ? double(tempo) / sound.own_tempo : 1.0;
    bool tape = p[FollowHow] == 2;
    double pitch = tape ? v.step * rate : v.step;                         // what the taps read at
    double advance = rate * (p[PitchHow] == 1 ? 1.0 : v.step);            // how fast the sound moves on
    v.step = pitch;
    if (!v.cloud && pad < int(sampler_pads) && std::fabs(pitch - advance) > 1e-6) {
        v.stretch = true;
        v.beats = p[FollowHow] == 0;
        v.advance = advance;
        v.place = v.position;
        // Beats: a segment a sixteenth of the project long, cut in at its own place, so hits stay whole.
        // Tones: 60 ms windows, half overlapped, for held sounds.
        v.window = v.beats ? std::max(256u, unsigned(dsp::sample_rate * 15 / (tempo / 10.0))) : 2646u;
        v.hop = v.beats ? v.window : v.window / 2;
        v.since_hop = v.hop; // a tap at once
    }
    v.held = true; v.gate = sound.play == PadPlay::Gate; v.group = sound.group;
    v.age = ++clock;
    float level = sound.level / 100.0f * p[Level] / 100.0f, velocity_gain = 0.25f + 0.75f * velocity / 127.0f;
    v.gain = level * level * velocity_gain;
    float angle = (std::clamp<int>(sound.pan, -50, 50) + 50) / 100.0f * dsp::pi / 2;
    v.left = std::cos(angle) * 1.4142f; v.right = std::sin(angle) * 1.4142f; // centre is unity on both sides
    // The pad's envelope; the kit's DECAY scales every pad's decay.
    float decay = sound.decay >= 100 ? 1000.0f : time_seconds(sound.decay) * (p[Decay] / 100.0f) + 0.005f;
    v.shape = {dsp::Envelope::coefficient(time_seconds(sound.attack) * 0.3f), dsp::Envelope::coefficient(decay),
               dsp::Envelope::coefficient(time_seconds(sound.release) * 0.5f + 0.003f), sound.decay >= 100 ? 1.0f : sound.sustain / 100.0f};
    v.env.gate(true);
}

void SamplerEngine::note_on(int key, int velocity) {
    int pad = key - int(sampler_first_note);
    if (pad < 0 || pad >= int(sampler_pads)) return;
    start(free_voice(), pads[size_t(pad)], pad, velocity);
}

void SamplerEngine::play_pad(unsigned pad, int velocity, int semitones) {
    if (pad < sampler_pads) start(free_voice(), pads[pad], int(pad), velocity, semitones);
}

void SamplerEngine::note_off(int key) {
    int pad = key - int(sampler_first_note);
    for (auto& v : voices)
        if (v.pad == pad && v.held) { v.held = false; if (v.gate) v.env.gate(false); } // a one shot plays on to its end
}

void SamplerEngine::audition(uint16_t sample, uint32_t from, uint32_t to, int velocity) {
    PadSound sound;
    sound.sample = sample; sound.start = from; sound.end = to;
    for (auto& v : voices) if (v.pad == int(sampler_pads)) v.choked = true; // one audition at a time
    start(free_voice(), sound, int(sampler_pads), velocity);
}

void SamplerEngine::sample_gone(uint16_t slot) {
    for (auto& v : voices) if (v.slot == slot) v = Voice {};
    if (samples && slot < sample_slots)
        for (auto& g : grains) if (g.sample == (*samples)[slot]) g.sample = nullptr;
}

void SamplerEngine::all_notes_off() {
    for (auto& v : voices) if (v.pad >= 0) { v.held = false; v.env.gate(false); }
}
void SamplerEngine::silence() {
    for (auto& v : voices) v = Voice {};
    for (auto& g : grains) g.sample = nullptr;
    s1[0] = s1[1] = s2[0] = s2[1] = 0;
}
bool SamplerEngine::sounding() const {
    for (auto& v : voices) if (v.pad >= 0) return true;
    for (auto& g : grains) if (g.sample) return true;
    return false;
}

void SamplerEngine::render(float* out, unsigned frames) {
    float left[128], right[128];
    for (unsigned done = 0; done < frames; done += 128) {
        unsigned n = std::min(128u, frames - done);
        std::fill(left, left + n, 0.0f); std::fill(right, right + n, 0.0f);
        render_stereo(left, right, n);
        for (unsigned f = 0; f < n; ++f) out[done + f] += (left[f] + right[f]) * 0.5f;
    }
}

// One grain of a cloud: a short windowed piece from round the voice's place.
void SamplerEngine::spawn(Voice& v) {
    Grain* g = nullptr;
    for (auto& grain : grains) if (!grain.sample) { g = &grain; break; }
    if (!g) return;
    seed = seed * 1664525u + 1013904223u;
    float random = int32_t(seed) / 2147483648.0f;
    double span = v.end - v.start;
    double at = v.start + span * (p[Position] / 100.0) + v.scan + random * span * 0.05;
    at = std::fmod(std::fmod(at - v.start, span) + span, span) + v.start;
    seed = seed * 1664525u + 1013904223u;
    float detune = int32_t(seed) / 2147483648.0f * p[Spread] / 100.0f; // up to a semitone either way per 100 cents
    *g = Grain {v.sample, at, v.step * std::exp2(detune / 12.0), 0,
                uint32_t(0.01f * std::pow(100.0f, p[Size] / 100.0f) * dsp::sample_rate), v.gain * v.env.level, v.left, v.right};
}

// A stretched voice: taps begin where `place` has got to and read at the
// pad's pitch; Tones fades each over its window (sin², half overlapped, so
// they sum to one), Beats plays each segment whole with 2 ms edges.
void SamplerEngine::stretch(Voice& v, const float* l, const float* r, float* to_l, float* to_r, unsigned frames) {
    const double way = v.reverse ? -1.0 : 1.0;
    constexpr uint32_t edge = 88;
    for (unsigned f = 0; f < frames; ++f) {
        bool past = v.reverse ? v.place < v.start : v.place >= v.end - 1;
        if (past) { v.pad = -1; return; }
        if (v.since_hop >= v.hop) { // a new tap from here
            v.since_hop = 0;
            unsigned t = v.next_tap; v.next_tap ^= 1;
            v.tap[t] = v.place; v.tap_age[t] = 0; v.tap_on[t] = true;
        }
        ++v.since_hop;
        float a = v.env.next(v.shape) * v.gain * v.fade;
        if (v.choked) { v.fade -= 1.0f / 64; if (v.fade <= 0) { v.pad = -1; return; } }
        float sum_l = 0, sum_r = 0;
        for (unsigned t = 0; t < 2; ++t) {
            if (!v.tap_on[t]) continue;
            double at = v.tap[t];
            if (at < v.start || at >= v.end - 1 || v.tap_age[t] >= v.window) { v.tap_on[t] = false; continue; }
            float w;
            if (v.beats) {
                uint32_t age = v.tap_age[t], left = v.window - age;
                w = age < edge ? float(age) / edge : left < edge ? float(left) / edge : 1.0f;
            } else {
                float s = std::sin(dsp::pi * float(v.tap_age[t]) / float(v.window));
                w = s * s;
            }
            unsigned i = unsigned(at);
            float frac = float(at - i);
            unsigned j = std::min(i + 1, v.end - 1);
            sum_l += (l[i] + (l[j] - l[i]) * frac) * w;
            sum_r += (r[i] + (r[j] - r[i]) * frac) * w;
            v.tap[t] += way * v.step;
            ++v.tap_age[t];
        }
        to_l[f] += sum_l * a * v.left;
        to_r[f] += sum_r * a * v.right;
        v.place += way * v.advance;
        if (!v.env.active()) { v.pad = -1; return; }
    }
}

SamplerEngine::Lane* SamplerEngine::lane_for(int pad) {
    Lane* free = nullptr;
    for (auto& l : lane) {
        if (l.pad == pad) return &l;
        if (l.pad < 0 && !free) free = &l;
    }
    if (free) { free->pad = pad; free->fed = false; free->tail = 0; std::fill(std::begin(free->l), std::end(free->l), 0.0f); std::fill(std::begin(free->r), std::end(free->r), 0.0f); }
    return free;
}

void SamplerEngine::render_stereo(float* left, float* right, unsigned frames) {
    // Clouds: their voices only place grains; the grains play.
    for (auto& v : voices) {
        if (v.pad < 0 || !v.cloud) continue;
        for (unsigned f = 0; f < frames; ++f) v.env.next(v.shape);
        if (!v.env.active() || (v.choked && (v.fade -= 0.25f) <= 0)) { v.pad = -1; continue; }
        v.scan += (p[Scan] - 50) / 50.0 * v.step * frames; // forwards or backwards at up to its own speed
        v.next_grain -= frames;
        double every = dsp::sample_rate / std::max(1, int(1 + p[Density] * 0.99));
        while (v.next_grain <= 0) { spawn(v); v.next_grain += every; }
    }
    for (auto& g : grains) {
        if (!g.sample) continue;
        const float* l = g.sample->left.data();
        const float* r = g.sample->right.data();
        for (unsigned f = 0; f < frames && g.age < g.length; ++f, ++g.age) {
            unsigned i = unsigned(g.position) % g.sample->frames;
            float w = std::sin(dsp::pi * float(g.age) / float(g.length)) * g.gain * 0.5f; // a smooth window
            left[f] += l[i] * w * g.left; right[f] += r[i] * w * g.right;
            g.position += g.step;
        }
        if (g.age >= g.length) g.sample = nullptr;
    }
    // The punched pad renders on its own, and so does each pad with a chain
    // (blocks of 128 at most here).
    bool punching = aside && frames <= 128, chains = pad_effects && frames <= 128;
    if (punching) { std::fill(aside_l, aside_l + frames, 0.0f); std::fill(aside_r, aside_r + frames, 0.0f); }
    if (chains) for (auto& l : lane) if (l.pad >= 0) { l.fed = false; std::fill(l.l, l.l + frames, 0.0f); std::fill(l.r, l.r + frames, 0.0f); }
    for (auto& v : voices) {
        if (v.pad < 0 || v.cloud) continue;
        float* to_l = punching && v.pad == aside_pad ? aside_l : left;
        float* to_r = punching && v.pad == aside_pad ? aside_r : right;
        if (chains && v.pad < int(sampler_pads) && (*pad_effects)[size_t(v.pad)][0].kind() != EffectKind::None)
            if (Lane* l = lane_for(v.pad)) {
                if (!l->fed) { l->fed = true; l->tail = uint32_t(3 * dsp::sample_rate); }
                to_l = l->l; to_r = l->r;
            }
        const float* l = v.sample->left.data();
        const float* r = v.sample->right.data();
        if (v.stretch) { stretch(v, l, r, to_l, to_r, frames); continue; }
        for (unsigned f = 0; f < frames; ++f) {
            bool past = v.reverse ? v.position < v.start : v.position >= v.end - 1;
            if (past) { v.pad = -1; break; }
            unsigned i = unsigned(v.position);
            float frac = float(v.position - i);
            unsigned j = std::min(i + 1, v.end - 1);
            float a = v.env.next(v.shape) * v.gain * v.fade;
            if (v.choked) { v.fade -= 1.0f / 64; if (v.fade <= 0) { v.pad = -1; break; } } // a quick fade, not a click
            to_l[f] += (l[i] + (l[j] - l[i]) * frac) * a * v.left;
            to_r[f] += (r[i] + (r[j] - r[i]) * frac) * a * v.right;
            v.position += v.reverse ? -v.step : v.step;
            if (!v.env.active()) { v.pad = -1; break; }
        }
    }
    if (chains)
        for (auto& l : lane) {
            if (l.pad < 0) continue;
            for (auto& effect : (*pad_effects)[size_t(l.pad)]) effect.process(l.l, l.r, frames);
            float* into_l = punching && l.pad == aside_pad ? aside_l : left; // a punched pad: its chain, then the punch
            float* into_r = punching && l.pad == aside_pad ? aside_r : right;
            for (unsigned f = 0; f < frames; ++f) { into_l[f] += l.l[f]; into_r[f] += l.r[f]; }
            if (!l.fed) l.tail = l.tail > frames ? l.tail - frames : 0;
            if (!l.tail || (*pad_effects)[size_t(l.pad)][0].kind() == EffectKind::None) l.pad = -1;
        }
    if (punching) {
        aside->process(aside_l, aside_r, frames);
        for (unsigned f = 0; f < frames; ++f) { left[f] += aside_l[f]; right[f] += aside_r[f]; }
    }
    if (!filtered) return;
    float* io[2] {left, right};
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned f = 0; f < frames; ++f) {
            float x = io[side][f];
            float hp = (x - (k + g) * s1[side] - s2[side]) / (1 + g * (g + k));
            float bp = g * hp + s1[side], lp = g * bp + s2[side];
            s1[side] = g * hp + bp; s2[side] = g * bp + lp;
            io[side][f] = p[FilterType] == 1 ? bp : p[FilterType] == 2 ? hp : lp;
        }
}

} // namespace melodee
