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
    // A pad in a mute group cuts the others in it; a mono pad cuts what it played before.
    if (sound.group)
        for (auto& o : voices) if (o.pad >= 0 && o.group == sound.group && o.pad != pad) o.choked = true;
    if (sound.mono && !zone) // an instrument's press has cut these already: its layers must not cut each other
        for (auto& o : voices) if (o.pad == pad && &o != &v) o.choked = true;
    end_stream(v);
    v = Voice {};
    v.pad = pad; v.trigger = trigger >= 0 ? trigger : pad; v.sample = data; v.slot = sound.sample;
    // Roadmap P2: a sample made ready at the device's rate plays from that copy, a frame each device frame (only its
    // pitch moves the read on), the pad's 44.1 kHz frames scaled there; else (an instrument's zones, or no copy made)
    // from its 44.1 kHz frames, each device frame reading that much further on.
    const bool ready = !part && data->plays_at(dsp::sample_rate());
    v.scale = ready ? double(data->play_rate) / dsp::model_rate : 1.0;
    auto at_rate = [&](uint32_t frame) { return ready ? uint32_t(std::llround(frame * v.scale)) : frame; };
    const uint32_t total = part ? part->total : ready ? data->play_frames : data->frames;
    v.start = std::min(zone ? zone->start : at_rate(sound.start), total - 1);
    uint32_t end = zone ? zone->end : at_rate(sound.end);
    v.end = end && end <= total ? end : total;
    if (v.end <= v.start + 1) { v.pad = -1; return; }
    v.l = part ? part->left : ready ? data->play_left.data() : data->left.data();
    v.r = part ? part->right : ready ? data->play_right.data() : data->right.data();
    v.reverse = sound.reverse;
    v.position = v.reverse ? v.end - 1 : v.start;
    if (zone) {
        v.zone = true; v.key = key; v.semis = semitones; v.off_by = zone->off_by;
        v.loop = !zone->one_shot && !v.reverse && zone->loop != ZoneLoop::Off && zone->loop_end > zone->loop_start + 16 && zone->loop_end <= v.end;
        v.loop_at = zone->loop_start; v.loop_hi = zone->loop_end;
        v.loop_held = zone->loop == ZoneLoop::UntilReleased;
    } else {
        v.loop = sound.loop && pad < int(sampler_pads);
        v.loop_at = std::clamp(at_rate(sound.loop_point()), v.start, v.end - 2);
        v.edge_in = !v.loop && v.start > 0;              // the file's own start and end need no fade
        v.edge_out = !v.loop && v.end < total;
        v.total = total;
        if (v.loop && sound.loop_xfade) { // roadmap P3: its time in the frames it reads, at most half the loop
            const double read_rate = ready ? double(data->play_rate) : double(dsp::model_rate);
            const uint32_t span = v.end - v.loop_at;
            const uint32_t want = std::min<uint32_t>(uint32_t(std::lround(sound.loop_xfade / 1000.0 * read_rate)), span / 2);
            const uint32_t before = v.reverse ? total - v.end : v.loop_at;  // across the seam on its near side
            const uint32_t after = v.reverse ? v.loop_at : total - v.end;   // across it once round
            if (want >= 2) {
                if (before > want) { v.xfade_kind = 1; v.xfade = want; }
                else if (after > want + 1) { v.xfade_kind = 2; v.xfade = want; }
                else { v.xfade_kind = 3; v.xfade = std::max<uint32_t>(2, std::min(want, span / 4)); }
            }
        }
    }
    // Warp's mode is the kit's FollowHow: Tape is Pitch, the others Time stretch.
    // Warp on by Pitch leaves the pad's Semi and Fine out, as the MPC does.
    const bool tape = p[FollowHow] == 2, warped = !zone && sound.warped();
    // A zone is in tune by its key from its root; the key has the pad's Semi and the note in it.
    const double own_tune = zone ? (key - int(zone->root)) * zone->keytrack / 100.0 + zone->tune / 100.0 + sound.fine / 100.0
                          : warped && tape ? 0.0 : sound.pitch + sound.fine / 100.0;
    v.step = std::exp2((own_tune + (zone ? 0 : semitones) + p[Tune]) / 12.0);
    v.cloud = p[Cloud] != 0 && pad < int(sampler_pads); // not for Chop's audition
    // Warp, else Follow: the speed the region moves at. Warp's Seq plays it at
    // the project's tempo from its own (by # Beats, else worked out from its
    // length); 50..200 % makes it that long. By Tape, speed and pitch move
    // together; by Beats or Tones the sound is stretched, its pitch kept.
    // A pitch change leaves the length alone under PITCH Stretch, and under
    // Warp by Time stretch; Warp Off plays it by speed, so tuned pads stay clean.
    double rate = 1.0, frames = double(v.end - v.start);
    if (zone) {} // no tempo of its own, and no Warp: a zone is one note of many
    else if (sound.warp == warp_seq) {
        double own = sound.beats ? sound.beats * 60.0 * dsp::model_rate / (frames / v.scale) * 10.0 : double(sound.own_tempo);
        if (own > 0) rate = tempo / own;
    } else if (sound.warp >= warp_min) rate = 100.0 / sound.warp;
    else if (p[Follow] && sound.own_tempo) rate = double(tempo) / sound.own_tempo;
    const bool keep_length = p[PitchHow] == 1 || (warped && !tape);
    const bool shifted = std::fabs(v.step - 1.0) > 1e-6;
    // At 44.1 kHz (no copy at the device's rate) each output frame reads that much further along.
    double per_frame = ready ? 1.0 : dsp::model_rate / dsp::sample_rate();
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
    v.held = true; v.gate = sound.play == PadPlay::Gate && !(zone && zone->one_shot); v.group = sound.group;
    if (zone && zone->on_release) { v.held = v.gate = v.loop = false; v.stretch = false; } // a release sample plays out once
    // A cloud never reads to its region's end, so a one shot's cloud lasts as
    // long as the pad would play, then lets go; a gate ends on release, a loop when hit again.
    if (v.cloud && !v.gate && !v.loop) v.life = std::max(1.0, frames / std::max(advance, 1e-6));
    v.age = ++clock;
    // Offset: up to a sixteenth of the project's tempo after the hit.
    v.wait = pad < int(sampler_pads) ? uint32_t(sound.offset * (dsp::sample_rate() * 15 / (tempo / 10.0)) / 100) : 0;
    // Volume (dB, as the MPC shows it), Normalize, and velocity by the pad's sensitivity.
    float sense = sound.velocity_sense / 100.0f, curve = 0.25f + 0.75f * std::clamp(velocity, 0, 127) / 127.0f;
    float velocity_gain = 1.0f - sense * (1.0f - curve);
    v.gain = volume_gain(sound.level) * volume_gain(p[Level]) * velocity_gain * std::pow(10.0f, sound.gain / 2000.0f) * accent;
    if (zone) v.gain *= std::pow(10.0f, zone->gain / 20.0f) * zone_crossfade(*zone, key, std::clamp(velocity, 1, 127)); // its xfin / xfout
    float angle = (std::clamp<float>(float(sound.pan) + (zone ? zone->pan * 50.0f : 0.0f), -50.0f, 50.0f) + 50) / 100.0f * dsp::pi / 2;
    v.left = std::cos(angle); v.right = std::sin(angle); // constant power: centre is -3 dB a side, as the stock MPC plays it
    // The pad's envelope; the kit's DECAY scales every pad's decay. A one shot
    // that decays from the end plays whole and fades over its decay before the end.
    // A zone's stages are off the pad's, so the pad's controls still move every zone.
    const int attack = zone ? std::clamp(sound.attack + zone->attack, 0, 100) : sound.attack;
    const int decay = zone ? std::clamp(sound.decay + zone->decay, 0, 100) : sound.decay;
    const int sustain = zone ? std::clamp(sound.sustain + zone->sustain, 0, 100) : sound.sustain;
    const int release = zone ? std::clamp(sound.release + zone->release, 0, 100) : sound.release;
    float decay_s = time_seconds(decay) * (p[Decay] / 100.0f) + 0.005f;
    bool from_end = !v.gate && !sound.decay_from_start;
    bool hold = decay >= 100 || from_end;
    v.shape = {dsp::Envelope::coefficient(time_seconds(attack) * 0.3f), dsp::Envelope::coefficient(hold ? 1000.0f : decay_s),
               dsp::Envelope::coefficient(time_seconds(release) * 0.5f + 0.003f), hold ? 1.0f : sustain / 100.0f};
    if (from_end && decay < 100 && !v.loop) v.tail = decay_s * dsp::sample_rate();
    v.env.gate(true);
    // The pad's filter, or 16 Levels' cutoff (Classic when the pad has none).
    v.filter = sound.filter;
    int at = sound.cutoff;
    if (cutoff >= 0) { at = cutoff; if (v.filter == PadFilter::Off) v.filter = PadFilter::Classic; }
    const bool zone_filter = zone && zone->filter != PadFilter::Off && zone->cutoff > 0;
    if (zone_filter) v.filter = zone->filter;
    if (v.filter != PadFilter::Off) {
        float res = sound.resonance / 100.0f;
        v.cutoff = at / 100.0f * 9.97f;
        if (zone_filter) // its own Hz, off the pad's CUTOFF wide open, so that control still moves it
            v.cutoff = std::log2(zone->cutoff / 20.0f) + (zone->cutoff_velocity * std::clamp(velocity, 0, 127) / 127.0f + zone->cutoff_key * float(key - int(zone->cutoff_center))) / 1200.0f
                     + (at / 100.0f - 1.0f) * 9.97f;
        v.depth = sound.filter_amount / 100.0f * 7.0f; // up to seven octaves open
        bool four = v.filter == PadFilter::Lpf4 || v.filter == PadFilter::Hpf4 || v.filter == PadFilter::Bpf4;
        if (v.filter == PadFilter::Classic) v.k1 = 2.0f - 1.97f * res;
        else if (four) { v.k1 = 1.848f; v.k2 = std::max(0.05f, 0.765f - 0.715f * res); } // Butterworth pairs, the resonance on the second
        else v.k1 = std::max(0.05f, 1.414f - 1.364f * res);
        if (zone_filter) v.k1 = std::max(0.05f, std::pow(10.0f, -zone->resonance / 20.0f)); // the dB its peak has
        float f_decay = time_seconds(sound.filter_decay);
        bool f_hold = from_end;
        v.filter_shape = {dsp::Envelope::coefficient(time_seconds(sound.filter_attack) * 0.3f), dsp::Envelope::coefficient(f_hold ? 1000.0f : f_decay),
                          dsp::Envelope::coefficient(time_seconds(sound.filter_release) * 0.5f + 0.003f), f_hold ? 1.0f : sound.filter_sustain / 100.0f};
        if (f_hold && !v.loop && sound.filter_decay < 100) v.filter_tail = f_decay * dsp::sample_rate(); // 100 holds, as the amp's decay
        v.filter_env.gate(true);
        v.since_tune = 1u << 30; // tune on the first frame
    }
    if (part && pager) {
        v.stream = pager->open(*part, uint32_t(v.position), v.reverse, float(std::max(advance, pitch)));
        if (v.stream < 0) { // no stream free: what is held plays, then it ends
            if (v.reverse) { v.pad = -1; return; }
            v.capped = true;
            v.head_end = std::min(v.start + stream_chunk, v.end);
        }
    }
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
// a mono pad's last hit is cut once for all of them, so its layers never cut each other.
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
    if (sound.mono) for (auto& v : voices) if (v.pad == pad) v.choked = true;
    for (unsigned k = 0; k < chosen; ++k) {
        PadSound one = layer_sound(sound, picked[k]);
        one.mono = false; // cut above, once
        start(free_voice(), one, pad, velocity, semitones, cutoff, trigger);
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

// A note is a pad's key, and over it the semitones from the pad's root it plays at (an instrument's).
void SamplerEngine::note_on(int key, int velocity) { hit((key & 0xff) - int(sampler_first_note), velocity, int8_t(key >> 8), -1); }

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
        if (v.pad >= 0 && z.rt_decay > 0) v.gain *= std::pow(10.0f, -z.rt_decay * held / 20.0f);
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
    for (auto& v : voices) if (v.pad >= 0) { v.held = false; v.env.gate(false); v.filter_env.gate(false); }
}
void SamplerEngine::silence() {
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
    double at = v.start + span * (p[Position] / 100.0) + v.scan + random * span * 0.05;
    at = std::fmod(std::fmod(at - v.start, span) + span, span) + v.start;
    seed = seed * 1664525u + 1013904223u;
    float detune = int32_t(seed) / 2147483648.0f * p[Spread] / 100.0f; // up to a semitone either way per 100 cents
    double step = v.step * std::exp2(detune / 12.0);
    uint32_t length = uint32_t(0.01f * std::pow(100.0f, p[Size] / 100.0f) * dsp::sample_rate());
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
constexpr double slice_edge = 0.002 * dsp::model_rate;
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
        float a = v.env.next(v.shape) * v.gain * v.fade;
        if (v.zone) { v.duck += std::clamp(v.duck_to - v.duck, -edge_step, edge_step); a *= v.duck; }
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
        voice_out(v, sum_l, sum_r, a, frames_left(v, v.place, v.advance), to_l, to_r, f);
        v.place += way * v.advance;
        if (!v.env.active()) { v.pad = -1; return; }
    }
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

// A zone's frames: as a pad's, but round a loop of its own, the fraction kept
// and the seam read across, so a loop in tune at any pitch does not click.
void SamplerEngine::play_zone(Voice& v, float* to_l, float* to_r, unsigned from, unsigned frames) {
    const float* l = v.l;
    const float* r = v.r;
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
        float a = v.env.next(v.shape) * v.gain * v.fade;
        v.duck += std::clamp(v.duck_to - v.duck, -edge_step, edge_step);
        a *= v.duck;
        if (v.choked) { v.fade -= edge_step; if (v.fade <= 0) { v.pad = -1; return; } }
        voice_out(v, l[i] + (l[j] - l[i]) * frac, r[i] + (r[j] - r[i]) * frac, a, frames_left(v, v.position, v.step), to_l, to_r, f);
        v.position += v.reverse ? -v.step : v.step;
        if (!v.env.active()) { v.pad = -1; return; }
    }
}

SamplerEngine::Lane* SamplerEngine::lane_for(int pad) {
    Lane* free = nullptr;
    for (auto& l : lane) {
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
    // Clouds: their voices only place grains; the grains play.
    for (auto& v : voices) {
        if (v.pad < 0 || !v.cloud) continue;
        for (unsigned f = 0; f < frames; ++f) v.env.next(v.shape);
        if (v.life > 0 && (v.life -= frames) <= 0) { v.env.gate(false); v.filter_env.gate(false); }
        v.sounded += frames;
        if (!v.env.active() || (v.choked && (v.fade -= 0.25f) <= 0)) { v.pad = -1; continue; }
        v.scan += (p[Scan] - 50) / 50.0 * v.step * frames; // forwards or backwards at up to its own speed
        v.next_grain -= frames;
        double every = dsp::sample_rate() / std::max(1, int(1 + p[Density] * 0.99));
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
    // The punched pad renders on its own, and so does each pad with a chain
    // (blocks of 128 at most here).
    bool punching = aside && frames <= 128, chains = pad_effects && frames <= 128;
    bool knobbing = (knob_mask[0] || knob_mask[1]) && knob_l && knob_r && frames <= 128;
    if (punching) { std::fill(aside_l, aside_l + frames, 0.0f); std::fill(aside_r, aside_r + frames, 0.0f); }
    if (chains) for (auto& l : lane) if (l.pad >= 0) { l.fed = false; std::fill(l.l, l.l + frames, 0.0f); std::fill(l.r, l.r + frames, 0.0f); }
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
        float* to_l = punching && v.pad == aside_pad ? aside_l : out_l(v.pad);
        float* to_r = punching && v.pad == aside_pad ? aside_r : out_r(v.pad);
        if (chains && v.pad < int(sampler_pads) && (*pad_effects)[size_t(v.pad)][0].kind() != EffectKind::None)
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
        if (v.stretch) { stretch(v, l, r, to_l, to_r, from, frames); continue; }
        // A loop runs from its loop point to the region's end and round again (backwards: from the end down to the
        // loop point), keeping the fraction and reading across the seam, so a seamless loop stays seamless and in
        // tune at any pitch or device rate.
        const double span = double(v.end - v.loop_at);
        for (unsigned f = from; f < frames; ++f) {
            if (v.loop) {
                if (v.reverse) while (v.position < v.loop_at) { v.position += span; v.wrapped = v.turned = true; }
                else while (v.position >= v.end) { v.position -= span; v.wrapped = v.turned = true; }
            } else if (v.reverse ? v.position < v.start : v.position >= v.end - 1) { v.pad = -1; break; }
            unsigned i = unsigned(v.position);
            float frac = float(v.position - i);
            unsigned j = i + 1 < v.end ? i + 1 : v.loop ? v.loop_at : v.end - 1;
            float a = v.env.next(v.shape) * v.gain * v.fade;
            if (v.edge_in || v.edge_out) a *= edge_gain(v, v.position);
            if (v.choked) { v.fade -= edge_step; if (v.fade <= 0) { v.pad = -1; break; } } // a quick fade, not a click
            float sl = l[i] + (l[j] - l[i]) * frac, sr = r[i] + (r[j] - r[i]) * frac;
            if (v.xfade_kind) seam(v, l, r, v.position, sl, sr);
            voice_out(v, sl, sr, a, frames_left(v, v.position, v.step), to_l, to_r, f);
            v.position += v.reverse ? -v.step : v.step;
            if (!v.env.active()) { v.pad = -1; break; }
        }
    }
    if (chains)
        for (auto& l : lane) {
            if (l.pad < 0) continue;
            auto& chain = (*pad_effects)[size_t(l.pad)];
            for (auto& effect : chain) effect.process(l.l, l.r, frames);
            float* into_l = punching && l.pad == aside_pad ? aside_l : out_l(l.pad); // a punched pad: its chain, then the punch
            float* into_r = punching && l.pad == aside_pad ? aside_r : out_r(l.pad);
            float loudest = 0;
            for (unsigned f = 0; f < frames; ++f) {
                into_l[f] += l.l[f]; into_r[f] += l.r[f];
                loudest = std::max(loudest, std::max(std::fabs(l.l[f]), std::fabs(l.r[f])));
            }
            // Its tail plays out: the lane goes once the chain has been quiet a while (it holds nothing to come back
            // then), or after lane_ring_seconds of ringing (a loop or a long feedback), cleared so none of it returns.
            if (l.fed) { l.quiet = l.ringing = 0; }
            else { l.ringing += frames; l.quiet = loudest < lane_floor ? l.quiet + frames : 0; }
            bool rung = l.ringing >= uint32_t(lane_ring_seconds * dsp::sample_rate());
            if (rung) for (auto& effect : chain) effect.clear();
            if (rung || l.quiet >= uint32_t(lane_quiet_seconds * dsp::sample_rate()) || chain[0].kind() == EffectKind::None) l.pad = -1;
        }
    if (punching) {
        aside->process(aside_l, aside_r, frames);
        float* into_l = out_l(aside_pad);
        float* into_r = out_r(aside_pad);
        for (unsigned f = 0; f < frames; ++f) { into_l[f] += aside_l[f]; into_r[f] += aside_r[f]; }
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
            double at = v.start + span * (p[sampler::Position] / 100.0) + v.scan;
            at = std::fmod(std::fmod(at - v.start, span) + span, span) + v.start;
            t.at = uint32_t(at); t.step = float((p[sampler::Scan] - 50) / 50.0 * v.step);
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
