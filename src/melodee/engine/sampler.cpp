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
    g = std::tan(dsp::pi * std::min(hz, dsp::sample_rate() * 0.45f) / dsp::sample_rate());
    k = 2 - 1.9f * p[Resonance] / 100.0f;
}

SamplerEngine::Voice& SamplerEngine::free_voice() {
    for (auto& v : voices) if (v.pad < 0) return v;
    Voice* oldest = &voices[0];
    for (auto& v : voices) if (v.age < oldest->age) oldest = &v;
    return *oldest;
}

void SamplerEngine::start(Voice& v, const PadSound& sound, int pad, int velocity, int semitones, int cutoff, int trigger) {
    if (!samples || !sound.used() || sound.sample > preview_slot) return;
    const SampleData* data = (*samples)[sound.sample];
    if (!data || !data->frames) return;
    // A pad in a mute group cuts the others in it; a mono pad cuts what it played before.
    if (sound.group)
        for (auto& o : voices) if (o.pad >= 0 && o.group == sound.group && o.pad != pad) o.choked = true;
    if (sound.mono)
        for (auto& o : voices) if (o.pad == pad && &o != &v) o.choked = true;
    v = Voice {};
    v.pad = pad; v.trigger = trigger >= 0 ? trigger : pad; v.sample = data; v.slot = sound.sample;
    v.start = std::min(sound.start, data->frames - 1);
    v.end = sound.end && sound.end <= data->frames ? sound.end : data->frames;
    if (v.end <= v.start + 1) { v.pad = -1; return; }
    v.reverse = sound.reverse;
    v.position = v.reverse ? v.end - 1 : v.start;
    v.loop = sound.loop && pad < int(sampler_pads);
    v.loop_at = std::clamp(sound.loop_point(), v.start, v.end - 2);
    v.edge_in = !v.loop && v.start > 0;              // the file's own start and end need no fade
    v.edge_out = !v.loop && v.end < data->frames;
    // Warp's mode is the kit's FollowHow: Tape is Pitch, the others Time stretch.
    // Warp on by Pitch leaves the pad's Semi and Fine out, as the MPC does.
    const bool tape = p[FollowHow] == 2, warped = sound.warped();
    const double own_tune = warped && tape ? 0.0 : sound.pitch + sound.fine / 100.0;
    v.step = std::exp2((own_tune + semitones + p[Tune]) / 12.0);
    v.cloud = p[Cloud] != 0 && pad < int(sampler_pads); // not for Chop's audition
    // Warp, else Follow: the speed the region moves at. Warp's Seq plays it at
    // the project's tempo from its own (by # Beats, else worked out from its
    // length); 50..200 % makes it that long. By Tape, speed and pitch move
    // together; by Beats or Tones the sound is stretched, its pitch kept.
    // A pitch change leaves the length alone under PITCH Stretch, and under
    // Warp by Time stretch; Warp Off plays it by speed, so tuned pads stay clean.
    double rate = 1.0, frames = double(v.end - v.start);
    if (sound.warp == warp_seq) {
        double own = sound.beats ? sound.beats * 60.0 * dsp::model_rate / frames * 10.0 : double(sound.own_tempo);
        if (own > 0) rate = tempo / own;
    } else if (sound.warp >= warp_min) rate = 100.0 / sound.warp;
    else if (p[Follow] && sound.own_tempo) rate = double(tempo) / sound.own_tempo;
    const bool keep_length = p[PitchHow] == 1 || (warped && !tape);
    const bool shifted = std::fabs(v.step - 1.0) > 1e-6;
    // Samples are at 44.1 kHz: at another device rate each output frame reads that much further along.
    double per_frame = dsp::model_rate / dsp::sample_rate();
    double pitch = (tape ? v.step * rate : v.step) * per_frame;        // what the taps read at
    double advance = rate * (keep_length ? 1.0 : v.step) * per_frame; // how fast the sound moves on
    v.step = pitch;
    if (!v.cloud && pad < int(sampler_pads) && std::fabs(pitch - advance) > 1e-6) {
        v.stretch = true;
        v.beats = p[FollowHow] == 0 && !(keep_length && shifted); // a note held to length wants overlapped windows
        v.advance = advance;
        v.place = v.position;
        // Beats: a segment a sixteenth of the project long, cut in at its own place, so hits stay whole.
        // Tones: 60 ms windows, half overlapped, for held sounds.
        v.window = v.beats ? std::max(256u, unsigned(dsp::sample_rate() * 15 / (tempo / 10.0))) : unsigned(0.06f * dsp::sample_rate());
        v.hop = v.beats ? v.window : v.window / 2;
        v.since_hop = v.hop; // a tap at once
    }
    v.held = true; v.gate = sound.play == PadPlay::Gate; v.group = sound.group;
    v.age = ++clock;
    // Offset: up to a sixteenth of the project's tempo after the hit.
    v.wait = pad < int(sampler_pads) ? uint32_t(sound.offset * (dsp::sample_rate() * 15 / (tempo / 10.0)) / 100) : 0;
    // Volume (dB, as the MPC shows it), Normalize, and velocity by the pad's sensitivity.
    float sense = sound.velocity_sense / 100.0f, curve = 0.25f + 0.75f * std::clamp(velocity, 0, 127) / 127.0f;
    float velocity_gain = 1.0f - sense * (1.0f - curve);
    v.gain = volume_gain(sound.level) * volume_gain(p[Level]) * velocity_gain * std::pow(10.0f, sound.gain / 2000.0f);
    float angle = (std::clamp<int>(sound.pan, -50, 50) + 50) / 100.0f * dsp::pi / 2;
    v.left = std::cos(angle); v.right = std::sin(angle); // constant power: centre is -3 dB a side, as the stock MPC plays it
    // The pad's envelope; the kit's DECAY scales every pad's decay. A one shot
    // that decays from the end plays whole and fades over its decay before the end.
    float decay_s = time_seconds(sound.decay) * (p[Decay] / 100.0f) + 0.005f;
    bool from_end = !v.gate && !sound.decay_from_start;
    bool hold = sound.decay >= 100 || from_end;
    v.shape = {dsp::Envelope::coefficient(time_seconds(sound.attack) * 0.3f), dsp::Envelope::coefficient(hold ? 1000.0f : decay_s),
               dsp::Envelope::coefficient(time_seconds(sound.release) * 0.5f + 0.003f), hold ? 1.0f : sound.sustain / 100.0f};
    if (from_end && sound.decay < 100 && !v.loop) v.tail = decay_s * dsp::sample_rate();
    v.env.gate(true);
    // The pad's filter, or 16 Levels' cutoff (Classic when the pad has none).
    v.filter = sound.filter;
    int at = sound.cutoff;
    if (cutoff >= 0) { at = cutoff; if (v.filter == PadFilter::Off) v.filter = PadFilter::Classic; }
    if (v.filter != PadFilter::Off) {
        float res = sound.resonance / 100.0f;
        v.cutoff = at / 100.0f * 9.97f;
        v.depth = sound.filter_amount / 100.0f * 7.0f; // up to seven octaves open
        bool four = v.filter == PadFilter::Lpf4 || v.filter == PadFilter::Hpf4 || v.filter == PadFilter::Bpf4;
        if (v.filter == PadFilter::Classic) v.k1 = 2.0f - 1.97f * res;
        else if (four) { v.k1 = 1.848f; v.k2 = std::max(0.05f, 0.765f - 0.715f * res); } // Butterworth pairs, the resonance on the second
        else v.k1 = std::max(0.05f, 1.414f - 1.364f * res);
        float f_decay = time_seconds(sound.filter_decay);
        bool f_hold = from_end;
        v.filter_shape = {dsp::Envelope::coefficient(time_seconds(sound.filter_attack) * 0.3f), dsp::Envelope::coefficient(f_hold ? 1000.0f : f_decay),
                          dsp::Envelope::coefficient(time_seconds(sound.filter_release) * 0.5f + 0.003f), f_hold ? 1.0f : sound.filter_sustain / 100.0f};
        if (f_hold && !v.loop && sound.filter_decay < 100) v.filter_tail = f_decay * dsp::sample_rate(); // 100 holds, as the amp's decay
        v.filter_env.gate(true);
        v.since_tune = 1u << 30; // tune on the first frame
    }
}

// A kit pad hit: a one shot that loops stops instead when it is playing;
// its linked pad (same bank) plays with it.
void SamplerEngine::hit(int pad, int velocity, int semitones, int cutoff) {
    if (pad < 0 || pad >= int(sampler_pads) || pad_muted(unsigned(pad))) return;
    const auto& sound = pads[size_t(pad)];
    if (sound.loop && sound.play == PadPlay::OneShot) {
        bool stopped = false;
        for (auto& v : voices)
            if (v.pad == pad && v.trigger == pad && v.loop && v.env.stage != dsp::Envelope::Stage::Release && !v.choked) {
                v.env.gate(false); v.filter_env.gate(false); stopped = true;
            }
        if (stopped) return;
    }
    start(free_voice(), sound, pad, velocity, semitones, cutoff);
    if (sound.link) {
        int linked = pad / 16 * 16 + (sound.link - 1) % 16;
        if (linked != pad && !pad_muted(unsigned(linked))) start(free_voice(), pads[size_t(linked)], linked, velocity, 0, -1, pad);
    }
}

void SamplerEngine::note_on(int key, int velocity) { hit(key - int(sampler_first_note), velocity, 0, -1); }

void SamplerEngine::play_pad(unsigned pad, int velocity, int semitones, int cutoff) { hit(int(pad), velocity, semitones, cutoff); }

void SamplerEngine::set_pad_mute(unsigned pad, bool on) {
    if (pad >= sampler_pads) return;
    uint64_t bit = uint64_t(1) << (pad % 64);
    muted[pad / 64] = on ? muted[pad / 64] | bit : muted[pad / 64] & ~bit;
    if (on) for (auto& v : voices) if (v.pad == int(pad)) v.choked = true; // fades out over a few milliseconds
}

void SamplerEngine::set_pad_mutes(const std::array<uint64_t, (sampler_pads + 63) / 64>& bits) {
    for (unsigned pad = 0; pad < sampler_pads; ++pad) set_pad_mute(pad, (bits[pad / 64] >> (pad % 64)) & 1);
}

void SamplerEngine::note_off(int key) {
    int pad = key - int(sampler_first_note);
    for (auto& v : voices)
        if (v.trigger == pad && v.pad >= 0 && v.held) { // a one shot plays on to its end (a loop until hit again)
            v.held = false;
            if (v.gate) { v.env.gate(false); v.filter_env.gate(false); }
        }
}

void SamplerEngine::audition(uint16_t sample, uint32_t from, uint32_t to, int velocity) {
    PadSound sound;
    sound.sample = sample; sound.start = from; sound.end = to;
    for (auto& v : voices) if (v.pad == int(sampler_pads)) v.choked = true; // one audition at a time
    if (sample == no_sample) return; // only that: the audition stops
    start(free_voice(), sound, int(sampler_pads), velocity);
}

void SamplerEngine::sample_gone(uint16_t slot) {
    for (auto& v : voices) if (v.slot == slot) v = Voice {};
    if (samples && slot <= preview_slot)
        for (auto& g : grains) if (g.sample == (*samples)[slot]) g.sample = nullptr;
}

void SamplerEngine::all_notes_off() {
    for (auto& v : voices) if (v.pad >= 0) { v.held = false; v.env.gate(false); v.filter_env.gate(false); }
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
                uint32_t(0.01f * std::pow(100.0f, p[Size] / 100.0f) * dsp::sample_rate()), v.gain * v.env.level, v.left, v.right};
}

// Output frames left before the region's end (reversed: its start), for
// the fades of a one shot that decays from the end; a loop has no end.
float SamplerEngine::frames_left(const Voice& v, double at, double speed) const {
    if (v.loop || (!v.tail && !v.filter_tail)) return 1e9f;
    double distance = v.reverse ? at - v.start : (v.end - 1) - at;
    return float(std::max(0.0, distance) / std::max(speed, 1e-6));
}

// A region that starts or ends inside its file (a chop's slice, a trimmed
// pad) fades in and out over 2 ms of the sample at those edges, whichever
// way it plays, so a cut mid-wave does not click (shell/chop.h's Zero Snap
// puts the cuts on zero crossings; this catches what is left: stereo, DC,
// deep bass). A loop has no fades: it would dip at every turn.
constexpr double slice_edge = 0.002 * dsp::model_rate;
float SamplerEngine::edge_gain(const Voice& v, double at) {
    double g = 1;
    if (v.edge_in) g = std::min(g, (at - v.start) / slice_edge);
    if (v.edge_out) g = std::min(g, ((v.end - 1) - at) / slice_edge);
    return float(std::clamp(g, 0.0, 1.0));
}

// One frame of a voice: through the pad's filter (its cutoff moved by its
// envelope, retuned every 16 frames), the amp and the pan.
void SamplerEngine::voice_out(Voice& v, float l, float r, float a, float left_frames, float* to_l, float* to_r, unsigned f) {
    if (v.tail > 0 && left_frames < v.tail) a *= left_frames / v.tail;
    if (v.filter != PadFilter::Off) {
        float e = v.filter_env.next(v.filter_shape);
        if (v.filter_tail > 0 && left_frames < v.filter_tail) e *= left_frames / v.filter_tail;
        if (++v.since_tune >= 16) {
            v.since_tune = 0;
            float hz = 20.0f * std::exp2(std::min(v.cutoff + v.depth * e, 10.5f));
            v.fg = std::tan(dsp::pi * std::min(hz, dsp::sample_rate() * 0.45f) / dsp::sample_rate());
        }
        const float g = v.fg;
        auto stage = [&](float x, float k, float* s) { // one state-variable stage: low, band and high pass
            float hp = (x - (k + g) * s[0] - s[1]) / (1 + g * (g + k));
            float bp = g * hp + s[0], lp = g * bp + s[1];
            s[0] = g * hp + bp; s[1] = g * bp + lp;
            return std::array<float, 3> {lp, bp, hp};
        };
        float* io[2] {&l, &r};
        for (unsigned side = 0; side < 2; ++side) {
            float x = *io[side];
            switch (v.filter) {
            case PadFilter::Classic: { // a 12 dB low pass that saturates as it rings, after the MPC3000's
                auto o = stage(dsp::soft_clip(x * 1.1f), v.k1, v.fs[0][side]);
                v.fs[0][side][0] = dsp::soft_clip(v.fs[0][side][0]);
                x = o[0];
                break;
            }
            case PadFilter::Lpf2: x = stage(x, v.k1, v.fs[0][side])[0]; break;
            case PadFilter::Hpf2: x = stage(x, v.k1, v.fs[0][side])[2]; break;
            case PadFilter::Bpf2: x = stage(x, v.k1, v.fs[0][side])[1]; break;
            case PadFilter::Lpf4: x = stage(stage(x, v.k1, v.fs[0][side])[0], v.k2, v.fs[1][side])[0]; break;
            case PadFilter::Hpf4: x = stage(stage(x, v.k1, v.fs[0][side])[2], v.k2, v.fs[1][side])[2]; break;
            case PadFilter::Bpf4: x = stage(stage(x, v.k1, v.fs[0][side])[1], v.k2, v.fs[1][side])[1]; break;
            default: break;
            }
            *io[side] = x;
        }
    }
    to_l[f] += l * a * v.left;
    to_r[f] += r * a * v.right;
}

// A stretched voice: taps begin where `place` has got to and read at the
// pad's pitch; Tones fades each over its window (sin², half overlapped, so
// they sum to one), Beats plays each segment whole with 2 ms edges.
void SamplerEngine::stretch(Voice& v, const float* l, const float* r, float* to_l, float* to_r, unsigned from, unsigned frames) {
    const double way = v.reverse ? -1.0 : 1.0;
    constexpr uint32_t edge = 88;
    for (unsigned f = from; f < frames; ++f) {
        bool past = v.reverse ? v.place < (v.loop ? v.loop_at : v.start) : v.place >= v.end - 1;
        if (past) {
            if (!v.loop) { v.pad = -1; return; }
            v.place = v.reverse ? v.end - 1 : v.loop_at;
            v.since_hop = v.hop; // a fresh tap from the loop point
        }
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
            if (v.edge_in || v.edge_out) w *= edge_gain(v, at); // a tap reaching the region's edge fades too
            unsigned i = unsigned(at);
            float frac = float(at - i);
            unsigned j = std::min(i + 1, v.end - 1);
            sum_l += (l[i] + (l[j] - l[i]) * frac) * w;
            sum_r += (r[i] + (r[j] - r[i]) * frac) * w;
            v.tap[t] += way * v.step;
            ++v.tap_age[t];
        }
        voice_out(v, sum_l, sum_r, a, frames_left(v, v.place, v.advance), to_l, to_r, f);
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
        double every = dsp::sample_rate() / std::max(1, int(1 + p[Density] * 0.99));
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
    bool knobbing = (knob_mask[0] || knob_mask[1]) && frames <= 128;
    if (punching) { std::fill(aside_l, aside_l + frames, 0.0f); std::fill(aside_r, aside_r + frames, 0.0f); }
    if (chains) for (auto& l : lane) if (l.pad >= 0) { l.fed = false; std::fill(l.l, l.l + frames, 0.0f); std::fill(l.r, l.r + frames, 0.0f); }
    knob_used = false;
    if (knobbing) { std::fill(knob_l, knob_l + frames, 0.0f); std::fill(knob_r, knob_r + frames, 0.0f); }
    // Where a pad's sound joins: the kit, or Knob FX's own output.
    auto out_l = [&](int pad) { if (knobbing && knobbed(pad)) { knob_used = true; return knob_l; } return left; };
    auto out_r = [&](int pad) { return knobbing && knobbed(pad) ? knob_r : right; };
    for (auto& v : voices) {
        if (v.pad < 0 || v.cloud) continue;
        float* to_l = punching && v.pad == aside_pad ? aside_l : out_l(v.pad);
        float* to_r = punching && v.pad == aside_pad ? aside_r : out_r(v.pad);
        if (chains && v.pad < int(sampler_pads) && (*pad_effects)[size_t(v.pad)][0].kind() != EffectKind::None)
            if (Lane* l = lane_for(v.pad)) {
                if (!l->fed) { l->fed = true; l->tail = uint32_t(3 * dsp::sample_rate()); }
                to_l = l->l; to_r = l->r;
            }
        const float* l = v.sample->left.data();
        const float* r = v.sample->right.data();
        unsigned from = 0;
        if (v.wait) { // Offset: it starts later in this block, or in a later one
            from = std::min(v.wait, frames);
            v.wait -= from;
            if (v.choked) { v.pad = -1; continue; }
            if (from == frames) continue;
        }
        if (v.stretch) { stretch(v, l, r, to_l, to_r, from, frames); continue; }
        const uint32_t low = v.reverse && v.loop ? v.loop_at : v.start;
        for (unsigned f = from; f < frames; ++f) {
            bool past = v.reverse ? v.position < low : v.position >= v.end - 1;
            if (past) {
                if (!v.loop) { v.pad = -1; break; }
                v.position = v.reverse ? v.end - 1 : v.loop_at; // round again
            }
            unsigned i = unsigned(v.position);
            float frac = float(v.position - i);
            unsigned j = std::min(i + 1, v.end - 1);
            float a = v.env.next(v.shape) * v.gain * v.fade;
            if (v.edge_in || v.edge_out) a *= edge_gain(v, v.position);
            if (v.choked) { v.fade -= 1.0f / 64; if (v.fade <= 0) { v.pad = -1; break; } } // a quick fade, not a click
            voice_out(v, l[i] + (l[j] - l[i]) * frac, r[i] + (r[j] - r[i]) * frac, a, frames_left(v, v.position, v.step), to_l, to_r, f);
            v.position += v.reverse ? -v.step : v.step;
            if (!v.env.active()) { v.pad = -1; break; }
        }
    }
    if (chains)
        for (auto& l : lane) {
            if (l.pad < 0) continue;
            for (auto& effect : (*pad_effects)[size_t(l.pad)]) effect.process(l.l, l.r, frames);
            float* into_l = punching && l.pad == aside_pad ? aside_l : out_l(l.pad); // a punched pad: its chain, then the punch
            float* into_r = punching && l.pad == aside_pad ? aside_r : out_r(l.pad);
            for (unsigned f = 0; f < frames; ++f) { into_l[f] += l.l[f]; into_r[f] += l.r[f]; }
            if (!l.fed) l.tail = l.tail > frames ? l.tail - frames : 0;
            if (!l.tail || (*pad_effects)[size_t(l.pad)][0].kind() == EffectKind::None) l.pad = -1;
        }
    if (punching) {
        aside->process(aside_l, aside_r, frames);
        float* into_l = out_l(aside_pad);
        float* into_r = out_r(aside_pad);
        for (unsigned f = 0; f < frames; ++f) { into_l[f] += aside_l[f]; into_r[f] += aside_r[f]; }
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
