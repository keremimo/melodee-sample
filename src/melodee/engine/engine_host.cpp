// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/engine_host.h"
#include <chrono>
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/expression.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
// Flush denormals to zero on this thread: decaying filters and envelopes
// would otherwise crawl through slow subnormal arithmetic.
void flush_denormals() {
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("msr fpcr, %0" ::"r"(fpcr | (uint64_t(1) << 24)));
#endif
}
float level_gain(int level) { float x = std::clamp(level, 0, 127) / 100.0f; return x * x; }
} // namespace

PlayPatterns copy_patterns(const std::array<Pattern, pattern_count>& from) {
    PlayPatterns out {};
    for (unsigned p = 0; p < pattern_count; ++p) out[p] = new Pattern(from[p]);
    return out;
}
void free_patterns(PlayPatterns& patterns) { for (auto& p : patterns) { delete p; p = nullptr; } }

ProjectPack::ProjectPack(const Project& project, const PatternBank& bank) : project(project) {
    for (unsigned t = 0; t < track_count; ++t) patterns[t] = copy_patterns(bank.at[t]);
}
ProjectPack::~ProjectPack() { for (auto& t : patterns) free_patterns(t); }
TrackPack::TrackPack(unsigned track, const Track& value, const std::array<Pattern, pattern_count>& from, const Motion& motion)
    : track(uint8_t(track)), value(value), patterns(copy_patterns(from)), motion(motion) {}
TrackPack::~TrackPack() { free_patterns(patterns); }

EngineHost::EngineHost() {
    for (auto& t : patterns) for (auto& p : t) p = new Pattern;
    for (auto& s : positions_now) s.store(-1);
    pad_fx = std::make_unique<std::array<SamplerEngine::PadEffects, track_count>>();
    for (unsigned t = 0; t < track_count; ++t) { slots[t].sampler.set_pad_effects(&(*pad_fx)[t]); slots[t].sampler.set_pager(&pager); }
    // Build every engine's static tables now, not on the audio thread.
    for (unsigned k = 0; k < unsigned(EngineKind::Count); ++k) (void)engine_info(EngineKind(k));
}

EngineHost::~EngineHost() {
    for (auto& t : patterns) free_patterns(t);
    Pattern* p = nullptr;
    while (retired_patterns.pop(p)) delete p;
    for (auto& q : limbo) delete q;
}

// Before the audio thread runs: copies into the patterns it already holds.
void EngineHost::load(const Project& project, const PatternBank& bank) {
    for (unsigned t = 0; t < track_count; ++t)
        for (unsigned p = 0; p < pattern_count; ++p) *patterns[t][p] = bank.at[t][p];
    load_state(project);
}

// Everything of a project but its patterns (Reload swaps those in by pointer).
void EngineHost::load_state(const Project& project, bool keep_loops) {
    for (unsigned n = track_count; n < chain_count; ++n) { // the buses and the master; each track's in load_track()
        const auto& chain = project.chain(n);
        for (unsigned k = 0; k < max_slots; ++k) {
            const auto& s = chain.slots[k];
            fx[n][k].set_kind(EffectKind(s.kind), pool);
            for (unsigned q = 0; q < effect_params; ++q) fx[n][k].set_param(q, s.params[q]);
            fx[n][k].set_bypass(s.bypass);
        }
    }
    for (unsigned b = 0; b < bus_count; ++b) returns[b] = project.buses[b].level / 100.0f;
    // Knob FX and the master compressor.
    const auto& k = project.knob_fx;
    knob.set_kind(EffectKind(k.kind), pool);
    knob_in.set_kind(EffectKind(k.kind), pool);
    for (unsigned q = 0; q < effect_params; ++q) { knob.set_param(q, k.params[q]); knob_in.set_param(q, k.params[q]); }
    knob_heard = k.active();
    knob_tracks = k.tracks;
    knob_pads = k.pads;
    comp_settings = project.compressor;
    comp.set(comp_settings);
    // Pad FX's K1-K3 and Flex Beat's settings.
    mpc_params = project.pad_fx_params;
    flex_mode = project.flex.mode; flex_mix = project.flex.mix; flex_quantize = project.flex.quantize;
    motion = project.motion;
    song = project.song;
    global_tempo = project.tempo; swing = project.swing;
    seq_tempo = project.seq_tempo; pattern_tempo = project.pattern_tempo;
    bar = project.bar_ticks(); beat = project.beat_ticks();
    for (unsigned t = 0; t < track_count; ++t) load_track(t, project.tracks[t]);
    for (unsigned l = 0; l < loop_tracks; ++l) { // the Looper's tracks
        const auto& m = project.loop_mix[l];
        uint8_t t = uint8_t(track_count + l);
        for (auto type : {Command::Type::Level, Command::Type::Pan, Command::Type::Send, Command::Type::Mute, Command::Type::Solo, Command::Type::SendBus}) {
            int16_t v = type == Command::Type::Level ? m.level : type == Command::Type::Pan ? m.pan : type == Command::Type::Send ? m.send
                      : type == Command::Type::Mute ? m.mute : type == Command::Type::Solo ? m.solo : m.bus;
            loop_strip_command({type, t, 0, 0, v});
        }
    }
    loops.configure(project.looper, keep_loops);
    knob_targets();
    lead = project.tracks[0].pattern;
    tempo = 0;
    follow_tempo();
}

void EngineHost::set_audio_format(double frames_a_second, uint64_t latency_frames) {
    clock.set_format(frames_a_second, latency_frames);
    sync.set_rate(frames_a_second);
    float rate = float(frames_a_second);
    if (rate == dsp::sample_rate()) return;
    dsp::device_rate.store(rate, std::memory_order_relaxed);
    take.set_rate(rate); recall_ring.set_rate(rate); render_out.set_rate(rate);
    reformatting = true;
    format_serial.fetch_add(1, std::memory_order_release);
}

void EngineHost::follow_tempo() {
    unsigned own = pattern_tempo[lead % pattern_count];
    uint16_t now = uint16_t(ext_on && ext_tempo ? ext_tempo : seq_tempo && own ? own : global_tempo); // MIDI clock in leads
    if (now == tempo) return;
    tempo = now;
    for (auto& s : slots) s.sampler.set_tempo(tempo);
    tempo_shown.store(tempo, std::memory_order_relaxed);
}

void EngineHost::retire(Pattern* pattern) {
    if (!pattern || retired_patterns.push(pattern)) return;
    for (auto& q : limbo) if (!q) { q = pattern; return; } // tried again each block
    // No room anywhere: it is leaked rather than freed here.
}

// A note Seq Record wrote just ahead of the playhead was heard as it was played: once, not twice.
bool EngineHost::skipped(Slot& slot, unsigned key, uint32_t tick) {
    for (auto& s : slot.skips) {
        if (s.key < 0) continue;
        if (tick_now > s.until) { s.key = -1; continue; }
        if (s.key == int(key) && s.tick == tick && s.pattern == slot.pattern) { s.key = -1; return true; }
    }
    return false;
}

// Everything of one track but its patterns: engine and sound, kit, mix, its
// chain and its pads' chains, the arp. Effects take their delay memory from
// the pool made at start, so this allocates nothing (audio thread for TrackLoad).
void EngineHost::load_track(unsigned t, const Track& track) {
    auto& slot = slots[t % track_count];
    midi_all_off(t, frame_now); // what this track sent ends before it changes
    select_engine(slot, track.engine);
    const auto& info = engine_info(track.engine);
    slot.base = track.params;
    if (slot.engine) for (unsigned r = 0; r < mod_routes; ++r) slot.engine->set_mod(r, track.mods[r]);
    for (unsigned pad = 0; pad < sampler_pads; ++pad) slot.sampler.set_pad(pad, track.pads[pad]);
    slot.sampler.set_pad_mutes(track.pad_mutes);
    if (slot.engine) for (unsigned p = 0; p < info.param_count; ++p) slot.engine->set_param(int(p), track.params[p]);
    apply({Command::Type::Level, uint8_t(t), 0, 0, track.mix.level});
    apply({Command::Type::Pan, uint8_t(t), 0, 0, track.mix.pan});
    slot.send = track.mix.send / 100.0f;
    slot.bus = track.mix.bus;
    slot.mute = track.mix.mute; slot.solo = track.mix.solo;
    slot.pattern = track.pattern;
    patterns_now[t].store(slot.pattern, std::memory_order_relaxed);
    for (unsigned k = 0; k < max_slots; ++k) {
        const auto& s = track.fx.slots[k];
        fx[t][k].set_kind(EffectKind(s.kind), pool);
        for (unsigned q = 0; q < effect_params; ++q) fx[t][k].set_param(q, s.params[q]);
        fx[t][k].set_bypass(s.bypass);
        slot.fx_base[k] = s.params;
    }
    slot.fx_moved = false;
    for (unsigned pad = 0; pad < sampler_pads; ++pad) {
        const auto& chain = track.pad_fx.at[pad];
        for (unsigned k = 0; k < pad_slots; ++k) {
            auto& effect = (*pad_fx)[t][pad][k];
            const auto& s = chain.slots[k];
            if (!s.kind && effect.kind() == EffectKind::None) continue; // most pads have none
            effect.set_kind(EffectKind(s.kind), pool);
            for (unsigned q = 0; q < effect_params; ++q) effect.set_param(q, s.params[q]);
            effect.set_bypass(s.bypass);
        }
    }
    auto& a = slot.arp;
    a.clear(); a.sounding = -1; slot.arp_late.key = -1; slot.arp_played = slot.arp_from = -1;
    a.on = track.arp; a.mode = ArpMode(track.arp_mode % unsigned(ArpMode::Count)); a.rate = track.arp_rate % arp_rates;
    a.octaves = track.engine == EngineKind::Sampler ? 1 : std::clamp<unsigned>(track.arp_octaves, 1, 4); a.gate = std::clamp<unsigned>(track.arp_gate, 5, 100);
    if (slot.kind == EngineKind::Midi) midi_program(t, frame_now); // the device gets the track's sound as it loads
}

void EngineHost::select_engine(Slot& slot, EngineKind kind) {
    if (slot.engine) slot.engine->silence();
    slot.kind = kind;
    slot.engine = kind == EngineKind::Prophet ? static_cast<Engine*>(&slot.prophet)
                : kind == EngineKind::Sampler ? static_cast<Engine*>(&slot.sampler)
                : kind == EngineKind::Kit909 ? static_cast<Engine*>(&slot.kit)
                : kind == EngineKind::Fm6 ? static_cast<Engine*>(&slot.fm)
                : kind == EngineKind::Sid ? static_cast<Engine*>(&slot.sid)
                : kind == EngineKind::Cz ? static_cast<Engine*>(&slot.cz)
                : kind == EngineKind::Midi ? static_cast<Engine*>(&slot.midi) : nullptr;
    slot.sampler.set_samples(&samples);
    slot.arp.top = kind == EngineKind::Sampler ? int(sampler_last_key) : 127; // a kit's notes run past 127
    for (auto& off : slot.offs) off.key = -1;
    slot.held = {};
}

bool EngineHost::loop_strip_command(const Command& c) {
    using Type = Command::Type;
    if (c.track < track_count || c.track >= track_count + loop_tracks) return false;
    auto& strip = loop_strips[c.track - track_count];
    switch (c.type) {
    case Type::Level: strip.gain = level_gain(c.value); return true;
    case Type::Pan: {
        float angle = (std::clamp<int>(c.value, -50, 50) + 50) / 100.0f * dsp::pi / 2;
        strip.pan_left = std::cos(angle); strip.pan_right = std::sin(angle);
        return true;
    }
    case Type::Send: strip.send = c.value / 100.0f; return true;
    case Type::Mute: strip.mute = c.value != 0; return true;
    case Type::Solo: strip.solo = c.value != 0; return true;
    case Type::SendBus: strip.bus = uint8_t(c.value % bus_count); return true;
    default: return false;
    }
}

void EngineHost::apply(const Command& c) {
    using Type = Command::Type;
    if (loop_strip_command(c)) return;
    auto& slot = slots[c.track % track_count];
    switch (c.type) {
    case Type::Looper: loops.ask(looper::Do(c.index), c.track, c.value, c.position); break;
    case Type::NoteOn:
        if (slot.engine && (c.index <= 127 || slot.kind == EngineKind::Sampler)) slot.engine->note_on(c.index, c.value);
        if (c.pattern != 1) midi_note(c.track % track_count, c.index & 0xff, c.value, frame_now, -1); // a pad's note (not one from MIDI in)
        break;
    case Type::NoteOff:
        if (slot.engine && (c.index <= 127 || slot.kind == EngineKind::Sampler)) slot.engine->note_off(c.index);
        if (c.pattern != 1) midi_note_off(c.track % track_count, c.index & 0xff, frame_now);
        break;
    case Type::AllNotesOff:
        midi_all_off(c.track, frame_now);
        for (unsigned t = 0; t < track_count; ++t)
            if ((c.track == 255 || t == c.track) && slots[t].engine) {
                slots[t].engine->all_notes_off();
                for (auto& off : slots[t].offs) off.key = -1;
                slots[t].held = {};
            }
        break;
    case Type::Engine: midi_all_off(c.track, frame_now); select_engine(slot, EngineKind(c.index)); break;
    case Type::Sound:
        if (slot.kind == EngineKind::Midi) midi_all_off(c.track, frame_now); // its port or channel may change
        if (slot.engine) {
            std::array<int16_t, max_params> values;
            const auto& info = engine_info(slot.kind);
            sound_values(info, c.index, values);
            for (unsigned p = 0; p < info.param_count; ++p) slot.engine->set_param(int(p), values[p]);
            slot.base = values;
        }
        break;
    case Type::Param:
        // A MIDI track's port or channel: what went out on the old one ends there.
        if (slot.kind == EngineKind::Midi && c.index < midi_track::routing_params && slot.base[c.index] != c.value) midi_all_off(c.track, frame_now);
        if (c.index < max_params) slot.base[c.index] = c.value;
        if (slot.engine) slot.engine->set_param(c.index, c.value);
        if (slot.kind == EngineKind::Midi) midi_param(c.track % track_count, c.index, c.value, frame_now);
        break;
    case Type::MotionWrite: motion.write(c.motion); break;
    case Type::Mod: if (slot.engine && c.index < mod_routes) slot.engine->set_mod(c.index, c.route); break;
    case Type::FxKind:
        if (c.track < chain_count && c.index < max_slots) {
            fx[c.track][c.index].set_kind(EffectKind(c.value), pool);
            if (c.track < track_count) { // the track's own values start at the kind's initial ones
                const auto& info = effect_info(EffectKind(c.value));
                for (unsigned q = 0; q < effect_params; ++q) slots[c.track].fx_base[c.index][q] = info.params[q].label ? info.params[q].initial : 0;
            }
        }
        break;
    case Type::FxParam:
        if (c.track < chain_count && c.index < max_slots) {
            fx[c.track][c.index].set_param(c.pattern, c.value);
            if (c.track < track_count && c.pattern < effect_params) slots[c.track].fx_base[c.index][c.pattern] = c.value;
        }
        break;
    case Type::FxBypass: if (c.track < chain_count && c.index < max_slots) fx[c.track][c.index].set_bypass(c.value != 0); break;
    case Type::PadFxKind: case Type::PadFxParam: case Type::PadFxBypass: {
        unsigned slot = c.index & 0xff, param = c.index >> 8;
        if (c.track >= track_count || c.pattern >= sampler_pads || slot >= pad_slots) break;
        auto& effect = (*pad_fx)[c.track][c.pattern][slot];
        if (c.type == Type::PadFxKind) effect.set_kind(EffectKind(c.value), pool);
        else if (c.type == Type::PadFxParam) effect.set_param(param, c.value);
        else effect.set_bypass(c.value != 0);
        break;
    }
    case Type::SendBus: slot.bus = uint8_t(c.value % bus_count); break;
    case Type::Return: if (c.index < bus_count) returns[c.index] = std::clamp<int>(c.value, 0, 100) / 100.0f; break;
    case Type::Pad: slot.sampler.set_pad(c.index, c.pad); break;
    case Type::PadMute:
        if (c.index == 0xffff) for (unsigned pad = 0; pad < sampler_pads; ++pad) slot.sampler.set_pad_mute(pad, c.value != 0);
        else slot.sampler.set_pad_mute(c.index, c.value != 0);
        break;
    case Type::TrackLoad:
        if (c.track_pack) {
            const auto& pack = *c.track_pack;
            auto& target = slots[pack.track % track_count];
            if (target.engine) target.engine->all_notes_off();
            load_track(pack.track, pack.value);
            std::swap(patterns[pack.track % track_count], c.track_pack->patterns); // the pack takes the old ones back to be freed
            motion = pack.motion;
            if (!done_tracks.push(c.track_pack)) {} // the MIDI thread frees it; a full queue only leaks one
        }
        break;
    case Type::Reload:
        if (c.pack) {
            apply({Type::Stop});
            for (auto& s : slots) if (s.engine) s.engine->silence();
            pending.waiting = false; punch.clear(); pad_punch.clear(); punch_target = 255;
            for (auto& l : live) { l.pending.waiting = false; l.punch.stop(); l.punch.clear(); }
            flex.clear(); flex_pad = 0; flex_waiting = false; flex_now.store(0, std::memory_order_relaxed);
            load_state(c.pack->project, c.value == 1);
            reformatting = false;
            std::swap(patterns, c.pack->patterns); // the pack takes the old ones back to be freed
            if (!done_packs.push(c.pack)) {}         // the MIDI thread frees it; a full queue only leaks one
        }
        break;
    case Type::SongPunchSet: if (c.index < song_punches) song.punches[c.index] = c.song_punch; break;
    case Type::SongPunchCount: song.punch_count = uint16_t(std::clamp<int>(c.value, 0, song_punches)); break;
    case Type::SongRecord: song_recording = c.value != 0; break;
    case Type::PunchOn: {
        unsigned pad = c.index >> 8;
        PendingPunch p {true, c.track, PunchKind((c.index & 0xff) % punch_count), std::clamp<int>(c.value, 0, 100) / 100.0f,
                        unsigned(c.pattern & 3), unsigned(c.pattern >> 4) % 3};
        if (pad) { // one pad alone, at once: the pad's hit is the start
            if (c.track >= track_count) break;
            if (pad_punch_track >= 0 && pad_punch_track != c.track) slots[size_t(pad_punch_track)].sampler.punch_pad(-1, nullptr);
            pad_punch_track = c.track; pad_punch_pad = int(pad - 1);
            slot.sampler.punch_pad(pad_punch_pad, &pad_punch);
            double grid = bar_length() / (4u << p.snap);
            pad_punch.start(p.kind, p.depth, bar_length(), grid, p.tail, params_of(p.kind));
            break;
        }
        unsigned s = c.position ? (c.position - 1) % live_slots : 0;
        live_punch = true;
        if (punch_from_song) { punch.stop(); punch_from_song = false; } // a punch by hand wins over the song's
        if (running) live[s].pending = p;
        else start_live(s, p);
        break;
    }
    case Type::PunchOff:
        if (c.index >> 8) { if (int(c.index >> 8) - 1 == pad_punch_pad && c.track == pad_punch_track) pad_punch.stop(); break; }
        for (unsigned s = 0; s < live_slots; ++s) {
            if (c.track != 254 && (c.position ? (c.position - 1) % live_slots : 0) != s) continue;
            live[s].pending.waiting = false; // let go before it began
            live[s].punch.stop();
        }
        live_punch = false;
        for (auto& l : live) live_punch |= l.punch.on() || l.pending.waiting;
        break;
    case Type::PunchDepth:
        if (c.index >> 8) pad_punch.set_depth(std::clamp<int>(c.value, 0, 100) / 100.0f);
        else
            for (unsigned s = 0; s < live_slots; ++s) {
                if (c.position && (c.position - 1) % live_slots != s) continue; // 0: every slot (K1 depth)
                live[s].punch.set_depth(std::clamp<int>(c.value, 0, 100) / 100.0f);
                live[s].pending.depth = std::clamp<int>(c.value, 0, 100) / 100.0f;
            }
        break;
    case Type::PunchParam: {
        if (c.index >= 16 || c.pattern >= punch_params) break;
        mpc_params[c.index][c.pattern] = c.value;
        auto kind = PunchKind(punch_mpc_first + c.index);
        for (auto& l : live) if (l.punch.kind_now() == kind) l.punch.set_params(mpc_params[c.index].data());
        if (pad_punch.kind_now() == kind) pad_punch.set_params(mpc_params[c.index].data());
        break;
    }
    case Type::FlexBeat:
        switch (c.index) {
        case 0:
            flex_pad = uint8_t(c.value % int(flex_pads));
            flex_waiting = false;
            if (!flex_pad) { flex.stop(); flex_now.store(0, std::memory_order_relaxed); break; }
            if (running && flex_quantize) flex_waiting = true; // on the next sixteenth
            else start_flex();
            break;
        case 1: flex_mode = uint8_t(c.value != 0); break;
        case 2: flex_mix = uint8_t(std::clamp<int>(c.value, 0, 100)); flex.set_depth(flex_mix / 100.0f); break;
        case 3: flex_quantize = c.value != 0; break;
        default: break;
        }
        break;
    case Type::Click:
        click = c.value != 0;
        click_level = std::clamp<int>(c.index, 0, 100) / 100.0f;
        break;
    case Type::Arp: {
        auto& a = slot.arp;
        switch (c.index) {
        case 0:
            a.on = c.value != 0;
            if (!a.on) { a.clear(); if (a.sounding >= 0 && slot.engine) slot.engine->note_off(a.sounding); a.sounding = -1; slot.arp_late.key = -1; }
            break;
        case 1: a.mode = ArpMode(c.value % int(ArpMode::Count)); break;
        case 2: a.rate = unsigned(c.value) % arp_rates; break;
        case 3: a.octaves = unsigned(std::clamp<int>(c.value, 1, 4)); break;
        case 4: a.gate = unsigned(std::clamp<int>(c.value, 5, 100)); break;
        }
        break;
    }
    case Type::ArpKey:
        if (c.value) slot.arp.press(c.index, c.value); else slot.arp.release(c.index);
        if (!slot.arp.count) slot.arp.position = 0;
        break;
    case Type::RecordArm: {
        if (take.state() != TakeRecorder::State::Idle) break;
        unsigned source = c.track % unsigned(InputSource::Count);
        if (source != source_now.load(std::memory_order_relaxed)) { source_now.store(source, std::memory_order_relaxed); recall_ring.restart(); }
        bool seq = c.pattern == 255;
        uint32_t length = 0;
        cue_track = -1;
        if (seq) { // the pattern's length; while it plays, from where it comes round
            unsigned t = c.position % track_count;
            length = uint32_t(std::max<uint32_t>(patterns[t][slots[t].pattern]->length, 1) * tick_length() + 0.5);
            if (running && !slots[t].silent && slots[t].engine) cue_track = int(t);
        } else if (c.pattern) {
            length = uint32_t(double(c.pattern) * bar * tick_length() + 0.5);
        }
        take.arm(float(c.value), length, c.index, cue_track >= 0, length ? 0 : auto_quiet, auto_silence_db);
        break;
    }
    case Type::RecordSource: {
        if (take.state() != TakeRecorder::State::Idle) break;
        unsigned source = c.track % unsigned(InputSource::Count), track = c.index % track_count;
        bool post = c.value != 0;
        bool same = source == source_now.load(std::memory_order_relaxed)
                 && (source != unsigned(InputSource::Track)
                     || (track == source_track.load(std::memory_order_relaxed) && post == source_post.load(std::memory_order_relaxed)));
        source_now.store(source, std::memory_order_relaxed);
        source_track.store(track, std::memory_order_relaxed);
        source_post.store(post, std::memory_order_relaxed);
        if (!same) recall_ring.restart(); // what came in before is another source's
        break;
    }
    case Type::RecordAutoEnd: auto_quiet = c.position; auto_silence_db = float(c.value); break;
    case Type::RecordGain: record_gain = std::pow(10.0f, std::clamp<int>(c.value, -24, 24) / 20.0f); break;
    case Type::RecordNow: take.start_now(); break;
    case Type::RecordStop: take.stop(); cue_track = -1; break;
    case Type::Monitor: monitor_on.store(c.value != 0, std::memory_order_relaxed); break;
    case Type::KnobFx: knob_command(c); break;
    case Type::Compressor: {
        MasterComp settings = comp_settings;
        switch (c.index) {
        case 0: settings.on = c.value != 0; break;
        case 1: settings.color = c.value != 0; break;
        case 2: settings.attack = uint8_t(c.value); break;
        case 3: settings.release = uint8_t(c.value); break;
        case 4: settings.amount = uint8_t(c.value); break;
        case 5: settings.boost = uint8_t(c.value); break;
        default: break;
        }
        comp_settings = settings;
        comp.set(settings);
        break;
    }
    case Type::BounceArm:
        if (running) break; // the shell stops first
        render_out.cue();
        bounce_song = c.value == 1;
        bounce_ticks = bounce_song ? 0 : std::max<uint32_t>(c.position, 1);
        break;
    case Type::PadNote:
        if (slot.engine == &slot.sampler) slot.sampler.play_pad(c.index, c.value, int(c.pattern) - 64, int(c.position) - 1);
        midi_note(c.track % track_count, int(sampler_first_note + c.index), c.value, frame_now, -1); // 16 Levels: the pad's note
        break;
    case Type::MidiTrack:
        if (!is_midi(c.track)) midi_all_off(c.track, frame_now); // what went out on the old channel ends there (a MIDI track keeps its own)
        out_channel[c.track % track_count] = uint8_t(std::min<unsigned>(c.index, 16));
        out_mode[c.track % track_count] = uint8_t(c.value);
        break;
    case Type::MidiSync:
        if (c.index == 0) sync_clock = c.value != 0;
        else if (c.index == 1) sync_mtc = c.value != 0;
        else if (c.index == 2) { sync_cv = c.value != 0; if (!sync_cv) emit({MidiOutEvent::Kind::CvLow, 1, 0, 0, 0, frame_now}); }
        else if (c.index == 3) sync_ppqn = unsigned(std::max<int>(c.value, 1));
        sync.set(sync_clock, sync_mtc, sync_cv, sync_ppqn);
        break;
    case Type::ExtSync:
        if (c.index == 0) {
            ext_on = c.value != 0;
            if (!ext_on) { ext_gate = false; ext_tempo = 0; ext_head = ext_tail = 0; }
            follow_tempo();
        } else if (c.index == 1) {
            if (ext_gate && (ext_head + 1) % ext_ring.size() != ext_tail) { ext_ring[ext_head] = c.position; ext_head = (ext_head + 1) % ext_ring.size(); }
        } else if (c.index == 2) {
            ext_tempo = uint16_t(std::clamp<int>(c.value, 300, 3000));
            follow_tempo();
        } else if (c.index == 3) {
            ext_gate = ext_on; ext_clocks = 0; ext_base = tick_now; ext_head = ext_tail = 0;
            sync.allow(0);
        } else if (c.index == 4) {
            ext_gate = false; ext_head = ext_tail = 0;
        }
        sync.set_external(ext_gate);
        ext_now.store(ext_gate, std::memory_order_relaxed);
        break;
    case Type::MtcIn: {
        if (c.index == 0) { mtc_off(); break; }
        if (!running) break;
        // Where the master is at this block's first frame: it was at `micros` when frame `position` was heard.
        int32_t ago = int32_t(uint32_t(frame_now) - c.position);
        double master = c.micros / 1e6 + ago / clock.frames_a_second();
        double error = master - mtc_at;
        if (c.index == 1 && !(mtc_on && std::fabs(error) < 0.02)) { mtc_place(master); break; } // the same place: no jump
        if (!mtc_on) break;
        if (std::fabs(error) > 0.1) { // a stray sample, or the song fell far behind (an overload): placed again
            if (++mtc_misses >= 4) mtc_place(master);
            break;
        }
        mtc_misses = 0;
        mtc_error += 0.1 * (error - mtc_error);                 // jitter averaged over about ten quarter frames
        speed = 1 + std::clamp(mtc_error * 2, -0.005, 0.005);  // a millisecond behind: 0.2 % faster until caught up
        mtc_error_now.store(int(mtc_error * 1e6), std::memory_order_relaxed);
        mtc_ppm.store(int(std::lround((speed - 1) * 1e6)), std::memory_order_relaxed);
        break;
    }
    case Type::Audition:
        if (slot.engine == &slot.sampler) slot.sampler.audition(c.pad.sample, c.pad.start, c.pad.end, c.value);
        break;
    case Type::Pressure: {
        if (!slot.engine) break;
        float value = std::clamp<int>(c.value, 0, 127) / 127.0f;
        slot.engine->set_pressure(c.index, value);
        // The arp's notes follow the pressure of the key they came from, while it is held.
        if (slot.arp.on) {
            slot.arp.press_harder(c.index, value);
            if (slot.arp_from == int(c.index) && slot.arp_played >= 0 && slot.arp_played != int(c.index))
                slot.engine->set_pressure(slot.arp_played, value);
        }
        break;
    }
    case Type::MotionClear: motion.clear(c.motion.track, c.motion.pattern, c.motion.param, c.motion.pad); break;
    case Type::Wheel:
        slot.wheel = std::clamp(c.value / 1000.0f, 0.0f, 1.0f);
        if (slot.engine) slot.engine->set_wheel(slot.wheel);
        break;
    case Type::Level: slot.gain = level_gain(c.value); break;
    case Type::Pan: {
        float angle = (std::clamp<int>(c.value, -50, 50) + 50) / 100.0f * dsp::pi / 2;
        slot.pan_left = std::cos(angle); slot.pan_right = std::sin(angle);
        break;
    }
    case Type::Send: slot.send = c.value / 100.0f; break;
    case Type::Mute: slot.mute = c.value != 0; midi_silenced(); break;
    case Type::Solo: slot.solo = c.value != 0; midi_silenced(); break;
    case Type::PatternData:
        if (c.data) {
            auto& at = patterns[c.track % track_count][c.pattern % pattern_count];
            Pattern* old = at;
            at = c.data;
            retire(old);
        }
        break;
    case Type::Pattern:
        if (running && !song_mode) slot.queued = uint8_t(c.value % pattern_count);
        else if (!song_mode) { slot.pattern = uint8_t(c.value % pattern_count); slot.queued = 255; lead = slot.pattern; follow_tempo(); }
        patterns_now[c.track % track_count].store(slot.pattern, std::memory_order_relaxed);
        break;
    case Type::SongClip: if (c.index < song_clips) song.clips[c.index] = c.clip; break;
    case Type::SongCount: song.count = uint16_t(std::clamp<int>(c.value, 0, song_clips)); break;
    case Type::SongLoop: song.loop_start = c.index; song.loop_end = uint16_t(c.value); break;
    case Type::Play:
        if (running) break;
        mtc_off(); // MTC in places it right after (MtcIn locate)
        running = true; until_tick = 0;
        tick_now = c.value == 1 ? 0 : c.position;
        arp_ticks = 0; until_arp = 0; // the repeats line up with the bar, so their swing falls with the sequencer's
        song_mode = c.value == 1; song_bar_now = c.index;
        count_left = c.track == 1 ? bar : 0;
        count_beat.store(count_left ? 1 : 0, std::memory_order_relaxed);
        loop_restart = true;
        for (auto& s : slots) {
            s.start = 0; s.clip = -1; s.silent = song_mode;
            if (s.queued != 255) { s.pattern = s.queued; s.queued = 255; }
        }
        in_song.store(song_mode, std::memory_order_relaxed);
        if (render_out.cued()) { // a render starts with the first frame played
            if (bounce_song != song_mode || count_left) render_out.cancel();
            else render_out.begin(0);
        }
        break;
    case Type::Stop:
        running = false;
        mtc_off();
        sync_stop(frame_now);
        midi_all_off(255, frame_now);
        render_out.cancel(); // a render stopped by hand is dropped; one in its tail ends there
        if (take.state() == TakeRecorder::State::Cued) take.stop();
        cue_track = -1;
        count_left = 0; count_beat.store(0, std::memory_order_relaxed);
        if (punch_from_song) { punch.stop(); punch_from_song = false; }
        song_mode = false;
        in_song.store(false, std::memory_order_relaxed);
        for (unsigned t = 0; t < track_count; ++t) {
            positions_now[t].store(-1, std::memory_order_relaxed);
            for (auto& skip : slots[t].skips) skip.key = -1;
            if (slots[t].queued != 255) { slots[t].pattern = slots[t].queued; slots[t].queued = 255; }
            slots[t].silent = false; slots[t].clip = -1;
            patterns_now[t].store(slots[t].pattern, std::memory_order_relaxed);
        }
        for (auto& s : slots) restore(s);
        for (auto& s : slots) {
            for (auto& off : s.offs) if (off.key >= 0 && s.engine) { s.engine->note_off(off.key); off.key = -1; }
            s.held = {};
        }
        break;
    case Type::Tempo:
        global_tempo = uint16_t(std::clamp<int>(c.value, 300, 3000));
        follow_tempo();
        break;
    case Type::PatternTempo:
        pattern_tempo[c.index % pattern_count] = uint16_t(c.value ? std::clamp<int>(c.value, 300, 3000) : 0);
        follow_tempo();
        break;
    case Type::SeqTempo:
        seq_tempo = c.value != 0; lead = uint8_t(c.index % pattern_count);
        follow_tempo();
        break;
    case Type::Signature:
        beat = std::max<unsigned>(c.index, 1); bar = std::max<unsigned>(uint16_t(c.value), beat);
        break;
    case Type::Locate:
        if (!running || song_mode) break;
        sync_stop(frame_now); // the clocks go on from the new place: Song Position and Continue
        tick_now = c.position; until_tick = 0; // the new place plays at once
        loop_restart = true;
        for (auto& s : slots) { s.start = 0; for (auto& skip : s.skips) skip.key = -1; }
        tick_count.store(tick_now, std::memory_order_relaxed);
        break;
    case Type::Recorded:
        for (auto& skip : slot.skips)
            if (skip.key < 0 || tick_now > skip.until) {
                skip = {int16_t(c.index), c.pattern, c.position, tick_now + 2 * ticks_per_quarter};
                break;
            }
        break;
    case Type::Swing: swing = uint8_t(std::clamp<int>(c.value, 50, 75)); break;
    }
}

void EngineHost::schedule_off(Slot& slot, int key, int32_t samples) {
    for (auto& off : slot.offs)
        if (off.key == key) { off.samples = samples; return; } // a retrigger moves its end
    for (auto& off : slot.offs)
        if (off.key < 0) { off.key = key; off.samples = samples; return; }
    slot.engine->note_off(key); // no room: end it now rather than hang
}

void EngineHost::cancel_off(Slot& slot, int key) {
    for (auto& off : slot.offs) if (off.key == key) off.key = -1;
}

void EngineHost::run_offs(Slot& slot, unsigned frames) {
    for (auto& off : slot.offs) {
        if (off.key < 0) continue;
        off.samples -= int32_t(frames);
        if (off.samples <= 0) {
            if (slot.engine) slot.engine->note_off(off.key);
            if (off.key == slot.held.key) slot.held = {}; // nothing took it over
            off.key = -1;
        }
    }
}

void EngineHost::run_chain(unsigned chain, float* left, float* right, unsigned frames) {
    for (auto& effect : fx[chain]) {
        if (effect.kind() == EffectKind::None) break; // slots are packed from the front
        effect.process(left, right, frames);
    }
}

// Back to the track's own values once motion has moved them.
void EngineHost::restore(Slot& slot) {
    if (slot.fx_moved) {
        unsigned t = unsigned(&slot - slots.data());
        for (unsigned k = 0; k < max_slots; ++k)
            for (unsigned q = 0; q < effect_params; ++q) fx[t][k].set_param(q, slot.fx_base[k][q]);
        slot.fx_moved = false;
    }
    if (slot.kind == EngineKind::Sampler) slot.sampler.restore_pads(); // a kit pad's own values, if motion moved them
    if (slot.wheel_moved && slot.engine) slot.engine->set_wheel(slot.wheel); // the mod wheel, back where the fader left it
    slot.wheel_moved = false;
    if (!slot.moved || !slot.engine) return;
    for (unsigned p = 0; p < max_params; ++p) slot.engine->set_param(int(p), slot.base[p]);
    slot.moved = false;
}

// Motion at sixteenth `at` of the track's pattern: each lane starts the
// pattern at the track's own value, then follows its events.
void EngineHost::play_motion(unsigned t, Slot& slot, unsigned at) {
    if (at == 0) restore(slot);
    for (unsigned n = 0; n < motion.count; ++n) {
        const auto& e = motion.events[n];
        if (e.track != t || e.pattern != slot.pattern || e.step != at) continue;
        if (e.param == motion_wheel) { // the mod wheel's lane (parity phase 8)
            if (slot.engine) { slot.engine->set_wheel(std::clamp(e.value / 127.0f, 0.0f, 1.0f)); slot.wheel_moved = true; }
            continue;
        }
        if (e.param >= motion_pad_base) { // a kit pad's own value
            if (slot.kind == EngineKind::Sampler) slot.sampler.move_pad(e.pad, PadParam(e.param - motion_pad_base), e.value);
            continue;
        }
        if (e.param >= motion_fx_base) { // one of the track's effects
            unsigned k = (e.param - motion_fx_base) / effect_params, q = (e.param - motion_fx_base) % effect_params;
            if (k < max_slots) { fx[t][k].set_param(q, e.value); slot.fx_moved = true; }
            continue;
        }
        slot.engine->set_param(e.param, e.value);
        if (slot.kind == EngineKind::Midi) midi_param(t, e.param, e.value, frame_now + block_at); // a CC lane plays out
        slot.moved = true;
    }
}

// Swing stretches the first sixteenth of every eighth and shortens the second.
double EngineHost::tick_samples(uint32_t at) const {
    double base = tick_length();
    return (at % (2 * sixteenth_ticks)) < sixteenth_ticks ? base * swing / 50.0 : base * (100 - swing) / 50.0;
}

void EngineHost::bar_start() {
    if (!song_mode) {
        for (unsigned t = 0; t < track_count; ++t) {
            auto& slot = slots[t];
            if (slot.queued == 255) continue;
            if (slot.kind == EngineKind::Midi && slot.queued != slot.pattern) midi_all_off(t, frame_now + block_at); // the old pattern's notes end
            slot.pattern = slot.queued; slot.queued = 255; slot.start = tick_now;
            patterns_now[t].store(slot.pattern, std::memory_order_relaxed);
            lead = slot.pattern;
        }
        follow_tempo(); // BPM per pattern: the one launched last leads
        return;
    }
    if (tick_now) ++song_bar_now;
    bool rendering = bounce_song && render_out.running(); // the mixdown plays the song once, loop or not
    if (!rendering && song.looping() && song_bar_now == song.loop_end) song_bar_now = song.loop_start;
    if ((rendering || !song.looping()) && !song_recording && song_bar_now >= std::max(song.length(), 1u)) { // the end of the song
        running = false;
        if (rendering) render_out.tail(); // on until the mix falls silent
        for (auto& s : slots) { for (auto& off : s.offs) if (off.key >= 0 && s.engine) { s.engine->note_off(off.key); off.key = -1; } s.held = {}; }
        return;
    }
    clips_at_bar(false);
}

void EngineHost::clips_at_bar(bool force) {
    bar_now.store(song_bar_now, std::memory_order_relaxed);
    uint32_t bar_tick = tick_now - tick_now % bar; // the tick this bar began on
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        int index = song.clip_at(t, song_bar_now);
        slot.silent = index < 0 || song.clips[size_t(index)].muted;
        if (slot.silent) { slot.clip = -1; positions_now[t].store(-1, std::memory_order_relaxed); continue; }
        const auto& clip = song.clips[size_t(index)];
        if (force || index != slot.clip || song_bar_now == clip.bar) {
            // A clip starts (or the loop came back into it): its pattern from where the clip says.
            slot.clip = index;
            slot.pattern = clip.pattern;
            slot.start = bar_tick - (song_bar_now - clip.bar) * bar;
            patterns_now[t].store(slot.pattern, std::memory_order_relaxed);
        }
    }
}

void EngineHost::mtc_off() {
    mtc_on = false; speed = 1; mtc_error = 0; mtc_misses = 0;
    mtc_now.store(false, std::memory_order_relaxed);
    mtc_error_now.store(0, std::memory_order_relaxed); mtc_ppm.store(0, std::memory_order_relaxed);
}

// MTC in: time to the song through the tempo playing now (BPM per pattern: the lead pattern's; in the song, the one it
// plays at). Patterns: every track from the patterns' start, as Locate (the tick since Play, each track's pattern round
// as many times as fits). Song: the bar and the tick in it, round the loop as the song plays it; past its end, it stops.
void EngineHost::mtc_place(double seconds) {
    double ticks = std::max(0.0, seconds) * tempo / 10.0 / 60.0 * ticks_per_quarter;
    auto at = uint32_t(std::ceil(ticks - 1e-9));
    sync_stop(frame_now); // the clocks out go on from the new place
    count_left = 0; count_beat.store(0, std::memory_order_relaxed);
    until_tick = (at - ticks) * tick_length(); // the tick after the place, to the sample
    for (auto& s : slots) for (auto& skip : s.skips) skip.key = -1;
    if (song_mode) {
        unsigned b = at / bar, offset = at % bar;
        if (song.looping() && b >= song.loop_end) b = song.loop_start + (b - song.loop_start) % (song.loop_end - song.loop_start);
        tick_now = b * bar + offset;
        for (auto& s : slots) s.clip = -1; // every clip from the new place
        if (offset == 0) {
            song_bar_now = b ? b - 1 : 0; // the first tick's bar_start() moves it on to b (and finds its end)
        } else if (!song.looping() && !song_recording && b >= std::max(song.length(), 1u)) {
            running = false; // past the song's end
            for (auto& s : slots) { for (auto& off : s.offs) if (off.key >= 0 && s.engine) { s.engine->note_off(off.key); off.key = -1; } s.held = {}; }
        } else {
            song_bar_now = b;
            clips_at_bar(true);
        }
    } else {
        tick_now = at;
        for (auto& s : slots) s.start = 0;
    }
    tick_count.store(tick_now, std::memory_order_relaxed);
    mtc_on = running; mtc_at = seconds; mtc_error = 0; speed = 1; mtc_misses = 0;
    mtc_now.store(mtc_on, std::memory_order_relaxed);
    mtc_placed.fetch_add(1, std::memory_order_relaxed);
}

void EngineHost::arp_tick() {
    for (auto& slot : slots) {
        auto& a = slot.arp;
        if (!a.on || !slot.engine) continue;
        unsigned rate = arp_rate_ticks(a.rate);
        if (arp_ticks % rate) continue;
        int velocity = 0, key = a.next(velocity);
        if (key < 0) continue;
        auto length = std::max<int32_t>(64, int32_t(arp_tick_length() * rate * a.gate / 100.0));
        double late = arp_swing_ticks(a.rate, (arp_ticks % (4 * arp_ticks_per_quarter)) / rate, swing); // every second straight note, as the project swings
        if (late > 0) { slot.arp_late = {int16_t(key), int16_t(velocity), int16_t(a.from), int32_t(arp_tick_length() * late), length}; continue; }
        arp_note(slot, key, velocity, a.from, length);
    }
    ++arp_ticks;
}

void EngineHost::run_late(Slot& slot, unsigned frames) {
    auto& late = slot.arp_late;
    if (late.key < 0) return;
    late.samples -= int32_t(frames);
    if (late.samples > 0) return;
    if (slot.engine && slot.arp.on) arp_note(slot, late.key, late.velocity, late.from, late.length);
    late.key = -1;
}

void EngineHost::arp_note(Slot& slot, int key, int velocity, int from, int32_t length) {
    slot.engine->note_on(key, velocity);
    float pressure = slot.arp.pressure_of(from);
    if (pressure > 0) slot.engine->set_pressure(key, pressure);
    slot.arp_played = key; slot.arp_from = from;
    schedule_off(slot, key, length);
    midi_note(unsigned(&slot - slots.data()), key, velocity, frame_now, length);
}

// --- MIDI out and sync (parity phase 9) ---

// Pad MIDI Out: Always, or Empty for a note nothing here would sound (a kit
// pad without a sample, a track without an engine). Past 127 is no MIDI note.
bool EngineHost::out_wanted(unsigned t, int key) const {
    t %= track_count;
    if (key < 0 || key > 127) return false;
    if (is_midi(t)) return heard(t); // a MIDI track: always, unless muted (docs/MIDI_TRACKS.md)
    if (!out_channel[t] || !out_mode[t]) return false;
    if (out_mode[t] == 1) return true;
    const auto& slot = slots[t];
    if (!slot.engine) return true;
    if (slot.kind == EngineKind::Sampler) return key < int(sampler_first_note) || !slot.sampler.pad_used(unsigned(key) - sampler_first_note);
    return false;
}

bool EngineHost::heard(unsigned t) const {
    bool solo = false;
    for (const auto& s : slots) solo |= s.solo;
    const auto& slot = slots[t % track_count];
    return !slot.mute && !(solo && !slot.solo);
}

void EngineHost::midi_silenced() {
    for (unsigned t = 0; t < track_count; ++t)
        if (is_midi(t) && !heard(t)) midi_all_off(t, frame_now);
}

void EngineHost::midi_note(unsigned t, int key, int velocity, uint64_t at, int64_t length) {
    if (!out_wanted(t, key)) return;
    t %= track_count;
    // A MIDI track's own port and channel (its first two parameters), else Pad MIDI Out's channel on MIDI Config's port.
    bool own = is_midi(t);
    uint8_t channel = own ? uint8_t(std::clamp<int>(slots[t].base[midi_track::Channel], 1, 16)) : out_channel[t];
    MidiDest dest = own ? midi_track::dest_of(slots[t].base[midi_track::Port]) : MidiDest::Config;
    auto event = [&](MidiOutEvent::Kind kind, uint8_t b, uint64_t when) {
        MidiOutEvent e {kind, channel, uint8_t(key), b, 0, when};
        e.dest = dest;
        if (own) e.track = uint8_t(t);
        emit(e);
    };
    for (auto& off : midi_offs) // a retrigger ends the note it replaces first
        if (off.on && off.channel == channel && off.key == key && off.dest == dest) { event(MidiOutEvent::Kind::NoteOff, 0, at); off.on = false; }
    event(MidiOutEvent::Kind::NoteOn, uint8_t(std::clamp(velocity, 1, 127)), at);
    for (auto& off : midi_offs)
        if (!off.on) { off = {true, channel, uint8_t(key), uint8_t(t), length >= 0 ? at + uint64_t(length) : ~uint64_t(0), dest}; return; }
    if (length >= 0) event(MidiOutEvent::Kind::NoteOff, 0, at + uint64_t(length)); // no room: its off now, timed
}

void EngineHost::midi_param(unsigned t, int id, int value, uint64_t at) {
    using namespace midi_track;
    t %= track_count;
    if (!is_midi(t)) return;
    const auto& base = slots[t].base;
    if (is_program(id)) { midi_program(t, at); return; }
    if (!is_cc(id)) return;
    MidiOutEvent e {MidiOutEvent::Kind::Control, uint8_t(std::clamp<int>(base[Channel], 1, 16)),
                    uint8_t(std::clamp<int>(base[CcNumber1 + (id - Cc1)], 0, 127)), uint8_t(std::clamp(value, 0, 127)), 0, at};
    e.dest = dest_of(base[Port]); e.track = uint8_t(t);
    emit(e);
}

void EngineHost::midi_program(unsigned t, uint64_t at) {
    using namespace midi_track;
    t %= track_count;
    const auto& base = slots[t].base;
    if (!base[Program]) return; // Off: the device keeps its own
    uint8_t channel = uint8_t(std::clamp<int>(base[Channel], 1, 16));
    auto out = [&](MidiOutEvent::Kind kind, int a, int b) {
        MidiOutEvent e {kind, channel, uint8_t(a), uint8_t(b), 0, at};
        e.dest = dest_of(base[Port]); e.track = uint8_t(t);
        emit(e);
    };
    if (base[BankMsb]) out(MidiOutEvent::Kind::Control, 0, std::clamp<int>(base[BankMsb] - 1, 0, 127));
    if (base[BankLsb]) out(MidiOutEvent::Kind::Control, 32, std::clamp<int>(base[BankLsb] - 1, 0, 127));
    out(MidiOutEvent::Kind::Program, std::clamp<int>(base[Program] - 1, 0, 127), 0);
}

void EngineHost::midi_note_off(unsigned t, int key, uint64_t at) {
    for (auto& off : midi_offs)
        if (off.on && off.track == t % track_count && off.key == key) {
            emit(off_event(off, at));
            off.on = false;
        }
}

bool EngineHost::midi_hold(unsigned t, int key, int64_t length, uint64_t at) {
    if (!out_wanted(t, key)) return true; // nothing went out to hold
    for (auto& off : midi_offs)
        if (off.on && off.track == t % track_count && off.key == key) { off.at = at + uint64_t(std::max<int64_t>(length, 0)); return true; }
    return false;
}

void EngineHost::midi_offs_due(uint64_t until) {
    for (auto& off : midi_offs)
        if (off.on && off.at < until) { emit(off_event(off, off.at)); off.on = false; }
}

void EngineHost::midi_all_off(unsigned track, uint64_t at) {
    for (auto& off : midi_offs)
        if (off.on && (track == 255 || off.track == track % track_count)) {
            emit(off_event(off, at));
            off.on = false;
        }
}

void EngineHost::sync_stop(uint64_t at) {
    if (sync_started) sync.stop(at, [&](const MidiOutEvent& e) { emit(e); });
    sync_started = false;
}

// MIDI clock in: each pulse lets the sequencer through four more ticks once
// its frame comes (a block after it arrived, to the frame: no block jitter).
void EngineHost::ext_release(uint64_t at) {
    while (ext_tail != ext_head && int32_t(ext_ring[ext_tail] - uint32_t(at)) <= 0) {
        ext_tail = (ext_tail + 1) % ext_ring.size();
        ++ext_clocks;
        sync.allow(ext_clocks);
        // Far behind the clock (a tempo jump): catch up at once, allowing for swing's late sixteenths.
        uint32_t allowed = ext_base + (ext_clocks - 1) * 4;
        if (running && !count_left && tick_now + 16 < allowed) until_tick = std::min(until_tick, -double(allowed - tick_now) * tick_length());
    }
}

void EngineHost::start_live(unsigned s, const PendingPunch& p) {
    auto& l = live[s % live_slots];
    if (p.target != l.target) { l.punch.clear(); l.target = p.target; } // a new signal: the old history is not its own
    l.punch.start(p.kind, p.depth, bar_length(), bar_length() / (4u << p.snap), p.tail, params_of(p.kind));
}

void EngineHost::run_live(uint8_t target, float* left, float* right, unsigned frames) {
    for (auto& l : live) if (l.target == target && l.punch.active()) l.punch.process(left, right, frames);
}

void EngineHost::start_flex() {
    const auto& info = flex_beat(flex_pad);
    flex.start(info.kind, flex_mix / 100.0f, bar_length(), bar_length() / (4u << info.grid), 1);
    flex_left = bar_length();
    flex_waiting = false;
    flex_now.store(flex_pad, std::memory_order_relaxed);
}

void EngineHost::start_punch(const PendingPunch& p) {
    if (p.target != punch_target) { punch.clear(); punch_target = p.target; } // a new signal: the old history is not its own
    punch.start(p.kind, p.depth, bar_length(), bar_length() / (4u << p.snap), p.tail, params_of(p.kind));
}

void EngineHost::play_song_punches() {
    if (punch_from_song && tick_now >= song_punch_until) { punch.stop(); punch_from_song = false; }
    if (live_punch) return; // a punch held by hand wins
    unsigned step = (tick_now % bar) / sixteenth_ticks;
    for (unsigned n = 0; n < song.punch_count; ++n) {
        const auto& e = song.punches[n];
        if (e.bar != song_bar_now || e.step != step) continue;
        start_punch({false, e.target, PunchKind(e.kind % punch_count), e.depth / 100.0f, 2, 1});
        punch_from_song = true;
        song_punch_until = tick_now + std::max<uint32_t>(1, e.length) * sixteenth_ticks;
    }
}

void EngineHost::tick() {
    // Count-In: a bar of the metronome before the song starts.
    if (count_left) {
        unsigned done = bar - count_left;
        if (done % beat == 0) {
            click_env = 1; click_phase = 0;
            click_step = (done == 0 ? 1568.0f : 1046.0f) / dsp::sample_rate();
            count_beat.store(done / beat + 1, std::memory_order_relaxed);
        }
        --count_left;
        if (!count_left) count_beat.store(0, std::memory_order_relaxed);
        until_tick += tick_length();
        return;
    }
    // The song's clocks start with its first tick, after any count-in: Start from the top, else Song Position and Continue.
    if (!sync_started) {
        sync_started = true;
        uint32_t from = song_mode ? song_bar_now * bar + tick_now % bar : tick_now;
        sync.start(frame_now + block_at, double(from) / ticks_per_quarter, from * tick_length() / clock.frames_a_second(), quarter_samples(),
                   [&](const MidiOutEvent& e) { emit(e); });
    }
    // Punches waiting for their grid, and Flex Beat for its sixteenth.
    for (unsigned s = 0; s < live_slots; ++s) {
        auto& p = live[s].pending;
        if (p.waiting && tick_now % (4 * ticks_per_quarter / (4u << p.snap)) == 0) { p.waiting = false; start_live(s, p); }
    }
    if (flex_waiting && tick_now % sixteenth_ticks == 0) start_flex();
    // The metronome: every beat while playing, higher on the bar.
    if (click && tick_now % beat == 0) {
        click_env = 1; click_phase = 0;
        click_step = (tick_now % bar == 0 ? 1568.0f : 1046.0f) / dsp::sample_rate();
    }
    // A pattern render ends after its ticks, on the frame.
    if (!bounce_song && render_out.running() && tick_now >= bounce_ticks) render_out.finish(block_at);
    if (tick_now % bar == 0) bar_start();
    if (!running) return;
    if (song_mode && tick_now % sixteenth_ticks == 0) play_song_punches();
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        bool sound = slot.engine != nullptr, midi = out_channel[t] && out_mode[t]; // an empty track may still play MIDI out
        if ((!sound && !midi) || slot.silent) continue;
        const auto& pattern = *patterns[t][slot.pattern];
        uint32_t length = std::max<uint32_t>(pattern.length, 1);
        uint32_t at = (tick_now - slot.start) % length;
        positions_now[t].store(int(at), std::memory_order_relaxed);
        if (at == 0 && int(t) == cue_track) { take.go(block_at); cue_track = -1; } // Rec length Seq: the take starts with the pattern
        if (sound && motion.count && at % sixteenth_ticks == 0) play_motion(t, slot, at / sixteenth_ticks);
        const auto& events = pattern.events;
        Slot::Held holding; // this tick's own Slide or Tie, for the notes of a later one
        for (size_t n = pattern.lower(at); n < events.size() && events[n].tick == at; ++n) {
            const auto& e = events[n];
            if (e.key > 127 && slot.kind != EngineKind::Sampler) continue; // past 127: kit pads only
            const int played = pad_note(e.key, e.pitch); // the key, and over it what a kit pad plays at
            if (skipped(slot, unsigned(played), at)) continue;
            random = random * 1664525u + 1013904223u;
            if (e.chance < 100 && (random >> 8) % 100 >= e.chance) continue;
            // Held for its length, a touch short; with Slide or Tie, until the next note starts (and past
            // it, so that note finds it held and takes it over: no new attack; a slide glides there).
            bool legato = e.flags & (step_slide | step_tie);
            double ticks = e.length - std::min<unsigned>(pattern.q(), e.length) * 0.1;
            if (legato) {
                size_t next = n + 1;
                while (next < events.size() && events[next].tick == at) ++next;
                uint32_t to = next < events.size() && events[next].tick < length ? events[next].tick : events.front().tick + length;
                ticks = double(to - at) + sixteenth_ticks;
            }
            auto samples = std::max<int32_t>(int32_t(tick_length() * ticks), 64);
            // Accent: louder, and harder where velocity moves the sound.
            bool accent = e.flags & step_accent;
            int velocity = accent ? std::min(127, e.velocity + 24) : e.velocity;
            auto from = slot.held; // the note held into this tick takes this one, the first note on it
            slot.held = {};
            if (sound) {
                slot.engine->set_accent(accent ? expression::accent_gain : 1.0f);
                if (from.key >= 0) {
                    slot.engine->slide(from.key, played, velocity, from.glide);
                    if (from.key != played) cancel_off(slot, from.key);
                } else {
                    slot.engine->note_on(played, velocity);
                }
                slot.engine->set_accent(1);
                schedule_off(slot, played, samples);
            }
            // MIDI: a tie onto the same key goes on; else the notes overlap, which a mono synth plays legato.
            if (from.midi != e.key || !midi_hold(t, e.key, samples, frame_now + block_at))
                midi_note(t, e.key, velocity, frame_now + block_at, samples);
            if (legato) holding = {int16_t(played), int16_t(e.key), (e.flags & step_slide) != 0};
        }
        if (holding.key >= 0) slot.held = holding;
    }
    until_tick += tick_samples(tick_now);
    ++tick_now;
    tick_count.store(tick_now, std::memory_order_relaxed);
}

// The input source, as Sample Record, Audio Recall and Monitor hear it, after the gain.
void EngineHost::select_input(const int32_t* input, const float* usb, unsigned frames) {
    auto source = InputSource(source_now.load(std::memory_order_relaxed));
    constexpr float scale = 1.0f / 2147483648.0f;
    for (unsigned f = 0; f < frames; ++f) {
        float l = 0, r = 0;
        if (source == InputSource::Resample) { l = master_l[f]; r = master_r[f]; }
        else if (source == InputSource::Track) { l = tap_l[f]; r = tap_r[f]; }
        else if (from_usb(source)) { if (usb) { l = usb[2 * f]; r = usb[2 * f + 1]; } }
        else if (input) { l = input[2 * f] * scale; r = input[2 * f + 1] * scale; }
        switch (source) {
        case InputSource::Mic: l = r = (l + r) * 0.5f; break; // one widget on both ADC sides: their average
        case InputSource::RearL: case InputSource::UsbL: r = l; break;
        case InputSource::RearR: case InputSource::UsbR: l = r; break;
        default: break;
        }
        record_l[f] = l * record_gain; record_r[f] = r * record_gain;
    }
}

void EngineHost::knob_targets() {
    for (unsigned t = 0; t < track_count; ++t)
        slots[t].sampler.set_knob_pads(knob_heard ? knob_pads[t][0] : 0, knob_heard ? knob_pads[t][1] : 0);
}

void EngineHost::knob_command(const Command& c) {
    switch (c.index) {
    case 0: knob.set_kind(EffectKind(c.value), pool); knob_in.set_kind(EffectKind(c.value), pool); break;
    case 1: knob.set_param(c.pattern, c.value); knob_in.set_param(c.pattern, c.value); break;
    case 2: knob_heard = c.value & 1; knob_rec = (c.value & 2) != 0; break;
    case 3: knob_tracks = uint16_t(c.position); break;
    case 4: {
        if (c.track >= track_count || c.pattern > 3) break;
        auto& word = knob_pads[c.track][c.pattern / 2];
        unsigned shift = (c.pattern % 2) * 32;
        word = (word & ~(uint64_t(0xffffffffu) << shift)) | (uint64_t(c.position) << shift);
        break;
    }
    default: break;
    }
    knob_targets();
}

void EngineHost::render(int32_t* out, unsigned frames, float volume, const int32_t* input, float* stems, const float* usb) {
    static thread_local bool flushed = false;
    if (!flushed) { flush_denormals(); flushed = true; }
    for (auto& q : limbo) if (q && retired_patterns.push(q)) q = nullptr; // retired copies the queue had no room for
    pager.block();
    // Samples first: a command sent after a sample was offered (the browser's
    // preview, played as soon as it is in) must find the new one, not be cut by it.
    Offer offer;
    while (offers.pop(offer)) {
        if (offer.slot > preview_slot) continue;
        for (auto& slot : slots) slot.sampler.sample_gone(offer.slot); // nothing may play the old one past here
        const SampleData* old = samples[offer.slot];
        samples[offer.slot] = offer.sample;
        if (old && !retired.push(old)) samples[offer.slot] = old; // no room to give it back: keep it, try later
    }
    // Where this block sits against CLOCK_MONOTONIC, for MIDI out and in.
    clock.stamp(int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()), frame_now);
    Command command;
    for (unsigned n = 0; n < 4096 && queue.pop(command); ++n) apply(command);
    frames = std::min(frames, block);
    if (reformatting) { // a new rate: silent until the Reload has built everything again at it
        std::fill(out, out + 2 * frames, 0);
        if (stems) std::fill(stems, stems + size_t(track_count) * block, 0.0f);
        frame_now += frames;
        return;
    }
    // The clock tempo-synced effects follow: quarters since Play (on from where it was while stopped).
    effect_clock.quarter = tick_length() * ticks_per_quarter;
    if (running) effect_clock.beats = (double(tick_now) - std::max(until_tick, 0.0) / tick_length()) / ticks_per_quarter;
    // The arps' clock, at the block's start (within 3 ms of the beat).
    until_arp -= frames;
    while (until_arp <= 0) { arp_tick(); until_arp += arp_tick_length(); }
    bool solo = false;
    for (auto& strip : loop_strips) solo |= strip.solo && loops.on();
    for (auto& slot : slots) {
        solo |= slot.solo;
        std::fill(slot.buffer, slot.buffer + frames, 0.0f);
        std::fill(slot.left, slot.left + frames, 0.0f); std::fill(slot.right, slot.right + frames, 0.0f);
    }

    // The Looper's time is the sequencer's at the block's first frame (run_loops() comes after the block is played).
    loop_running = running; loop_counting = count_left > 0;
    loop_tick = running ? double(tick_now) - until_tick / tick_length() : 0;
    // Render up to each step, play the step, carry on: steps land on the sample.
    unsigned done = 0;
    while (done < frames) {
        unsigned n = frames - done;
        bool held = false; // following MIDI clock in: the next tick waits for its pulse
        if (running) {
            ext_release(frame_now + done);
            if (until_tick <= 0) { // a tick is due: it plays, unless MIDI clock in has not let it through yet
                held = ext_gate && !count_left && tick_now >= ext_base + ext_clocks * 4;
                if (!held) { block_at = done; tick(); continue; }
                if (ext_tail != ext_head) n = std::clamp<unsigned>(unsigned(int32_t(ext_ring[ext_tail] - uint32_t(frame_now + done))), 1, n);
            } else {
                n = std::min<unsigned>(n, unsigned(std::ceil(until_tick / speed)));
            }
        }
        for (auto& slot : slots) {
            if (!slot.engine) continue;
            run_offs(slot, n);
            run_late(slot, n);
            if (slot.engine->stereo()) slot.engine->render_stereo(slot.left + done, slot.right + done, n);
            else slot.engine->render(slot.buffer + done, n);
        }
        sync.run(frame_now + done, n, quarter_samples(), [&](const MidiOutEvent& e) { emit(e); });
        if (running && !held) {
            until_tick -= n * speed;                                     // MTC in trims the pace; 1 otherwise
            if (mtc_on) mtc_at += n * speed / clock.frames_a_second();
        }
        done += n;
    }
    midi_offs_due(frame_now + frames);
    if (!running && sync_started) sync_stop(frame_now + frames); // the song came to its end
    if (!running && mtc_on) mtc_off();                           // and MTC in no longer leads it
    is_playing.store(running, std::memory_order_relaxed);

    // The signal: each track panned, through its chain, into the master and
    // its send into one bus; each bus through its chain at its return level
    // into the master; the master through its chain, then main volume.
    for (unsigned b = 0; b < bus_count; ++b) { std::fill(bus_l[b], bus_l[b] + frames, 0.0f); std::fill(bus_r[b], bus_r[b] + frames, 0.0f); }
    std::fill(master_l, master_l + frames, 0.0f); std::fill(master_r, master_r + frames, 0.0f);
    std::fill(knob_l, knob_l + frames, 0.0f); std::fill(knob_r, knob_r + frames, 0.0f);
    bool knob_fed = false;
    // The Track input source: one track on its own, the engine alone (pre) or as it leaves its chain (post).
    bool tapping = source_now.load(std::memory_order_relaxed) == unsigned(InputSource::Track);
    unsigned tapped = source_track.load(std::memory_order_relaxed) % track_count;
    bool tap_post = source_post.load(std::memory_order_relaxed);
    if (tapping) { std::fill(tap_l, tap_l + frames, 0.0f); std::fill(tap_r, tap_r + frames, 0.0f); }
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        float level = 0;
        float* stem = stems ? stems + size_t(t) * block : nullptr;
        if (stem) std::fill(stem, stem + frames, 0.0f);
        bool tap = tapping && t == tapped;
        if (slot.engine) {
            if (tap && !tap_post) { // before the track's level, pan and chain; a mono engine on both sides
                const float* l = slot.engine->stereo() ? slot.left : slot.buffer;
                const float* r = slot.engine->stereo() ? slot.right : slot.buffer;
                std::copy(l, l + frames, tap_l); std::copy(r, r + frames, tap_r);
            }
            if (slot.engine->stereo()) { // its own stereo picture; the track's pan balances it
                float l = slot.gain * slot.pan_left * 1.4142f, r = slot.gain * slot.pan_right * 1.4142f;
                for (unsigned f = 0; f < frames; ++f) { slot.left[f] *= l; slot.right[f] *= r; }
            } else {
                for (unsigned f = 0; f < frames; ++f) {
                    float x = slot.buffer[f] * slot.gain;
                    slot.left[f] = x * slot.pan_left; slot.right[f] = x * slot.pan_right;
                }
            }
            run_chain(t, slot.left, slot.right, frames);
            if (punch_target == t) punch.process(slot.left, slot.right, frames);
            run_live(uint8_t(t), slot.left, slot.right, frames);
            if (tap && tap_post) { std::copy(slot.left, slot.left + frames, tap_l); std::copy(slot.right, slot.right + frames, tap_r); }
            // Monitored, the tapped track is heard through the input (gain, Rec Input FX) instead: once, as it records.
            bool through_input = tap && monitor_on.load(std::memory_order_relaxed);
            bool heard = !slot.mute && !(solo && !slot.solo) && !through_input;
            // Knob FX: a kit's pads switched on come out apart, after their own chain (not the track's), at the track's level and pan.
            if (heard && slot.engine == &slot.sampler && slot.sampler.knob_fed()) {
                const float* kl = slot.sampler.knob_left();
                const float* kr = slot.sampler.knob_right();
                float l = slot.gain * slot.pan_left * 1.4142f, r = slot.gain * slot.pan_right * 1.4142f;
                for (unsigned f = 0; f < frames; ++f) { knob_l[f] += kl[f] * l; knob_r[f] += kr[f] * r; }
                knob_fed = true;
            }
            bool knobbed = knob_heard && ((knob_tracks >> t) & 1);
            if (heard) {
                float* to_l = knobbed ? knob_l : master_l;
                float* to_r = knobbed ? knob_r : master_r;
                knob_fed |= knobbed;
                for (unsigned f = 0; f < frames; ++f) {
                    to_l[f] += slot.left[f]; to_r[f] += slot.right[f];
                    level = std::max(level, std::max(std::fabs(slot.left[f]), std::fabs(slot.right[f])));
                }
                if (slot.send > 0)
                    for (unsigned f = 0; f < frames; ++f) { bus_l[slot.bus][f] += slot.left[f] * slot.send; bus_r[slot.bus][f] += slot.right[f] * slot.send; }
                if (stem) { // a centred mono track sums to itself, as does one panned hard
                    float unpan = 1.0f / std::max(slot.pan_left + slot.pan_right, 1.0f);
                    for (unsigned f = 0; f < frames; ++f) stem[f] = (slot.left[f] + slot.right[f]) * unpan;
                }
            }
        }
        float shown = meters[t].load(std::memory_order_relaxed) * 0.97f; // a short fall
        meters[t].store(std::max(level, shown), std::memory_order_relaxed);
    }
    run_loops(frames, solo);
    for (unsigned b = 0; b < bus_count; ++b) {
        run_chain(bus_chain(b), bus_l[b], bus_r[b], frames);
        for (unsigned f = 0; f < frames; ++f) { master_l[f] += bus_l[b][f] * returns[b]; master_r[f] += bus_r[b][f] * returns[b]; }
    }
    // Knob FX into the master; after its targets let go it rings on for a few seconds (tails), then rests.
    if (knob_fed) knob_tail = uint32_t(6 * dsp::sample_rate());
    if (knob_tail && knob.kind() != EffectKind::None) {
        knob.process(knob_l, knob_r, frames);
        for (unsigned f = 0; f < frames; ++f) { master_l[f] += knob_l[f]; master_r[f] += knob_r[f]; }
        knob_tail = knob_tail > frames ? knob_tail - frames : 0;
    } else if (knob_fed) {
        for (unsigned f = 0; f < frames; ++f) { master_l[f] += knob_l[f]; master_r[f] += knob_r[f]; }
    }
    run_chain(master_chain, master_l, master_r, frames);
    if (punch_target == 255) punch.process(master_l, master_r, frames);
    // Flex Beat over the whole master: a bar, then Empty again (One Shot) or the same again (Loop).
    if (flex_pad && flex.on()) {
        flex_left -= frames;
        if (flex_left <= 0) {
            if (flex_mode) start_flex();
            else { flex.stop(); flex_pad = 0; flex_now.store(0, std::memory_order_relaxed); }
        }
    }
    if (flex.active()) flex.process(master_l, master_r, frames);
    run_live(255, master_l, master_r, frames);
    comp.process(master_l, master_r, frames); // the master compressor (Shift + Pad 5), before main volume
    if (loops.on()) { // the mix as a loop records it (as Resample hears it)
        std::copy(master_l, master_l + frames, source_l[looper::source_mix]);
        std::copy(master_r, master_r + frames, source_r[looper::source_mix]);
    }
    unsigned sounding = 0;
    for (unsigned s = 0; s < live_slots; ++s) sounding |= unsigned(live[s].punch.on() || live[s].pending.waiting) << s;
    live_on.store(sounding, std::memory_order_relaxed);
    punch_on.store(punch.on() || pad_punch.on() || sounding, std::memory_order_relaxed);
    // The input source (or the mix itself, before main volume): the take, the
    // Recall ring and the meter hear it; a render takes the mix.
    select_input(input, usb, frames);
    // Rec Input FX: Knob FX on the input while a take waits or runs (so Monitor hears it too).
    auto taking = take.state();
    if (knob_rec && knob_heard && (taking == TakeRecorder::State::Armed || taking == TakeRecorder::State::Recording || taking == TakeRecorder::State::Cued))
        knob_in.process(record_l, record_r, frames);
    take.process(record_l, record_r, frames);
    recall_ring.write(record_l, record_r, frames);
    keep_sources(frames);
    render_out.process(master_l, master_r, frames);
    // Monitor: the source heard with the mix (never the mix itself, heard already).
    if (monitor_on.load(std::memory_order_relaxed) && source_now.load(std::memory_order_relaxed) != unsigned(InputSource::Resample))
        for (unsigned f = 0; f < frames; ++f) { master_l[f] += record_l[f]; master_r[f] += record_r[f]; }
    float peak = 0;
    for (unsigned f = 0; f < frames; ++f) {
        if (click_env > 0.0005f) { // a short blip on top of the mix
            float blip = std::sin(2 * dsp::pi * click_phase) * click_env * click_level * 0.5f;
            click_phase += click_step; click_phase -= std::floor(click_phase);
            click_env *= 0.9985f;
            master_l[f] += blip; master_r[f] += blip;
        }
        float left = dsp::soft_clip(master_l[f] * volume), right = dsp::soft_clip(master_r[f] * volume);
        peak = std::max(peak, std::max(std::fabs(left), std::fabs(right)));
        out[2 * f] = int32_t(std::clamp(left, -0.999f, 0.999f) * 2147483647.0f);
        out[2 * f + 1] = int32_t(std::clamp(right, -0.999f, 0.999f) * 2147483647.0f);
    }
    output_peak.store(peak, std::memory_order_relaxed);
    if (!running) effect_clock.beats += frames / effect_clock.quarter; // tempo-synced effects run on while stopped
    frame_now += frames;
}

// The Looper's block: its clock from the sequencer's, its loops into L1-L4, each through its strip (a stereo
// picture its pan balances), its chain, mute and solo, into the master and its send's bus.
void EngineHost::run_loops(unsigned frames, bool solo) {
    Looper::Clock time;
    time.rate = dsp::sample_rate();
    time.bar = bar; time.beat = beat;
    time.running = loop_running;
    time.counting = loop_counting;
    time.per_frame = speed / tick_length();
    time.tick = loop_tick;
    time.restart = loop_restart && loop_running && !loop_counting;
    if (time.restart) loop_restart = false;
    time.input_lag = unsigned(clock.latency_frames());
    Looper::Sources sources;
    sources.frames = source_frames;
    for (unsigned s = 0; s < looper::source_count; ++s) sources.at[s] = {source_l[s], source_r[s]};
    float out_l[loop_tracks][looper::block], out_r[loop_tracks][looper::block];
    loops.run(time, sources, frames, out_l, out_r);
    if (!loops.on()) { for (auto& m : loop_meters) m.store(0, std::memory_order_relaxed); return; }
    for (unsigned n = 0; n < loop_tracks; ++n) {
        auto& strip = loop_strips[n];
        float level = 0;
        float l = strip.gain * strip.pan_left * 1.4142f, r = strip.gain * strip.pan_right * 1.4142f;
        for (unsigned f = 0; f < frames; ++f) { strip.left[f] = out_l[n][f] * l; strip.right[f] = out_r[n][f] * r; }
        run_chain(loop_chain(n), strip.left, strip.right, frames);
        bool heard = loops.on() && !strip.mute && !(solo && !strip.solo);
        if (heard) {
            for (unsigned f = 0; f < frames; ++f) {
                master_l[f] += strip.left[f]; master_r[f] += strip.right[f];
                level = std::max(level, std::max(std::fabs(strip.left[f]), std::fabs(strip.right[f])));
            }
            if (strip.send > 0)
                for (unsigned f = 0; f < frames; ++f) { bus_l[strip.bus][f] += strip.left[f] * strip.send; bus_r[strip.bus][f] += strip.right[f] * strip.send; }
        }
        float shown = loop_meters[n].load(std::memory_order_relaxed) * 0.97f;
        loop_meters[n].store(std::max(level, shown), std::memory_order_relaxed);
    }
}

// What each source played this block, for the loops to write in the next (the mix was kept after the compressor).
void EngineHost::keep_sources(unsigned frames) {
    source_frames = 0;
    if (!loops.on()) return;
    uint32_t used = loops.sources_used();
    auto keep = [&](unsigned s, const float* l, const float* r) {
        if (!((used >> s) & 1)) return;
        std::copy(l, l + frames, source_l[s]); std::copy(r, r + frames, source_r[s]);
    };
    keep(looper::source_input, record_l, record_r);
    for (unsigned t = 0; t < track_count; ++t) {
        if (slots[t].engine) keep(looper::source_track(t), slots[t].left, slots[t].right);
        else if ((used >> looper::source_track(t)) & 1) { std::fill(source_l[looper::source_track(t)], source_l[looper::source_track(t)] + frames, 0.0f); std::fill(source_r[looper::source_track(t)], source_r[looper::source_track(t)] + frames, 0.0f); }
    }
    for (unsigned n = 0; n < loop_tracks; ++n) keep(looper::source_loop(n), loop_strips[n].left, loop_strips[n].right);
    source_frames = frames;
}

} // namespace melodee
