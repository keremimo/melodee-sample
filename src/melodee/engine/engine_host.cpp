// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/engine_host.h"
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine_info.h"
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
    for (unsigned t = 0; t < track_count; ++t) slots[t].sampler.set_pad_effects(&(*pad_fx)[t]);
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
void EngineHost::load_state(const Project& project) {
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
    click = project.click == 1; click_level = project.click_level / 100.0f;
    motion = project.motion;
    song = project.song;
    global_tempo = project.tempo; swing = project.swing;
    seq_tempo = project.seq_tempo; pattern_tempo = project.pattern_tempo;
    bar = project.bar_ticks(); beat = project.beat_ticks();
    for (unsigned t = 0; t < track_count; ++t) load_track(t, project.tracks[t]);
    knob_targets();
    lead = project.tracks[0].pattern;
    tempo = 0;
    follow_tempo();
}

void EngineHost::follow_tempo() {
    unsigned own = pattern_tempo[lead % pattern_count];
    uint16_t now = uint16_t(seq_tempo && own ? own : global_tempo);
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
}

void EngineHost::select_engine(Slot& slot, EngineKind kind) {
    if (slot.engine) slot.engine->silence();
    slot.kind = kind;
    slot.engine = kind == EngineKind::Prophet ? static_cast<Engine*>(&slot.prophet)
                : kind == EngineKind::Sampler ? static_cast<Engine*>(&slot.sampler)
                : kind == EngineKind::Kit909 ? static_cast<Engine*>(&slot.kit)
                : kind == EngineKind::Fm6 ? static_cast<Engine*>(&slot.fm)
                : kind == EngineKind::Sid ? static_cast<Engine*>(&slot.sid)
                : kind == EngineKind::Cz ? static_cast<Engine*>(&slot.cz) : nullptr;
    slot.sampler.set_samples(&samples);
    slot.arp.top = kind == EngineKind::Sampler ? int(sampler_last_key) : 127; // a kit's notes run past 127
    for (auto& off : slot.offs) off.key = -1;
}

void EngineHost::apply(const Command& c) {
    using Type = Command::Type;
    auto& slot = slots[c.track % track_count];
    switch (c.type) {
    case Type::NoteOn: if (slot.engine && (c.index <= 127 || slot.kind == EngineKind::Sampler)) slot.engine->note_on(c.index, c.value); break;
    case Type::NoteOff: if (slot.engine && (c.index <= 127 || slot.kind == EngineKind::Sampler)) slot.engine->note_off(c.index); break;
    case Type::AllNotesOff:
        for (unsigned t = 0; t < track_count; ++t)
            if ((c.track == 255 || t == c.track) && slots[t].engine) {
                slots[t].engine->all_notes_off();
                for (auto& off : slots[t].offs) off.key = -1;
            }
        break;
    case Type::Engine: select_engine(slot, EngineKind(c.index)); break;
    case Type::Sound:
        if (slot.engine) {
            std::array<int16_t, max_params> values;
            const auto& info = engine_info(slot.kind);
            sound_values(info, c.index, values);
            for (unsigned p = 0; p < info.param_count; ++p) slot.engine->set_param(int(p), values[p]);
            slot.base = values;
        }
        break;
    case Type::Param:
        if (c.index < max_params) slot.base[c.index] = c.value;
        if (slot.engine) slot.engine->set_param(c.index, c.value);
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
            load_state(c.pack->project);
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
            pad_punch.start(p.kind, p.depth, bar_length(), grid, p.tail);
            break;
        }
        live_punch = true; punch_from_song = false;
        if (running) pending = p;
        else start_punch(p);
        break;
    }
    case Type::PunchOff:
        if (c.index >> 8) { if (int(c.index >> 8) - 1 == pad_punch_pad && c.track == pad_punch_track) pad_punch.stop(); break; }
        if (pending.waiting && (pending.target == c.track || c.track == 254)) pending.waiting = false; // let go before it began
        else if (punch_target == c.track || c.track == 254) punch.stop();
        live_punch = false;
        break;
    case Type::PunchDepth:
        if (c.index >> 8) pad_punch.set_depth(std::clamp<int>(c.value, 0, 100) / 100.0f);
        else { punch.set_depth(std::clamp<int>(c.value, 0, 100) / 100.0f); pending.depth = std::clamp<int>(c.value, 0, 100) / 100.0f; }
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
        take.arm(float(c.value), length, c.index, cue_track >= 0);
        break;
    }
    case Type::RecordSource:
        if (take.state() == TakeRecorder::State::Idle && c.track % unsigned(InputSource::Count) != source_now.load(std::memory_order_relaxed)) {
            source_now.store(c.track % unsigned(InputSource::Count), std::memory_order_relaxed);
            recall_ring.restart(); // what came in before is another source's
        }
        break;
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
        break;
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
    case Type::Wheel: if (slot.engine) slot.engine->set_wheel(c.value / 1000.0f); break;
    case Type::Level: slot.gain = level_gain(c.value); break;
    case Type::Pan: {
        float angle = (std::clamp<int>(c.value, -50, 50) + 50) / 100.0f * dsp::pi / 2;
        slot.pan_left = std::cos(angle); slot.pan_right = std::sin(angle);
        break;
    }
    case Type::Send: slot.send = c.value / 100.0f; break;
    case Type::Mute: slot.mute = c.value != 0; break;
    case Type::Solo: slot.solo = c.value != 0; break;
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
        running = true; until_tick = 0;
        tick_now = c.value == 1 ? 0 : c.position;
        arp_ticks = 0; until_arp = 0; // the repeats line up with the bar, so their swing falls with the sequencer's
        song_mode = c.value == 1; song_bar_now = c.index;
        count_left = c.track == 1 ? bar : 0;
        count_beat.store(count_left ? 1 : 0, std::memory_order_relaxed);
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
        tick_now = c.position; until_tick = 0; // the new place plays at once
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
        if (off.key < 0) { off.key = int16_t(key); off.samples = samples; return; }
    slot.engine->note_off(key); // no room: end it now rather than hang
}

void EngineHost::run_offs(Slot& slot, unsigned frames) {
    for (auto& off : slot.offs) {
        if (off.key < 0) continue;
        off.samples -= int32_t(frames);
        if (off.samples <= 0) { if (slot.engine) slot.engine->note_off(off.key); off.key = -1; }
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
        for (auto& s : slots) for (auto& off : s.offs) if (off.key >= 0 && s.engine) { s.engine->note_off(off.key); off.key = -1; }
        return;
    }
    bar_now.store(song_bar_now, std::memory_order_relaxed);
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        int index = song.clip_at(t, song_bar_now);
        slot.silent = index < 0 || song.clips[size_t(index)].muted;
        if (slot.silent) { slot.clip = -1; positions_now[t].store(-1, std::memory_order_relaxed); continue; }
        const auto& clip = song.clips[size_t(index)];
        if (index != slot.clip || song_bar_now == clip.bar) {
            // A clip starts (or the loop came back into it): its pattern from where the clip says.
            slot.clip = index;
            slot.pattern = clip.pattern;
            slot.start = tick_now - (song_bar_now - clip.bar) * bar;
            patterns_now[t].store(slot.pattern, std::memory_order_relaxed);
        }
    }
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
}

void EngineHost::start_punch(const PendingPunch& p) {
    if (p.target != punch_target) { punch.clear(); punch_target = p.target; } // a new signal: the old history is not its own
    punch.start(p.kind, p.depth, bar_length(), bar_length() / (4u << p.snap), p.tail);
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
            click_step = (done == 0 ? 1568.0f : 1046.0f) / 44100.0f;
            count_beat.store(done / beat + 1, std::memory_order_relaxed);
        }
        --count_left;
        if (!count_left) count_beat.store(0, std::memory_order_relaxed);
        until_tick += tick_length();
        return;
    }
    // A punch waiting for its grid.
    if (pending.waiting && tick_now % (4 * ticks_per_quarter / (4u << pending.snap)) == 0) { pending.waiting = false; start_punch(pending); }
    // The metronome: every beat while playing, higher on the bar.
    if (click && tick_now % beat == 0) {
        click_env = 1; click_phase = 0;
        click_step = (tick_now % bar == 0 ? 1568.0f : 1046.0f) / 44100.0f;
    }
    // A pattern render ends after its ticks, on the frame.
    if (!bounce_song && render_out.running() && tick_now >= bounce_ticks) render_out.finish(block_at);
    if (tick_now % bar == 0) bar_start();
    if (!running) return;
    if (song_mode && tick_now % sixteenth_ticks == 0) play_song_punches();
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        if (!slot.engine || slot.silent) continue;
        const auto& pattern = *patterns[t][slot.pattern];
        uint32_t length = std::max<uint32_t>(pattern.length, 1);
        uint32_t at = (tick_now - slot.start) % length;
        positions_now[t].store(int(at), std::memory_order_relaxed);
        if (at == 0 && int(t) == cue_track) { take.go(block_at); cue_track = -1; } // Rec length Seq: the take starts with the pattern
        if (motion.count && at % sixteenth_ticks == 0) play_motion(t, slot, at / sixteenth_ticks);
        const auto& events = pattern.events;
        for (size_t n = pattern.lower(at); n < events.size() && events[n].tick == at; ++n) {
            const auto& e = events[n];
            if (e.key > 127 && slot.kind != EngineKind::Sampler) continue; // past 127: kit pads only
            if (skipped(slot, e.key, at)) continue;
            random = random * 1664525u + 1013904223u;
            if (e.chance < 100 && (random >> 8) % 100 >= e.chance) continue;
            // Held for its length, a touch short unless tied.
            double gap = e.flags & step_tie ? 0 : std::min<unsigned>(pattern.q(), e.length) * 0.1;
            auto samples = int32_t(tick_length() * (e.length - gap));
            int velocity = e.flags & step_accent ? std::min(127, e.velocity + 24) : e.velocity;
            slot.engine->note_on(e.key, velocity);
            schedule_off(slot, e.key, std::max<int32_t>(samples, 64));
        }
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
    Command command;
    for (unsigned n = 0; n < 4096 && queue.pop(command); ++n) apply(command);
    frames = std::min(frames, block);
    // The clock tempo-synced effects follow: quarters since Play (on from where it was while stopped).
    effect_clock.quarter = tick_length() * ticks_per_quarter;
    if (running) effect_clock.beats = (double(tick_now) - std::max(until_tick, 0.0) / tick_length()) / ticks_per_quarter;
    // The arps' clock, at the block's start (within 3 ms of the beat).
    until_arp -= frames;
    while (until_arp <= 0) { arp_tick(); until_arp += arp_tick_length(); }
    bool solo = false;
    for (auto& slot : slots) {
        solo |= slot.solo;
        std::fill(slot.buffer, slot.buffer + frames, 0.0f);
        std::fill(slot.left, slot.left + frames, 0.0f); std::fill(slot.right, slot.right + frames, 0.0f);
    }

    // Render up to each step, play the step, carry on: steps land on the sample.
    unsigned done = 0;
    while (done < frames) {
        unsigned n = frames - done;
        if (running) {
            if (until_tick <= 0) { block_at = done; tick(); continue; }
            n = std::min<unsigned>(n, unsigned(std::ceil(until_tick)));
        }
        for (auto& slot : slots) {
            if (!slot.engine) continue;
            run_offs(slot, n);
            run_late(slot, n);
            if (slot.engine->stereo()) slot.engine->render_stereo(slot.left + done, slot.right + done, n);
            else slot.engine->render(slot.buffer + done, n);
        }
        if (running) until_tick -= n;
        done += n;
    }
    is_playing.store(running, std::memory_order_relaxed);

    // The signal: each track panned, through its chain, into the master and
    // its send into one bus; each bus through its chain at its return level
    // into the master; the master through its chain, then main volume.
    for (unsigned b = 0; b < bus_count; ++b) { std::fill(bus_l[b], bus_l[b] + frames, 0.0f); std::fill(bus_r[b], bus_r[b] + frames, 0.0f); }
    std::fill(master_l, master_l + frames, 0.0f); std::fill(master_r, master_r + frames, 0.0f);
    std::fill(knob_l, knob_l + frames, 0.0f); std::fill(knob_r, knob_r + frames, 0.0f);
    bool knob_fed = false;
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        float level = 0;
        float* stem = stems ? stems + size_t(t) * block : nullptr;
        if (stem) std::fill(stem, stem + frames, 0.0f);
        if (slot.engine) {
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
            bool heard = !slot.mute && !(solo && !slot.solo);
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
    for (unsigned b = 0; b < bus_count; ++b) {
        run_chain(bus_chain(b), bus_l[b], bus_r[b], frames);
        for (unsigned f = 0; f < frames; ++f) { master_l[f] += bus_l[b][f] * returns[b]; master_r[f] += bus_r[b][f] * returns[b]; }
    }
    // Knob FX into the master; after its targets let go it rings on for a few seconds (tails), then rests.
    if (knob_fed) knob_tail = uint32_t(6 * dsp::sample_rate);
    if (knob_tail && knob.kind() != EffectKind::None) {
        knob.process(knob_l, knob_r, frames);
        for (unsigned f = 0; f < frames; ++f) { master_l[f] += knob_l[f]; master_r[f] += knob_r[f]; }
        knob_tail = knob_tail > frames ? knob_tail - frames : 0;
    } else if (knob_fed) {
        for (unsigned f = 0; f < frames; ++f) { master_l[f] += knob_l[f]; master_r[f] += knob_r[f]; }
    }
    run_chain(master_chain, master_l, master_r, frames);
    if (punch_target == 255) punch.process(master_l, master_r, frames);
    comp.process(master_l, master_r, frames); // the master compressor (Shift + Pad 5), before main volume
    punch_on.store(punch.on() || pad_punch.on() || pending.waiting, std::memory_order_relaxed);
    // The input source (or the mix itself, before main volume): the take, the
    // Recall ring and the meter hear it; a render takes the mix.
    select_input(input, usb, frames);
    // Rec Input FX: Knob FX on the input while a take waits or runs (so Monitor hears it too).
    auto taking = take.state();
    if (knob_rec && knob_heard && (taking == TakeRecorder::State::Armed || taking == TakeRecorder::State::Recording || taking == TakeRecorder::State::Cued))
        knob_in.process(record_l, record_r, frames);
    take.process(record_l, record_r, frames);
    recall_ring.write(record_l, record_r, frames);
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
}

} // namespace melodee
