// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/sampler.h"
#include "melodee/engine/engine_info.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace melodee {
using namespace sampler;

// A one shot's region edges inside its file fade over 2 ms of the sample (edge_gain).
constexpr double slice_edge = 0.002 * dsp::model_rate;

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
    // The kit's tune, level, decay and speed reach what plays too (the kit filter and a cloud's values already do).
    if (id == Tune || id == Level || id == Decay || id == Follow || id == FollowHow || id == PitchHow) retune_all = true;
}

// A free voice. With max_voices sounding already, the oldest is choked (it fades out over a few milliseconds, as a
// mute group's cut does) and the new one plays in a spare voice meanwhile, so a full kit never cuts a voice dead.
// Only with every spare still fading does the one nearest silence give way.
SamplerEngine::Voice& SamplerEngine::free_voice() {
    Voice* free = nullptr;
    Voice* oldest = nullptr;
    Voice* fading = nullptr; // the choked voice furthest faded
    unsigned live = 0;
    for (auto& v : voices) {
        if (v.pad < 0) { if (!free) free = &v; continue; }
        if (v.choked) { if (!fading || v.fade < fading->fade) fading = &v; continue; }
        ++live;
        if (!oldest || v.age < oldest->age) oldest = &v;
    }
    if (live >= max_voices && oldest) oldest->choked = true;
    if (free) return *free;
    if (fading) return *fading;
    return oldest ? *oldest : voices[0];
}

void SamplerEngine::end_stream(Voice& v) {
    if (v.stream >= 0 && pager) pager->close(v.stream);
    v.stream = -1;
}

void SamplerEngine::start(Voice& v, const PadSound& sound, int pad, int velocity, int semitones, int cutoff, int trigger,
                          const SampleZone* zone, Multisample* multi, int key) {
    if (!samples || !sound.used() || sound.sample > preview_slot) return;
    const SampleData* data = (*samples)[sound.sample];
    ZonePart* part = zone && multi ? &multi->parts[zone->part] : nullptr;
    if (!data || (!part && !data->frames)) return;
    glide_setup();
    // A pad in a mute group cuts the others in it; a mono pad cuts what it played before at the same pitch, so
    // 16 Levels' notes at other pitches (a chord) sound together.
    if (sound.group)
        for (auto& o : voices) if (o.pad >= 0 && o.group == sound.group && o.pad != pad) o.choked = true;
    if (sound.mono && !zone) // an instrument's press has cut these already: its layers must not cut each other
        for (auto& o : voices) if (o.pad == pad && o.semis == semitones && &o != &v) o.choked = true;
    end_stream(v);
    v = Voice {};
    v.pad = pad; v.trigger = trigger >= 0 ? trigger : pad; v.sample = data; v.slot = sound.sample;
    v.velocity = uint8_t(std::clamp(velocity, 0, 127)); v.semis = semitones; v.levels_cutoff = int16_t(cutoff); v.accent = accent;
    // Roadmap P2: a sample made ready at the device's rate plays from that copy, a frame each device frame (only its
    // pitch moves the read on), the pad's 44.1 kHz frames scaled there; else (an instrument's zones, or no copy made)
    // from its 44.1 kHz frames, each device frame reading that much further on.
    v.native = !part && data->plays_at(dsp::sample_rate());
    v.scale = v.native ? double(data->play_rate) / dsp::model_rate : 1.0;
    auto at_rate = [&](uint32_t frame) { return v.native ? uint32_t(std::llround(frame * v.scale)) : frame; };
    const uint32_t total = part ? part->total : v.native ? data->play_frames : data->frames;
    v.start = std::min(zone ? zone->start : at_rate(sound.start), total - 1);
    uint32_t end = zone ? zone->end : at_rate(sound.end);
    v.end = end && end <= total ? end : total;
    if (v.end <= v.start + 1) { v.pad = -1; return; }
    v.l = part ? part->left : v.native ? data->play_left.data() : data->left.data();
    v.r = part ? part->right : v.native ? data->play_right.data() : data->right.data();
    v.reverse = sound.reverse;
    v.position = v.reverse ? v.end - 1 : v.start;
    v.total = total;
    if (zone) {
        v.zone = true; v.key = key; v.off_by = zone->off_by;
        v.multi = multi; v.zone_index = uint32_t(zone - multi->zones.data());
        v.loop = !zone->one_shot && !v.reverse && zone->loop != ZoneLoop::Off && zone->loop_end > zone->loop_start + 16 && zone->loop_end <= v.end;
        v.loop_at = zone->loop_start; v.loop_hi = zone->loop_end;
        v.loop_held = zone->loop == ZoneLoop::UntilReleased;
    } else loop_setup(v, sound);
    // A sample still being read (the browser's preview) plays what has landed, as a stream plays what is pinned.
    if (!part && data->growth && !data->growth->whole.load(std::memory_order_acquire)) v.grow = data->growth.get();
    v.cloud = sound.cloud && pad < int(sampler_pads) && !v.grow; // not for Chop's audition, nor before all is there
    v.held = true; v.gate = sound.play == PadPlay::Gate && !(zone && zone->one_shot); v.group = sound.group;
    if (zone && zone->on_release) v.held = v.gate = v.loop = false; // a release sample plays out once
    aim(v, sound, zone, multi, key);
    settle(v);
    // A speed of its own and a pitch of its own: stretched.
    if (!v.cloud && pad < int(sampler_pads) && !(zone && zone->on_release) && std::fabs(v.step - v.advance) > 1e-6) begin_stretch(v, false);
    // A cloud never reads to its region's end, so a one shot's cloud lasts as
    // long as the pad would play, then lets go; a gate ends on release, a loop when hit again.
    if (v.cloud && !v.gate && !v.loop) v.life = std::max(1.0, double(v.end - v.start) / std::max(v.advance, 1e-6));
    v.age = ++clock;
    // Offset: up to a sixteenth of the project's tempo after the hit.
    v.wait = pad < int(sampler_pads) ? uint32_t(sound.offset * (dsp::sample_rate() * 15 / (tempo / 10.0)) / 100) : 0;
    // Both envelopes start, the filter's too when the pad has none yet, so one put on as it plays opens as it would.
    v.env.gate(true);
    v.filter_env.gate(true);
    v.since_tune = 1u << 30; // the filter tuned on its first frame
    if (part && pager) {
        v.stream = pager->open(*part, uint32_t(v.position), v.reverse, float(std::max(v.advance, v.step)));
        if (v.stream < 0) { // no stream free: what is held plays, then it ends
            if (v.reverse) { v.pad = -1; return; }
            v.capped = true;
            v.head_end = std::min(v.start + stream_chunk, v.end);
        }
    }
}

// The glide a voice's values take to what they are asked (aim): a one pole of 8 ms, for six of those (to within
// 0.25 % of the way), then there.
void SamplerEngine::glide_setup() {
    const float rate = dsp::sample_rate();
    if (rate == glide_rate) return;
    constexpr float seconds = 0.008f;
    glide_rate = rate;
    glide_step = 1.0f - std::exp(-1.0f / (seconds * rate));
    glide_frames = std::max(1u, uint32_t(seconds * 6 * rate));
}

// What a voice's pad (`sound`, its layer's or zone's) and the kit ask of it now: its speed and pitch, level, pan,
// envelopes, the decay from its end, and its filter. At its start, and again whenever any of them moves (refresh).
// `key` is an instrument's key now, the pad's Semi in it.
void SamplerEngine::aim(Voice& v, const PadSound& sound, const SampleZone* zone, Multisample* multi, int key) {
    const int pad = v.pad;
    // Warp's mode is the kit's FollowHow: Tape is Pitch, the others Time stretch.
    // Warp on by Pitch leaves the pad's Semi and Fine out, as the MPC does.
    const bool tape = p[FollowHow] == 2, warped = !zone && sound.warped();
    // A zone is in tune by its key from its root; the key has the pad's Semi and the note in it.
    // Round 3 "instrument C": the zone's trim on this pad, cents and tenths of a dB over the file's own.
    const ZoneTrim* trim = zone && multi && pad >= 0 && pad < int(sampler_pads) ? find_trim(trims, unsigned(pad), v.zone_index, sound.sample) : nullptr;
    const double own_tune = zone ? (key - int(zone->root)) * zone->keytrack / 100.0 + zone->tune / 100.0 + sound.fine / 100.0
                                       + (trim ? trim->tune / 100.0 : 0.0)
                          : warped && tape ? 0.0 : sound.pitch + sound.fine / 100.0;
    const double step = std::exp2((own_tune + (zone ? 0 : v.semis) + p[Tune]) / 12.0);
    // Warp, else Follow: the speed the region moves at. Warp's Seq plays it at
    // the project's tempo from its own (by # Beats, else worked out from its
    // length); 50..200 % makes it that long. By Tape, speed and pitch move
    // together; by Beats or Tones the sound is stretched, its pitch kept.
    // A pitch change leaves the length alone under PITCH Stretch, and under
    // Warp by Time stretch; Warp Off plays it by speed, so tuned pads stay clean.
    double rate = 1.0;
    const double frames = double(v.end - v.start);
    v.follows = false;
    if (zone) {} // no tempo of its own, and no Warp: a zone is one note of many
    else if (sound.warp == warp_seq) {
        double own = sound.beats ? sound.beats * 60.0 * dsp::model_rate / (frames / v.scale) * 10.0 : double(sound.own_tempo);
        if (own > 0) { rate = tempo / own; v.follows = true; }
    } else if (sound.warp >= warp_min) rate = 100.0 / sound.warp;
    else if (p[Follow] && sound.own_tempo) { rate = double(tempo) / sound.own_tempo; v.follows = true; }
    const bool keep_length = p[PitchHow] == 1 || (warped && !tape);
    const bool shifted = std::fabs(step - 1.0) > 1e-6;
    // At 44.1 kHz (no copy at the device's rate) each output frame reads that much further along.
    const double per_frame = v.native ? 1.0 : dsp::model_rate / dsp::sample_rate();
    v.aim.step = (tape ? step * rate : step) * per_frame;             // what it reads at (a stretch's taps)
    v.aim.advance = rate * (keep_length ? 1.0 : step) * per_frame;    // how fast the sound moves on
    if (bend != 0.0f && pad < int(sampler_pads)) { // the pitch wheel (not Chop's audition): a stretched voice keeps its length, the rest move with it as tape does
        const double bent = std::exp2(double(bend) / 12.0);
        v.aim.step *= bent;
        if (!v.stretch) v.aim.advance *= bent;
    }
    v.aim.beats = p[FollowHow] == 0 && !(keep_length && shifted);     // a note held to length wants overlapped windows
    // Volume (dB, as the MPC shows it), Normalize, and velocity by the pad's sensitivity: at 100 the gain follows
    // the velocity (64 is -6 dB, 20 is -16 dB), at 0 every hit is full.
    const float sense = sound.velocity_sense / 100.0f, curve = v.velocity / 127.0f;
    const float velocity_gain = 1.0f - sense * (1.0f - curve);
    v.aim.gain = volume_gain(sound.level) * volume_gain(p[Level]) * velocity_gain * std::pow(10.0f, sound.gain / 2000.0f) * v.accent * v.fall;
    if (zone) v.aim.gain *= std::pow(10.0f, (zone->gain + (trim ? trim->level / 10.0f : 0.0f)) / 20.0f)
                          * zone_crossfade(*zone, v.key, std::clamp<int>(v.velocity, 1, 127)); // its xfin / xfout, by the key pressed
    const float angle = (std::clamp<float>(float(sound.pan) + (zone ? zone->pan * 50.0f : 0.0f), -50.0f, 50.0f) + 50) / 100.0f * dsp::pi / 2;
    v.aim.left = std::cos(angle); v.aim.right = std::sin(angle); // constant power: centre is -3 dB a side, as the stock MPC plays it
    // The pad's envelope; the kit's DECAY scales every pad's decay. A one shot
    // that decays from the end plays whole and fades over its decay before the end.
    // A zone's stages are off the pad's, so the pad's controls still move every zone.
    const int attack = zone ? std::clamp(sound.attack + zone->attack, 0, 100) : sound.attack;
    const int decay = zone ? std::clamp(sound.decay + zone->decay, 0, 100) : sound.decay;
    const int sustain = zone ? std::clamp(sound.sustain + zone->sustain, 0, 100) : sound.sustain;
    const int release = zone ? std::clamp(sound.release + zone->release, 0, 100) : sound.release;
    const float decay_s = time_seconds(decay) * (p[Decay] / 100.0f) + 0.005f;
    const bool from_end = !v.gate && !sound.decay_from_start;
    const bool hold = decay >= 100 || from_end;
    v.shape = {dsp::Envelope::coefficient(time_seconds(attack) * 0.3f), dsp::Envelope::coefficient(hold ? 1000.0f : decay_s),
               dsp::Envelope::coefficient(time_seconds(release) * 0.5f + 0.003f), hold ? 1.0f : sustain / 100.0f};
    v.aim.tail = from_end && decay < 100 && !v.loop ? decay_s * dsp::sample_rate() : 0.0f;
    // The pad's filter, or 16 Levels' cutoff (Classic when the pad has none).
    PadFilter filter = sound.filter;
    int at = sound.cutoff;
    if (v.levels_cutoff >= 0) { at = v.levels_cutoff; if (filter == PadFilter::Off) filter = PadFilter::Classic; }
    const bool zone_filter = zone && zone->filter != PadFilter::Off && zone->cutoff > 0;
    if (zone_filter) filter = zone->filter;
    v.filter = filter;
    v.aim.filter_tail = 0;
    if (filter == PadFilter::Off) return;
    const float res = sound.resonance / 100.0f;
    v.aim.cutoff = at / 100.0f * 9.97f;
    if (zone_filter) // its own Hz, off the pad's CUTOFF wide open, so that control still moves it
        v.aim.cutoff = std::log2(zone->cutoff / 20.0f) + (zone->cutoff_velocity * v.velocity / 127.0f + zone->cutoff_key * float(key - int(zone->cutoff_center))) / 1200.0f
                     + (at / 100.0f - 1.0f) * 9.97f;
    v.aim.depth = sound.filter_amount / 100.0f * 7.0f; // up to seven octaves open
    const bool four = filter == PadFilter::Lpf4 || filter == PadFilter::Hpf4 || filter == PadFilter::Bpf4;
    v.aim.k1 = v.aim.k2 = 1.4f;
    if (filter == PadFilter::Classic) v.aim.k1 = 2.0f - 1.97f * res;
    else if (four) { v.aim.k1 = 1.848f; v.aim.k2 = std::max(0.05f, 0.765f - 0.715f * res); } // Butterworth pairs, the resonance on the second
    else v.aim.k1 = std::max(0.05f, 1.414f - 1.364f * res);
    if (zone_filter) v.aim.k1 = std::max(0.05f, std::pow(10.0f, -zone->resonance / 20.0f)); // the dB its peak has
    const float f_decay = time_seconds(sound.filter_decay);
    const bool f_hold = from_end;
    v.filter_shape = {dsp::Envelope::coefficient(time_seconds(sound.filter_attack) * 0.3f), dsp::Envelope::coefficient(f_hold ? 1000.0f : f_decay),
                      dsp::Envelope::coefficient(time_seconds(sound.filter_release) * 0.5f + 0.003f), f_hold ? 1.0f : sound.filter_sustain / 100.0f};
    if (f_hold && !v.loop && sound.filter_decay < 100) v.aim.filter_tail = f_decay * dsp::sample_rate(); // 100 holds, as the amp's decay
}

// A voice at what it is asked, at once (its start, a cloud: its grains take what it has as they begin).
void SamplerEngine::settle(Voice& v) {
    v.step = v.aim.step; v.advance = v.aim.advance;
    v.gain = v.aim.gain; v.left = v.aim.left; v.right = v.aim.right;
    v.cutoff = v.aim.cutoff; v.depth = v.aim.depth; v.k1 = v.aim.k1; v.k2 = v.aim.k2;
    v.tail = v.aim.tail; v.filter_tail = v.aim.filter_tail;
    v.gliding = 0;
}

// A frame of a voice's glide to what it is asked: what plays moves at once and without a step (no zipper on a
// turned level or cutoff, no click on a pan). The cutoff glides in octaves, so a sweep sounds even.
inline void SamplerEngine::glide(Voice& v) {
    if (!v.gliding) return;
    if (--v.gliding == 0) { settle(v); return; }
    const float k = glide_step;
    const double kd = k;
    v.step += (v.aim.step - v.step) * kd; v.advance += (v.aim.advance - v.advance) * kd;
    v.gain += (v.aim.gain - v.gain) * k; v.left += (v.aim.left - v.left) * k; v.right += (v.aim.right - v.right) * k;
    v.cutoff += (v.aim.cutoff - v.cutoff) * k; v.depth += (v.aim.depth - v.depth) * k;
    v.k1 += (v.aim.k1 - v.k1) * k; v.k2 += (v.aim.k2 - v.k2) * k;
    v.tail += (v.aim.tail - v.tail) * k; v.filter_tail += (v.aim.filter_tail - v.filter_tail) * k;
}

// A kit pad's loop over the voice's region: its point (the start when locked), the fades at a one shot's region
// edges inside the file, and (roadmap P3) its seam's crossfade.
void SamplerEngine::loop_setup(Voice& v, const PadSound& sound) {
    v.loop = sound.loop && v.pad < int(sampler_pads);
    v.loop_at = std::clamp(uint32_t(std::llround(sound.loop_point() * v.scale)), v.start, v.end - 2);
    v.edge_in = !v.loop && v.start > 0;              // the file's own start and end need no fade
    v.edge_out = !v.loop && v.end < v.total;
    v.xfade_kind = 0; v.xfade = 0;
    if (!v.loop || !sound.loop_xfade) return;
    // Its time in the frames it reads, at most half the loop.
    const double read_rate = dsp::model_rate * v.scale;
    const uint32_t span = v.end - v.loop_at;
    const uint32_t want = std::min<uint32_t>(uint32_t(std::lround(sound.loop_xfade / 1000.0 * read_rate)), span / 2);
    const uint32_t before = v.reverse ? v.total - v.end : v.loop_at;  // across the seam on its near side
    const uint32_t after = v.reverse ? v.loop_at : v.total - v.end;   // across it once round
    if (want < 2) return;
    if (before > want) { v.xfade_kind = 1; v.xfade = want; }
    else if (after > want + 1) { v.xfade_kind = 2; v.xfade = want; }
    else { v.xfade_kind = 3; v.xfade = std::max<uint32_t>(2, std::min(want, span / 4)); }
}

// A kit pad's voice takes its pad's region, loop and way as they are now. Where it plays stays where it is: a
// one shot the region no longer holds ahead of it fades out (as a cut does), a loop goes round into its new loop,
// and a start moved past a forward voice (or an end below a reversed one) waits for the next hit. A voice not
// started yet (Offset) starts in the new region.
void SamplerEngine::reach(Voice& v, const PadSound& sound) {
    const uint32_t total = v.total;
    const uint32_t start = std::min(uint32_t(std::llround(sound.start * v.scale)), total - 1);
    uint32_t end = uint32_t(std::llround(sound.end * v.scale));
    end = end && end <= total ? end : total;
    if (end <= start + 1) return;
    const bool reverse = sound.reverse;
    if (v.wait) {
        v.start = start; v.end = end; v.reverse = reverse;
        loop_setup(v, sound);
        v.position = v.place = reverse ? end - 1 : start;
        return;
    }
    // Within a region edge's fade of it counts as reached: the fade would come in a step.
    const double at = v.stretch ? v.place : v.position, edge = slice_edge * v.scale;
    if (!sound.loop && (reverse ? at < start + edge : at >= end - 1 - edge)) { v.choked = true; return; }
    v.reverse = reverse;
    v.start = !reverse && at < start + edge ? std::min(v.start, uint32_t(std::max(0.0, at))) : start;
    const bool above = reverse && at > end - 1 - edge;
    v.end = above && !sound.loop ? std::max(v.end, uint32_t(at) + 2) : end;
    loop_setup(v, sound);
    if (above && v.loop) { // round into the loop from its end, as it would come round
        if (v.stretch) v.place = v.end - 1;
        else v.position = v.end - 1;
    }
}

// A voice stretched from now: Beats' whole segments or Tones' windows, a new one every hop. At its start the first
// tap begins at once; live (a speed or a held length asked while it plays) the sound it played goes on at full, its
// first tap in from its fade, the next taking over as that one would.
void SamplerEngine::begin_stretch(Voice& v, bool live) {
    constexpr uint32_t edge = 88; // stretch()'s segment edges
    v.stretch = true;
    v.beats = v.aim.beats;
    v.place = v.position;
    // Beats: a segment a sixteenth of the project long, cut in at its own place, so hits stay whole.
    // Tones: 60 ms windows, half overlapped, for held sounds.
    v.window = v.beats ? std::max(256u, unsigned(dsp::sample_rate() * 15 / (tempo / 10.0))) : unsigned(0.06f * dsp::sample_rate());
    v.hop = v.beats ? v.window : v.window / 2;
    v.tap_on = {};
    v.since_hop = v.hop; // a tap at once
    if (!live) return;
    const unsigned t = v.next_tap;
    v.next_tap ^= 1;
    v.tap[t] = v.position; v.tap_age[t] = v.beats ? std::min(edge, v.window / 2) : v.window / 2; v.tap_on[t] = true;
    if (v.beats) v.since_hop = v.tap_age[t]; // the next segment as this one ends
}

// A voice hears what its pad and the kit ask now: everything aim() works out, its play mode, and a kit pad's
// region and loop; it glides there. Not while it plays a sample its pad no longer holds (a new sample, layer or
// instrument on the pad: that one waits for its hit), nor Chop's audition.
void SamplerEngine::refresh(Voice& v) {
    const PadSound& own = pads[size_t(v.pad)];
    PadSound layered;
    const PadSound* sound = &own;
    if (v.layer > 0) { layered = layer_sound(own, unsigned(v.layer)); sound = &layered; }
    if (!sound->used() || sound->sample != v.slot) return;
    const SampleZone* zone = nullptr;
    Multisample* multi = nullptr;
    int key = v.key;
    if (v.zone) {
        multi = multi_of(*sound);
        if (!multi || multi != v.multi || v.zone_index >= multi->zones.size()) return;
        zone = &multi->zones[v.zone_index];
        key = std::clamp(60 + int(sound->pitch) + v.semis, 0, 127);
    } else if (!v.cloud) {
        reach(v, *sound);
        if (v.choked) return;
    }
    // Its play mode: a gate already let go ends now, as it would have; a one shot plays on.
    if (!(zone && zone->on_release)) v.gate = sound->play == PadPlay::Gate && !(zone && zone->one_shot);
    if (v.gate && !v.held && v.env.stage != dsp::Envelope::Stage::Release) { v.env.gate(false); v.filter_env.gate(false); }
    const PadFilter was = v.filter;
    aim(v, *sound, zone, multi, key);
    if (was == PadFilter::Off && v.filter != PadFilter::Off) { // a filter put on: from its own values, its stages empty
        std::memset(v.fs, 0, sizeof(v.fs));
        v.cutoff = v.aim.cutoff; v.depth = v.aim.depth; v.k1 = v.aim.k1; v.k2 = v.aim.k2; v.filter_tail = v.aim.filter_tail;
        v.since_tune = 1u << 30;
    }
    // A held sustain moved: the envelope goes there at its decay, as an analogue one does.
    if (v.env.stage == dsp::Envelope::Stage::Sustain && std::fabs(v.env.level - v.shape.sustain) > 1e-4f) v.env.stage = dsp::Envelope::Stage::Decay;
    if (v.filter_env.stage == dsp::Envelope::Stage::Sustain && std::fabs(v.filter_env.level - v.filter_shape.sustain) > 1e-4f)
        v.filter_env.stage = dsp::Envelope::Stage::Decay;
    if (!v.stretch && !v.cloud && !(zone && zone->on_release) && std::fabs(v.aim.step - v.aim.advance) > 1e-6) begin_stretch(v, true);
    if (v.cloud) settle(v);
    else v.gliding = glide_frames;
}

// The voices whose pad or kit changed since the last render hear it now; the tempo moves those that follow it.
void SamplerEngine::retune_voices() {
    const bool any = retune_all || tempo_moved || std::any_of(retune.begin(), retune.end(), [](uint64_t bits) { return bits != 0; });
    if (!any) return;
    for (auto& v : voices)
        if (v.pad >= 0 && v.pad < int(sampler_pads) && !v.choked && (marked(v.pad) || (tempo_moved && v.follows))) refresh(v);
    retune.fill(0);
    retune_all = tempo_moved = false;
}

Multisample* SamplerEngine::multi_of(const PadSound& sound) const {
    if (!samples || !sound.used() || sound.sample > preview_slot) return nullptr;
    const SampleData* data = (*samples)[sound.sample];
    return data ? data->multi.get() : nullptr;
}

// A press of an instrument: which zones it reaches, each counted; what they cut;
// then a voice on each. As a pad, it may be cut, muted and linked as any other.
void SamplerEngine::zones_on(Multisample& multi, const PadSound& sound, int pad, int velocity, int semitones, int cutoff, int trigger) {
    const int key = std::clamp(60 + int(sound.pitch) + semitones, 0, 127);
    const int vel = std::clamp(velocity, 1, 127);
    const int owner = trigger >= 0 ? trigger : pad;
    // Roadmap P3: a keyswitch picks the zones that play from now on and sounds nothing of itself (nor cuts what sounds).
    if (multi.switch_low >= 0 && key >= multi.switch_low && key <= multi.switch_high) { multi.switched = int16_t(key); return; }
    for (auto& v : voices)
        if (v.zone && v.pad == pad && v.trigger == owner && v.key == key && v.held) { v.held = false; if (v.gate) { v.env.gate(false); v.filter_env.gate(false); } }
    if (sound.mono) for (auto& v : voices) if (v.pad == pad) v.choked = true;
    const int articulation = multi.switched >= 0 ? multi.switched : multi.switch_default;
    key_velocity[size_t(key)] = uint8_t(vel); key_since[size_t(key)] = played_frames; // for its release samples
    seed = seed * 1664525u + 1013904223u;
    const float dice = 0.5f * (int32_t(seed) / 2147483648.0f + 1.0f); // one roll for a press
    std::array<uint16_t, max_voices> reached;
    unsigned count = 0;
    for (size_t n = 0; n < multi.zones.size(); ++n) {
        const auto& z = multi.zones[n];
        if (key < z.low_key || key > z.high_key || vel < z.low_velocity || vel > z.high_velocity) continue;
        if (z.on_release || (z.switch_key >= 0 && z.switch_key != articulation) || zone_crossfade(z, key, vel) <= 0) continue;
        uint32_t turn = multi.turns[n]++;
        if (z.sequence_length > 1 && turn % z.sequence_length != z.sequence - 1) continue;
        if (dice < z.low_random || (dice >= z.high_random && z.high_random < 1.0f)) continue;
        if (!multi.parts[z.part].frames.load(std::memory_order_acquire)) continue; // silent until its file lands
        if (count < max_voices) reached[count++] = uint16_t(n);
    }
    for (unsigned r = 0; r < count; ++r) {
        uint32_t group = multi.zones[reached[r]].group;
        if (group) for (auto& v : voices) if (v.pad >= 0 && v.off_by == group) v.choked = true;
    }
    for (unsigned r = 0; r < count; ++r) start(free_voice(), sound, pad, velocity, semitones, cutoff, trigger, &multi.zones[reached[r]], &multi, key);
}

// A press of a pad with layers (roadmap P3): every layer its velocity reaches, the next in turn, or one at random;
// a mono pad's last hit at that pitch is cut once for all of them, so its layers never cut each other.
void SamplerEngine::layers_on(const PadSound& sound, int pad, int velocity, int semitones, int cutoff, int trigger) {
    std::array<uint8_t, pad_layers> used {};
    unsigned count = 0;
    for (unsigned n = 0; n < pad_layers; ++n) {
        const PadLayer l = sound.layer(n);
        if (!l.used() || l.sample > preview_slot || !(*samples)[l.sample] || (*samples)[l.sample]->multi) continue; // a layer is a plain sample
        used[count++] = uint8_t(n);
    }
    if (!count) return;
    std::array<uint8_t, pad_layers> picked {};
    unsigned chosen = 0;
    switch (sound.layer_play) {
    case LayerPlay::Cycle: {
        auto& turn = layer_turn[size_t(pad) % sampler_pads];
        picked[chosen++] = used[turn % count];
        turn = uint8_t((turn + 1) % count);
        break;
    }
    case LayerPlay::Random:
        seed = seed * 1664525u + 1013904223u;
        picked[chosen++] = used[(seed >> 16) % count];
        break;
    default: {
        const int vel = std::clamp(velocity, 1, 127);
        for (unsigned k = 0; k < count; ++k) {
            const PadLayer l = sound.layer(used[k]);
            if (vel >= l.low && vel <= l.high) picked[chosen++] = used[k];
        }
        break;
    }
    }
    if (sound.mono) for (auto& v : voices) if (v.pad == pad && v.semis == semitones) v.choked = true;
    for (unsigned k = 0; k < chosen; ++k) {
        PadSound one = layer_sound(sound, picked[k]);
        one.mono = false; // cut above, once
        Voice& v = free_voice();
        start(v, one, pad, velocity, semitones, cutoff, trigger);
        if (v.pad >= 0) v.layer = int8_t(picked[k]);
    }
}

void SamplerEngine::play(const PadSound& sound, int pad, int velocity, int semitones, int cutoff, int trigger) {
    if (auto* multi = multi_of(sound)) zones_on(*multi, sound, pad, velocity, semitones, cutoff, trigger);
    else if (sound.layered() && samples) layers_on(sound, pad, velocity, semitones, cutoff, trigger);
    else start(free_voice(), sound, pad, velocity, semitones, cutoff, trigger);
}

// A kit pad hit: a one shot that loops stops instead when it is playing;
// its linked pad (same bank) plays with it, and that pad's linked pad, and so
// on through the bank (roadmap P3), each pad once: a chain that comes back
// round to a pad already played stops there.
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
    play(sound, pad, velocity, semitones, cutoff, -1);
    uint32_t played = 1u << (pad % 16); // a bit a pad of the bank
    for (int at = pad; pads[size_t(at)].link;) {
        const int linked = pad / 16 * 16 + (pads[size_t(at)].link - 1) % 16;
        if (played & (1u << (linked % 16))) break; // round to a pad already played
        played |= 1u << (linked % 16);
        if (!pad_muted(unsigned(linked))) play(pads[size_t(linked)], linked, velocity, 0, -1, pad); // a muted pad is silent but links on
        at = linked;
    }
}

// A note is a pad's key, and over it the semitones from the pad's root it plays at (an instrument's), and over those a
// 16 Levels cutoff a pattern kept (roadmap P3: model/sampler.h pad_note).
void SamplerEngine::note_on(int key, int velocity) {
    if (pedal) { // pressed again under the pedal: its earlier off is not owed any more
        unsigned kept = 0;
        for (unsigned n = 0; n < pedal_count; ++n) if ((pedal_offs[n] & 0xffff) != (key & 0xffff)) pedal_offs[kept++] = pedal_offs[n];
        pedal_count = kept;
    }
    const int cutoff = (key >> 16) & 0xff;
    hit((key & 0xff) - int(sampler_first_note), velocity, int8_t(key >> 8), cutoff ? std::min(cutoff - 1, 100) : -1);
}

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

void SamplerEngine::set_bend(float amount) {
    const float semitones = std::clamp(amount, -1.0f, 1.0f) * bend_range;
    if (semitones == bend) return;
    bend = semitones;
    retune_all = true;
}

void SamplerEngine::control(int cc, int value) {
    if (cc != 64) return;
    const bool down = value >= 64;
    if (down == pedal) return;
    pedal = down;
    if (down) return;
    const unsigned count = pedal_count;
    pedal_count = 0;
    for (unsigned n = 0; n < count; ++n) note_off(pedal_offs[n]);
}

void SamplerEngine::note_off(int key) {
    if (pedal) { // held on by the sustain pedal; the oldest goes now when too many wait
        for (unsigned n = 0; n < pedal_count; ++n) if (pedal_offs[n] == key) return;
        if (pedal_count < pedal_offs.size()) { pedal_offs[pedal_count++] = key; return; }
        const int oldest = pedal_offs[0];
        std::copy(pedal_offs.begin() + 1, pedal_offs.end(), pedal_offs.begin());
        pedal_offs.back() = key;
        key = oldest;
    }
    int pad = (key & 0xff) - int(sampler_first_note), semis = int8_t(key >> 8);
    for (auto& v : voices)
        if (v.trigger == pad && v.pad >= 0 && v.held && (!v.zone || v.semis == semis)) { // a one shot plays on to its end (a loop until hit again)
            v.held = false;
            if (v.gate) { v.env.gate(false); v.filter_env.gate(false); }
        }
    release_zones(pad, semis);
}

// Roadmap P3 (SFZ trigger=release): the key of an instrument let go plays the zones that sound on a release, at its
// press's velocity, rt_decay dB quieter for each second it was held. Not for a key never pressed, nor a muted pad.
void SamplerEngine::release_zones(int pad, int semitones) {
    if (pad < 0 || pad >= int(sampler_pads) || pad_muted(unsigned(pad))) return;
    const auto& sound = pads[size_t(pad)];
    Multisample* multi = multi_of(sound);
    if (!multi || !multi->has_release) return;
    const int key = std::clamp(60 + int(sound.pitch) + semitones, 0, 127);
    const int vel = key_velocity[size_t(key)];
    if (!vel) return;
    key_velocity[size_t(key)] = 0;
    const float held = float(double(played_frames - key_since[size_t(key)]) / dsp::sample_rate());
    const int articulation = multi->switched >= 0 ? multi->switched : multi->switch_default;
    seed = seed * 1664525u + 1013904223u;
    const float dice = 0.5f * (int32_t(seed) / 2147483648.0f + 1.0f);
    for (size_t n = 0; n < multi->zones.size(); ++n) {
        const auto& z = multi->zones[n];
        if (!z.on_release || key < z.low_key || key > z.high_key || vel < z.low_velocity || vel > z.high_velocity) continue;
        if ((z.switch_key >= 0 && z.switch_key != articulation) || zone_crossfade(z, key, vel) <= 0) continue;
        uint32_t turn = multi->turns[n]++;
        if (z.sequence_length > 1 && turn % z.sequence_length != z.sequence - 1) continue;
        if (dice < z.low_random || (dice >= z.high_random && z.high_random < 1.0f)) continue;
        if (!multi->parts[z.part].frames.load(std::memory_order_acquire)) continue;
        Voice& v = free_voice();
        start(v, sound, pad, vel, semitones, -1, pad, &z, multi, key);
        if (v.pad >= 0 && z.rt_decay > 0) { v.fall = std::pow(10.0f, -z.rt_decay * held / 20.0f); v.gain *= v.fall; v.aim.gain *= v.fall; }
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
    for (auto& v : voices) if (v.slot == slot) { end_stream(v); v = Voice {}; }
    if (samples && slot <= preview_slot)
        for (auto& g : grains) if (g.sample == (*samples)[slot]) g.sample = nullptr;
}

void SamplerEngine::all_notes_off() {
    pedal = false; pedal_count = 0; // as a plug-in's: the pedal up too
    for (auto& v : voices) if (v.pad >= 0) { v.held = false; v.env.gate(false); v.filter_env.gate(false); }
}
void SamplerEngine::silence() {
    pedal = false; pedal_count = 0;
    for (auto& v : voices) { end_stream(v); v = Voice {}; }
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
    double span = v.zone ? std::min(v.end - v.start, stream_chunk) : v.end - v.start; // an instrument's clouds stay in what is held
    const PadSound& own = pads[size_t(v.pad) % sampler_pads]; // the pad's cloud, as it is now (Motion moves it)
    double at = v.start + span * (own.cloud_position / 100.0) + v.scan + random * span * 0.05;
    at = std::fmod(std::fmod(at - v.start, span) + span, span) + v.start;
    seed = seed * 1664525u + 1013904223u;
    float detune = int32_t(seed) / 2147483648.0f * own.cloud_spread / 100.0f; // up to a semitone either way per 100 cents
    double step = v.step * std::exp2(detune / 12.0);
    uint32_t length = uint32_t(0.01f * std::pow(100.0f, own.cloud_size / 100.0f) * dsp::sample_rate());
    // A grain never runs off its region's end (and round to its start: a jump in the sound). Near the end it is
    // cut short, its window with it; with hardly any room left it starts early enough to fit.
    const double top = v.start + span, fit = (top - 1 - at) / step;
    if (fit < std::min(double(length), 64.0)) at = std::max(double(v.start), top - 1 - double(length) * step);
    else length = uint32_t(std::min(double(length), fit));
    *g = Grain {v.sample, at, step, 0, std::max(length, 1u), v.gain * v.env.level, v.left, v.right,
                v.l, v.r, v.start, uint32_t(top), ++grain_serial, at, int16_t(v.pad), uint16_t(v.slot), v.scale};
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
float SamplerEngine::edge_gain(const Voice& v, double at) {
    double g = 1;
    if (v.edge_in) g = std::min(g, (at - v.start) / (slice_edge * v.scale)); // 2 ms at the frames it reads
    if (v.edge_out) g = std::min(g, ((v.end - 1) - at) / (slice_edge * v.scale));
    return float(std::clamp(g, 0.0, 1.0));
}

// Roadmap P3: a kit pad loop's frame at `at` near its seam, crossfaded (linear, so a loop already seamless stays
// level) with the frame a loop's length away across the seam: before the loop point as the end nears (1), or past
// the end just after the turn (2); with nothing across it in the file, a dip down and up again (3).
void SamplerEngine::seam(const Voice& v, const float* l, const float* r, double at, float& out_l, float& out_r) {
    const double span = double(v.end - v.loop_at), x = double(v.xfade);
    auto other = [&](double from, float& ol, float& orr) { // another place in the file, read with its fraction
        unsigned i = unsigned(std::max(0.0, from));
        unsigned j = std::min(i + 1, v.total - 1);
        float frac = float(from - i);
        ol = l[i] + (l[j] - l[i]) * frac;
        orr = r[i] + (r[j] - r[i]) * frac;
    };
    // How far into the fade it is, 0..1 (-1: outside it), and where across the seam it reads.
    double into = -1, across = 0;
    const bool back = v.reverse;
    switch (v.xfade_kind) {
    case 1:
        if (!back && at >= v.end - x) { into = (at - (v.end - x)) / x; across = at - span; }
        if (back && at < v.loop_at + x) { into = ((v.loop_at + x) - at) / x; across = at + span; }
        break;
    case 2:
        if (!v.turned) break;
        if (!back && at < v.loop_at + x) { into = 1 - (at - v.loop_at) / x; across = at + span; }
        if (back && at >= v.end - x) { into = 1 - ((v.end - 1) - at) / x; across = at - span; }
        break;
    case 3: { // a dip: down to nothing at the seam and up again after the turn
        double gain = 1;
        if (!back) {
            gain = std::min(gain, (v.end - at) / x);
            if (v.turned) gain = std::min(gain, (at - v.loop_at) / x);
        } else {
            gain = std::min(gain, (at - v.loop_at) / x);
            if (v.turned) gain = std::min(gain, (v.end - at) / x);
        }
        gain = std::clamp(gain, 0.0, 1.0);
        out_l *= float(gain); out_r *= float(gain);
        return;
    }
    default: return;
    }
    if (into < 0) return;
    float w = float(std::clamp(into, 0.0, 1.0)), ol = 0, orr = 0;
    other(across, ol, orr);
    out_l += (ol - out_l) * w;
    out_r += (orr - out_r) * w;
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

bool SamplerEngine::plain_run(const Voice& v, unsigned frames) const {
    if (v.gliding || v.grow || v.choked || v.xfade_kind) return false;
    // Everywhere it can read this run, with a frame to spare.
    const double travel = double(frames + 1) * v.step;
    const double lo = v.reverse ? v.position - travel : v.position, hi = v.reverse ? v.position : v.position + travel;
    const double edge = slice_edge * v.scale;
    if (v.edge_in && lo - v.start < edge) return false;           // edge_gain() is exactly 1 from here
    if (v.edge_out && (v.end - 1) - hi < edge) return false;
    if (!v.loop && (v.tail > 0 || v.filter_tail > 0)) {            // voice_out()'s fades to the end not begun
        const double distance = v.reverse ? lo - v.start : (v.end - 1) - hi;
        if (distance / std::max(v.step, 1e-6) < double(std::max(v.tail, v.filter_tail)) + 2) return false;
    }
    return true;
}

// The full loop below with what plain_run() ruled out taken away; the same arithmetic in the same order.
void SamplerEngine::run_plain(Voice& v, const float* l, const float* r, float* to_l, float* to_r, unsigned from, unsigned frames, double span) {
    const bool filtered = v.filter != PadFilter::Off;
    const bool loop = v.loop, reverse = v.reverse;
    const double step = v.step;
    const float gain = v.gain, fade = v.fade, pan_l = v.left, pan_r = v.right;
    double position = v.position;
    dsp::Envelope env = v.env;
    for (unsigned f = from; f < frames; ++f) {
        if (loop) {
            if (reverse) while (position < v.loop_at) { position += span; v.wrapped = v.turned = true; }
            else while (position >= v.end) { position -= span; v.wrapped = v.turned = true; }
        } else if (reverse ? position < v.start : position >= v.end - 1) { v.pad = -1; break; }
        unsigned i = unsigned(position);
        float frac = float(position - i);
        unsigned j = i + 1 < v.end ? i + 1 : loop ? v.loop_at : v.end - 1;
        float a = env.next(v.shape) * gain * fade;
        float sl = l[i] + (l[j] - l[i]) * frac, sr = r[i] + (r[j] - r[i]) * frac;
        if (filtered) voice_out(v, sl, sr, a, 1e9f, to_l, to_r, f); // clear of its tail (plain_run())
        else {
            to_l[f] += sl * a * pan_l;
            to_r[f] += sr * a * pan_r;
        }
        position += reverse ? -step : step;
        if (!env.active()) { v.pad = -1; break; }
    }
    v.position = position;
    v.env = env;
}

// A stretched voice: taps begin where `place` has got to and read at the
// pad's pitch; Tones fades each over its window (sin², half overlapped, so
// they sum to one), Beats plays each segment whole with 2 ms edges.
void SamplerEngine::stretch(Voice& v, const float* l, const float* r, float* to_l, float* to_r, unsigned from, unsigned frames) {
    const double way = v.reverse ? -1.0 : 1.0;
    constexpr uint32_t edge = 88;
    // A kit pad's loop goes round from its loop point to its end with the fraction kept, its taps too, so it
    // neither drifts out of tune nor cuts a tap off at the turn.
    const bool pad_loop = !v.zone && v.loop;
    const double span = double(v.end - v.loop_at);
    // Tones' windows (sin², always 60 ms) from the engine's table, worked out with the same expression once a rate.
    const float* tones = !v.beats && plain_runs ? tones_table(v.window) : nullptr;
    if (tones) ++plain_taken;
    // Clear of its tail's fades for the whole run: frames_left() need not be worked out frame by frame.
    bool tail_far = !(!v.loop && (v.tail > 0 || v.filter_tail > 0));
    if (!tail_far && plain_runs && !v.gliding) {
        const double travel = double(frames - from + 1) * v.advance;
        const double distance = v.reverse ? (v.place - travel) - v.start : (v.end - 1) - (v.place + travel);
        tail_far = distance / std::max(v.advance, 1e-6) >= double(std::max(v.tail, v.filter_tail)) + 2;
    }
    for (unsigned f = from; f < frames; ++f) {
        const bool round = v.zone && v.loop && (!v.loop_held || v.held);
        if (pad_loop) {
            bool turned = false;
            if (v.reverse) while (v.place < v.loop_at) { v.place += span; turned = true; }
            else while (v.place >= v.end) { v.place -= span; turned = true; }
            if (turned) {
                v.wrapped = true; v.turned = true;
                if (v.beats) { // a fresh segment from the loop point; the one playing fades out over its edge meanwhile
                    unsigned playing = v.next_tap ^ 1;
                    if (v.tap_on[playing] && v.window > edge) v.tap_age[playing] = std::max(v.tap_age[playing], v.window - edge);
                    v.since_hop = v.hop;
                }
            }
        } else if (v.reverse ? v.place < v.start : v.place >= (round ? v.loop_hi : v.end - 1)) {
            if (!round) { v.pad = -1; return; }
            v.place = v.loop_at;
            v.wrapped = true;
            v.since_hop = v.hop; // a fresh tap from the loop point
            came_round(v);
        }
        if (v.since_hop >= v.hop) { // a new tap from here
            v.since_hop = 0;
            unsigned t = v.next_tap; v.next_tap ^= 1;
            v.tap[t] = v.place; v.tap_age[t] = 0; v.tap_on[t] = true;
        }
        ++v.since_hop;
        glide(v);
        float a = v.env.next(v.shape) * v.gain * v.fade;
        if (v.zone || v.grow) { v.duck += std::clamp(v.duck_to - v.duck, -edge_step, edge_step); a *= v.duck; }
        if (v.choked) { v.fade -= edge_step; if (v.fade <= 0) { v.pad = -1; return; } }
        float sum_l = 0, sum_r = 0;
        for (unsigned t = 0; t < 2; ++t) {
            if (!v.tap_on[t]) continue;
            double at = v.tap[t];
            if (pad_loop) { // round the loop, as the place goes
                if (v.reverse) while (at < v.loop_at) at += span;
                else while (at >= v.end) at -= span;
                v.tap[t] = at;
            }
            if (at < v.start || (!pad_loop && at >= v.end - 1) || v.tap_age[t] >= v.window) { v.tap_on[t] = false; continue; }
            float w;
            if (v.beats) {
                uint32_t age = v.tap_age[t], left = v.window - age;
                w = age < edge ? float(age) / edge : left < edge ? float(left) / edge : 1.0f;
            } else if (tones) {
                w = tones[v.tap_age[t]];
            } else {
                float s = std::sin(dsp::pi * float(v.tap_age[t]) / float(v.window));
                w = s * s;
            }
            if (v.edge_in || v.edge_out) w *= edge_gain(v, at); // a tap reaching the region's edge fades too
            unsigned i = unsigned(at);
            float frac = float(at - i);
            unsigned j = i + 1 < v.end ? i + 1 : pad_loop ? v.loop_at : v.end - 1; // a loop's seam read across
            float tl = l[i] + (l[j] - l[i]) * frac, tr = r[i] + (r[j] - r[i]) * frac;
            if (pad_loop && v.xfade_kind) seam(v, l, r, at, tl, tr);
            sum_l += tl * w;
            sum_r += tr * w;
            v.tap[t] += way * v.step;
            ++v.tap_age[t];
        }
        voice_out(v, sum_l, sum_r, a, tail_far ? 1e9f : frames_left(v, v.place, v.advance), to_l, to_r, f);
        v.place += way * v.advance;
        if (!v.env.active()) { v.pad = -1; return; }
    }
}

// Tones' sin² window of `window` frames, a weight a tap age, as stretch() would work each out (nullptr: too long for
// the table). Built the first time a window of that length plays: one window's worth of sin(), once a rate.
const float* SamplerEngine::tones_table(uint32_t window) {
    if (window > tones_weights.size()) return nullptr;
    if (tones_window != window) {
        for (uint32_t age = 0; age < window; ++age) {
            float s = std::sin(dsp::pi * float(age) / float(window));
            tones_weights[age] = s * s;
        }
        tones_window = window;
    }
    return tones_weights.data();
}

// A zone's loop went round: a loop held whole needs no stream any more; a longer one starts a new run.
void SamplerEngine::came_round(Voice& v) {
    v.wrapped = true;
    if (v.loop_hi <= v.loop_at + stream_chunk) { end_stream(v); v.capped = false; return; }
    if (v.stream >= 0) pager->restart(v.stream, v.loop_at);
    else v.head_end = v.loop_at + stream_chunk;
}

// Whether a streamed voice may read its next block. Short of what it needs it
// ducks out over this block; with none safe it holds its place, silent, and
// comes back when the pager catches up: a gap on the audio, never a wait on the card.
bool SamplerEngine::admit(Voice& v, unsigned frames) {
    if (v.stream < 0 && !v.capped) { v.duck_to = 1; v.played = true; return true; }
    const double at = v.stretch ? v.place : v.position;
    const double speed = v.stretch ? std::max(v.advance, v.step) : v.step;
    const double extra = (v.stretch ? v.window * v.step : 0.0) + 2.0;
    const double one = frames * speed + extra, two = 2.0 * frames * speed + extra;
    const uint32_t limit = v.stream >= 0 ? pager->limit(v.stream) : v.head_end;
    bool enough = false, plenty = false;
    if (v.reverse) {
        const bool all = limit != UINT32_MAX && limit <= v.start; // pinned down to where it ends
        enough = all || (limit != UINT32_MAX && at - one >= limit);
        plenty = all || (limit != UINT32_MAX && at - two >= limit);
    } else {
        const bool round = v.loop && (!v.loop_held || v.held);
        const bool all = limit >= (round ? v.loop_hi : v.end);
        enough = all || at + one < limit;
        plenty = all || at + two < limit;
    }
    if (!enough) {
        if (v.capped || (v.stream >= 0 && pager->dead(v.stream))) { v.pad = -1; return false; } // no more to come
        if (v.played && !v.starved && pager) pager->underrun();
        v.starved = true;
        v.duck = 0;
        for (unsigned f = 0; f < frames; ++f) { v.env.next(v.shape); if (v.filter != PadFilter::Off) v.filter_env.next(v.filter_shape); }
        if (!v.env.active() || v.choked) v.pad = -1;
        return false;
    }
    v.starved = false; v.played = true;
    v.duck_to = plenty ? 1.0f : 0.0f;
    return true;
}

// A sample still being read (SampleGrowth: the browser's preview), as admit() a stream: it reads only what has landed,
// ducking out short of that and holding its place, silent, until the reading catches up. Reversed, looped or with a
// crossfaded seam it reads near its end too, so it waits for all of it. Once the reading is cut short (the file ran
// out early, or it is no longer wanted), it plays up to there and ends.
bool SamplerEngine::admit_grown(Voice& v, unsigned frames) {
    const bool cut = v.grow->cut.load(std::memory_order_acquire); // first: what it reads below is then all that comes
    const uint32_t limit = (v.native ? v.grow->play_frames : v.grow->frames).load(std::memory_order_acquire);
    bool enough = limit >= v.end, plenty = enough;
    if (!enough && !v.reverse && !v.loop && !v.xfade_kind) {
        const double at = v.stretch ? v.place : v.position;
        const double speed = v.stretch ? std::max(v.advance, v.step) : v.step;
        const double extra = (v.stretch ? v.window * v.step : 0.0) + 2.0;
        enough = at + frames * speed + extra < limit;
        plenty = at + 2.0 * frames * speed + extra < limit;
    }
    if (!enough) {
        if (cut) { v.pad = -1; return false; } // no more to come
        v.duck = 0;
        for (unsigned f = 0; f < frames; ++f) { v.env.next(v.shape); if (v.filter != PadFilter::Off) v.filter_env.next(v.filter_shape); }
        if (!v.env.active() || v.choked) v.pad = -1;
        return false;
    }
    v.duck_to = plenty ? 1.0f : 0.0f;
    return true;
}

// A zone voice's next `frames` with nothing to check frame by frame: no glide or choke, its duck all the way up, and
// clear of its tail's fades. (A zone has no edge fades or seam.)
bool SamplerEngine::zone_plain(const Voice& v, unsigned frames) const {
    if (v.gliding || v.choked || v.duck != 1.0f || v.duck_to != 1.0f) return false;
    if (!v.loop && (v.tail > 0 || v.filter_tail > 0)) { // voice_out()'s fades to the end not begun
        const double travel = double(frames + 1) * v.step;
        const double distance = v.reverse ? (v.position - travel) - v.start : (v.end - 1) - (v.position + travel);
        if (distance / std::max(v.step, 1e-6) < double(std::max(v.tail, v.filter_tail)) + 2) return false;
    }
    return true;
}

// play_zone()'s loop with what zone_plain() ruled out taken away; the same arithmetic in the same order.
void SamplerEngine::zone_run_plain(Voice& v, const float* l, const float* r, float* to_l, float* to_r, unsigned from, unsigned frames) {
    const bool filtered = v.filter != PadFilter::Off, reverse = v.reverse;
    const bool round = v.loop && (!v.loop_held || v.held);
    const double step = v.step;
    const float gain = v.gain, fade = v.fade, pan_l = v.left, pan_r = v.right;
    dsp::Envelope env = v.env;
    for (unsigned f = from; f < frames; ++f) {
        if (round && v.position >= v.loop_hi) {
            while (v.position >= v.loop_hi) v.position -= double(v.loop_hi - v.loop_at);
            came_round(v);
        }
        if (!round && (reverse ? v.position < v.start : v.position >= v.end - 1)) { v.pad = -1; break; }
        unsigned i = unsigned(v.position);
        float frac = float(v.position - i);
        unsigned j = i + 1;
        if (round && j >= v.loop_hi) j = v.loop_at;
        else j = std::min(j, v.end - 1);
        float a = env.next(v.shape) * gain * fade;
        float sl = l[i] + (l[j] - l[i]) * frac, sr = r[i] + (r[j] - r[i]) * frac;
        if (filtered) voice_out(v, sl, sr, a, 1e9f, to_l, to_r, f); // clear of its tail (zone_plain())
        else {
            to_l[f] += sl * a * pan_l;
            to_r[f] += sr * a * pan_r;
        }
        v.position += reverse ? -step : step;
        if (!env.active()) { v.pad = -1; break; }
    }
    v.env = env;
}

// A zone's frames: as a pad's, but round a loop of its own, the fraction kept
// and the seam read across, so a loop in tune at any pitch does not click.
void SamplerEngine::play_zone(Voice& v, float* to_l, float* to_r, unsigned from, unsigned frames) {
    const float* l = v.l;
    const float* r = v.r;
    if (plain_runs && zone_plain(v, frames - from)) { // nothing this run needs looking at frame by frame: the short loop
        zone_run_plain(v, l, r, to_l, to_r, from, frames);
        ++plain_taken;
        return;
    }
    for (unsigned f = from; f < frames; ++f) {
        const bool round = v.loop && (!v.loop_held || v.held);
        if (round && v.position >= v.loop_hi) {
            while (v.position >= v.loop_hi) v.position -= double(v.loop_hi - v.loop_at);
            came_round(v);
        }
        if (!round && (v.reverse ? v.position < v.start : v.position >= v.end - 1)) { v.pad = -1; return; } // a loop may run to the file's very end
        unsigned i = unsigned(v.position);
        float frac = float(v.position - i);
        unsigned j = i + 1;
        if (round && j >= v.loop_hi) j = v.loop_at;
        else j = std::min(j, v.end - 1);
        glide(v);
        float a = v.env.next(v.shape) * v.gain * v.fade;
        v.duck += std::clamp(v.duck_to - v.duck, -edge_step, edge_step);
        a *= v.duck;
        if (v.choked) { v.fade -= edge_step; if (v.fade <= 0) { v.pad = -1; return; } }
        voice_out(v, l[i] + (l[j] - l[i]) * frac, r[i] + (r[j] - r[i]) * frac, a, frames_left(v, v.position, v.step), to_l, to_r, f);
        v.position += v.reverse ? -v.step : v.step;
        if (!v.env.active()) { v.pad = -1; return; }
    }
}

bool SamplerEngine::sends_heard(int pad) const {
    if (pad < 0 || pad >= int(sampler_pads)) return false;
    if (pads[size_t(pad)].sending()) return true;
    for (float g : send_now[size_t(pad)]) if (g > 0) return true;
    return false;
}

SamplerEngine::Lane* SamplerEngine::lane_for(int pad) {
    Lane* free = nullptr;
    for (auto& l : *lane) {
        if (l.pad == pad) return &l;
        if (l.pad < 0 && !free) free = &l;
    }
    if (free) {
        free->pad = pad; free->fed = false; free->quiet = free->ringing = 0;
        std::fill(std::begin(free->l), std::end(free->l), 0.0f); std::fill(std::begin(free->r), std::end(free->r), 0.0f);
    }
    return free;
}

// The overload guard (Engine::relieve): a pad already let go (its envelope releasing) fades out at once, as a choke
// does; with `steal`, the oldest voice too.
bool SamplerEngine::relieve(bool steal) {
    Voice* oldest = nullptr;
    for (auto& v : voices) {
        if (v.pad < 0 || v.choked) continue;
        if (v.env.stage == dsp::Envelope::Stage::Release) { v.choked = true; continue; }
        if (!oldest || v.age < oldest->age) oldest = &v;
    }
    if (!steal || !oldest) return false;
    oldest->choked = true;
    return true;
}

void SamplerEngine::render_stereo(float* left, float* right, unsigned frames) {
    edge_step = (1.0f / 64) * dsp::model_frames();
    played_frames += frames;
    glide_setup();
    retune_voices();
    // Clouds: their voices only place grains; the grains play.
    for (auto& v : voices) {
        if (v.pad < 0 || !v.cloud) continue;
        for (unsigned f = 0; f < frames; ++f) v.env.next(v.shape);
        if (v.life > 0 && (v.life -= frames) <= 0) { v.env.gate(false); v.filter_env.gate(false); }
        v.sounded += frames;
        if (!v.env.active() || (v.choked && (v.fade -= 0.25f) <= 0)) { v.pad = -1; continue; }
        const PadSound& own = pads[size_t(v.pad) % sampler_pads];
        v.scan += (own.cloud_scan - 50) / 50.0 * v.step * frames; // forwards or backwards at up to its own speed
        v.next_grain -= frames;
        double every = dsp::sample_rate() / std::max(1, int(1 + own.cloud_density * 0.99));
        while (v.next_grain <= 0) { spawn(v); v.next_grain += every; }
    }
    for (auto& g : grains) {
        if (!g.sample) continue;
        const float* l = g.l;
        const float* r = g.r;
        const double span = double(g.top - g.low);
        for (unsigned f = 0; f < frames && g.age < g.length; ++f, ++g.age) {
            // Only a region shorter than the grain gets this far: round inside it.
            if (g.position >= g.top) g.position -= span * std::floor((g.position - g.low) / span);
            unsigned i = unsigned(g.position);
            float frac = float(g.position - i);
            unsigned j = i + 1 < g.top ? i + 1 : g.low;
            float w = std::sin(dsp::pi * float(g.age) / float(g.length)) * g.gain * 0.5f; // a smooth window
            left[f] += (l[i] + (l[j] - l[i]) * frac) * w * g.left; right[f] += (r[i] + (r[j] - r[i]) * frac) * w * g.right;
            g.position += g.step;
        }
        if (g.age >= g.length) g.sample = nullptr;
    }
    // Each pad with a chain or sends of its own renders on its own lane (blocks of 128 at most here).
    bool laned = frames <= 128;
    bool sending = false;
    for (unsigned b = 0; b < send_buses; ++b) sending |= send_l[b] && send_r[b];
    send_used = 0;
    auto on_lane = [&](int pad) { return laned && pad < int(sampler_pads) && (chained(pad) || (sending && sends_heard(pad))); };
    bool knobbing = (knob_mask[0] || knob_mask[1]) && knob_l && knob_r && frames <= 128;
    if (laned) for (auto& l : *lane) if (l.pad >= 0) { l.fed = false; std::fill(l.l, l.l + frames, 0.0f); std::fill(l.r, l.r + frames, 0.0f); }
    knob_used = false;
    // Where a pad's sound joins: the kit, the Sidechain's key (first: it must not duck itself), or Knob FX's own output.
    bool keying = key_pad >= 0 && frames <= 128;
    auto out_l = [&](int pad) {
        if (keying && pad == key_pad) return key_l;
        if (knobbing && knobbed(pad)) { knob_used = true; return knob_l; }
        return left;
    };
    auto out_r = [&](int pad) { return keying && pad == key_pad ? key_r : knobbing && knobbed(pad) ? knob_r : right; };
    for (auto& v : voices) {
        if (v.pad < 0 || v.cloud) continue;
        v.wrapped = false;
        float* to_l = out_l(v.pad);
        float* to_r = out_r(v.pad);
        if (on_lane(v.pad))
            if (Lane* l = lane_for(v.pad)) {
                l->fed = true;
                to_l = l->l; to_r = l->r;
            }
        const float* l = v.l;
        const float* r = v.r;
        unsigned from = 0;
        if (v.wait) { // Offset: it starts later in this block, or in a later one
            from = std::min(v.wait, frames);
            v.wait -= from;
            if (v.choked) { v.pad = -1; continue; }
            if (from == frames) continue;
        }
        v.sounded += frames - from;
        if (v.zone) {
            if (!admit(v, frames - from)) continue;
            if (v.stretch) stretch(v, l, r, to_l, to_r, from, frames);
            else play_zone(v, to_l, to_r, from, frames);
            if (v.stream >= 0 && v.pad >= 0) pager->place(v.stream, uint32_t(v.stretch ? v.place : v.position));
            continue;
        }
        if (v.grow && !admit_grown(v, frames - from)) continue;
        if (v.stretch) { stretch(v, l, r, to_l, to_r, from, frames); continue; }
        // A loop runs from its loop point to the region's end and round again (backwards: from the end down to the
        // loop point), keeping the fraction and reading across the seam, so a seamless loop stays seamless and in
        // tune at any pitch or device rate.
        const double span = double(v.end - v.loop_at);
        if (plain_runs && plain_run(v, frames - from)) { // nothing this run needs looking at frame by frame: the short loop
            run_plain(v, l, r, to_l, to_r, from, frames, span);
            ++plain_taken;
            continue;
        }
        for (unsigned f = from; f < frames; ++f) {
            if (v.loop) {
                if (v.reverse) while (v.position < v.loop_at) { v.position += span; v.wrapped = v.turned = true; }
                else while (v.position >= v.end) { v.position -= span; v.wrapped = v.turned = true; }
            } else if (v.reverse ? v.position < v.start : v.position >= v.end - 1) { v.pad = -1; break; }
            unsigned i = unsigned(v.position);
            float frac = float(v.position - i);
            unsigned j = i + 1 < v.end ? i + 1 : v.loop ? v.loop_at : v.end - 1;
            glide(v);
            float a = v.env.next(v.shape) * v.gain * v.fade;
            if (v.edge_in || v.edge_out) a *= edge_gain(v, v.position);
            if (v.grow) { v.duck += std::clamp(v.duck_to - v.duck, -edge_step, edge_step); a *= v.duck; }
            if (v.choked) { v.fade -= edge_step; if (v.fade <= 0) { v.pad = -1; break; } } // a quick fade, not a click
            float sl = l[i] + (l[j] - l[i]) * frac, sr = r[i] + (r[j] - r[i]) * frac;
            if (v.xfade_kind) seam(v, l, r, v.position, sl, sr);
            voice_out(v, sl, sr, a, frames_left(v, v.position, v.step), to_l, to_r, f);
            v.position += v.reverse ? -v.step : v.step;
            if (!v.env.active()) { v.pad = -1; break; }
        }
    }
    if (laned)
        for (auto& l : *lane) {
            if (l.pad < 0) continue;
            bool own_chain = chained(l.pad);
            if (own_chain) for (auto& effect : (*pad_effects)[size_t(l.pad)]) effect.process(l.l, l.r, frames);
            float* into_l = out_l(l.pad);
            float* into_r = out_r(l.pad);
            float loudest = 0;
            for (unsigned f = 0; f < frames; ++f) {
                into_l[f] += l.l[f]; into_r[f] += l.r[f];
                loudest = std::max(loudest, std::max(std::fabs(l.l[f]), std::fabs(l.r[f])));
            }
            // Its sends: into each bus as much as the pad sends there, eased across the block so a turn never clicks.
            auto& now = send_now[size_t(l.pad)];
            for (unsigned b = 0; b < send_buses; ++b) {
                float aim = pads[size_t(l.pad)].sends[b] / 100.0f;
                if (!send_l[b] || !send_r[b]) { now[b] = aim; continue; }
                if (now[b] <= 0 && aim <= 0) continue;
                float g = now[b], step = (aim - now[b]) / float(frames);
                for (unsigned f = 0; f < frames; ++f, g += step) { send_l[b][f] += l.l[f] * g; send_r[b][f] += l.r[f] * g; }
                now[b] = aim;
                send_used |= 1u << b;
            }
            // Its tail plays out: the lane goes once the chain has been quiet a while (it holds nothing to come back
            // then), or after lane_ring_seconds of ringing (a loop or a long feedback), cleared so none of it returns.
            if (l.fed) { l.quiet = l.ringing = 0; }
            else { l.ringing += frames; l.quiet = loudest < lane_floor ? l.quiet + frames : 0; }
            bool rung = l.ringing >= uint32_t(lane_ring_seconds * dsp::sample_rate());
            if (rung && own_chain) for (auto& effect : (*pad_effects)[size_t(l.pad)]) effect.clear();
            // A lane for sends alone holds no tail: it goes as soon as nothing fed it.
            bool done = own_chain ? rung || l.quiet >= uint32_t(lane_quiet_seconds * dsp::sample_rate()) : !l.fed;
            if (done) l.pad = -1;
        }
    for (auto& v : voices) if (v.stream >= 0 && v.pad < 0) end_stream(v); // what ended this block lets its stream go
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

// The newest voices first (a partial sort of pointers, on the stack), each
// where it stands now; then where the cloud's grains read.
void SamplerEngine::taps(TrackTaps& out) const {
    std::array<const Voice*, std::tuple_size_v<decltype(voices)>> live {};
    unsigned n = 0;
    for (const auto& v : voices) if (v.pad >= 0) live[n++] = &v;
    unsigned shown = std::min<unsigned>(n, TrackTaps::voices);
    std::partial_sort(live.begin(), live.begin() + shown, live.begin() + n, [](const Voice* a, const Voice* b) { return a->age > b->age; });
    out.count = uint8_t(shown);
    for (unsigned i = 0; i < shown; ++i) {
        const Voice& v = *live[i];
        VoiceTap& t = out.voice[i];
        t = VoiceTap {};
        t.serial = v.age; t.pad = int16_t(v.pad); t.slot = v.slot;
        t.from = v.start; t.to = v.zone && v.loop ? v.loop_hi : v.end;
        t.loop_at = v.loop ? v.loop_at : v.start;
        t.age = v.sounded;
        const double way = v.reverse ? -1.0 : 1.0;
        uint8_t flags = 0;
        if (v.cloud) { // where the grains come from (spawn's centre), and how fast Scan carries it
            double span = v.zone ? std::min(v.end - v.start, stream_chunk) : v.end - v.start;
            const PadSound& own = pads[size_t(v.pad) % sampler_pads];
            double at = v.start + span * (own.cloud_position / 100.0) + v.scan;
            at = std::fmod(std::fmod(at - v.start, span) + span, span) + v.start;
            t.at = uint32_t(at); t.step = float((own.cloud_scan - 50) / 50.0 * v.step);
            t.to = v.start + uint32_t(span);
            flags |= tap::Cloud;
        } else if (v.stretch) { t.at = uint32_t(std::max(0.0, v.place)); t.step = float(way * v.advance); flags |= tap::Stretch; }
        else { t.at = uint32_t(std::max(0.0, v.position)); t.step = float(way * v.step); }
        float level = v.env.level * v.fade;
        if (v.tail > 0) level *= std::min(1.0f, frames_left(v, v.stretch ? v.place : v.position, v.stretch ? v.advance : v.step) / v.tail);
        t.level = std::clamp(level, 0.0f, 1.0f);
        if (v.loop && !v.cloud) flags |= tap::Loop;
        if (v.wrapped) flags |= tap::Wrapped;
        if (v.env.stage == dsp::Envelope::Stage::Release) flags |= tap::Releasing;
        if (v.wait) flags |= tap::Waiting;
        if (v.choked) flags |= tap::Choked;
        t.flags = flags;
        if (v.scale != 1) { // a copy at the device's rate: back to the 44.1 kHz frames the pages draw (roadmap P2)
            auto model = [&](uint32_t f) { return uint32_t(std::llround(f / v.scale)); };
            t.from = model(t.from); t.to = model(t.to); t.loop_at = model(t.loop_at); t.at = model(t.at);
            t.step = float(t.step / v.scale);
        }
    }
    // The newest grains (a partial sort of pointers, on the stack), each where it began and where it reads now.
    std::array<const Grain*, std::tuple_size_v<decltype(grains)>> playing {};
    unsigned count = 0;
    for (const auto& g : grains) if (g.sample) playing[count++] = &g;
    unsigned kept = std::min<unsigned>(count, TrackTaps::grains);
    std::partial_sort(playing.begin(), playing.begin() + kept, playing.begin() + count,
                      [](const Grain* a, const Grain* b) { return int32_t(a->serial - b->serial) > 0; });
    out.grain_count = uint8_t(kept);
    for (unsigned i = 0; i < kept; ++i) {
        const Grain& g = *playing[i];
        out.grain[i] = GrainTap {g.serial, uint32_t(std::max(0.0, g.from / g.scale)), uint32_t(std::max(0.0, g.position / g.scale)), g.age,
                                 g.length, float(g.step / g.scale), g.pad, g.slot};
    }
}

} // namespace melodee
