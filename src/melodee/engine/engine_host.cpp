// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/engine_host.h"
#include <chrono>
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/lane_value.h"
#include <algorithm>
#include <cmath>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

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

PlayPatterns copy_patterns(const std::array<Pattern, pattern_count>& from, unsigned speed) {
    PlayPatterns out {};
    for (unsigned p = 0; p < pattern_count; ++p) out[p] = new Pattern(played_pattern(from[p], speed)); // as it plays (roadmap P1: its feel, its track's speed)
    return out;
}
void free_patterns(PlayPatterns& patterns) { for (auto& p : patterns) { delete p; p = nullptr; } }

ProjectPack::ProjectPack(const Project& project, const PatternBank& bank) : project(project) {
    for (unsigned t = 0; t < track_count; ++t) patterns[t] = copy_patterns(bank.at[t], project.tracks[t].speed);
}
ProjectPack::~ProjectPack() { for (auto& t : patterns) free_patterns(t); }
TrackPack::TrackPack(unsigned track, const Track& value, const std::array<Pattern, pattern_count>& from)
    : track(uint8_t(track)), value(value), patterns(copy_patterns(from, value.speed)) {}
TrackPack::~TrackPack() { free_patterns(patterns); }

EngineHost::EngineHost() : EngineHost(false, DelayPool::blocks) {}
EngineHost::EngineHost(Lean lean) : EngineHost(true, lean.delay_blocks) {}

EngineHost::EngineHost(bool lean, unsigned delay_blocks)
    : is_lean(lean), fx_clock(lean ? &own_clock : &effect_clock), take(!lean), recall_ring(!lean), render_out(!lean),
      punch(!lean), live {Live(!lean), Live(!lean), Live(!lean), Live(!lean)}, flex(!lean),
      kept {Kept(!lean), Kept(!lean), Kept(!lean), Kept(!lean)}, pool(delay_blocks), lines(!lean) {
    static_assert(live_slots == 4, "one Live and one Kept a slot above");
    for (auto& t : patterns) for (auto& p : t) p = new Pattern;
    for (auto& s : positions_now) s.store(-1);
    pad_fx = std::make_unique<std::array<SamplerEngine::PadEffects, track_count>>();
    print_frames = std::make_unique<std::array<float, 2 * block * track_count>>();
    for (unsigned t = 0; t < track_count; ++t) {
        slots[t].sampler.set_pad_effects(&(*pad_fx)[t]); slots[t].sampler.set_pager(&pager);
        slots[t].print.set_pager(&pager); // freeze plan F2b
        slots[t].plugin.set_pool(&pool);   // its performance knobs' echo and reverb (engine/macros.h)
        slots[t].print_l = print_frames->data() + 2 * block * t;
        slots[t].print_r = slots[t].print_l + block;
    }
    // Build every engine's static tables now, not on the audio thread.
    for (unsigned k = 0; k < unsigned(EngineKind::Count); ++k) (void)engine_info(EngineKind(k));
    knob_in.set_bypass(true); // Rec Input FX rests until a take waits or runs (render())
    { // freeze plan F2b: the centre's pan law worked out as set_pan() works it out at run time
        volatile int held = 0;
        int middle = held;
        float angle = (std::clamp<int>(middle, -50, 50) + 50) / 100.0f * dsp::pi / 2;
        pan_centre = std::cos(angle);
    }
}

EngineHost::~EngineHost() {
    pager.stop(); // freeze plan F2b: no pins taken while the prints' mappings go
    for (auto& slot : slots) {
        slot.print.cut_all();
        delete slot.freeze; delete slot.freeze_leaving;
    }
    for (auto* pack : freeze_next) delete pack;
    for (auto* pack : freeze_limbo) delete pack;
    FreezePack* done = nullptr;
    while (done_freezes.pop(done)) delete done;
    for (auto& t : patterns) free_patterns(t);
    Pattern* p = nullptr;
    while (retired_patterns.pop(p)) delete p;
    for (auto& q : limbo) delete q;
}

// Before the audio thread runs: copies into the patterns it already holds.
void EngineHost::load(const Project& project, const PatternBank& bank) {
    for (unsigned t = 0; t < track_count; ++t)
        for (unsigned p = 0; p < pattern_count; ++p) *patterns[t][p] = played_pattern(bank.at[t][p], project.tracks[t].speed);
    load_state(project);
    snap_mix = true; // its levels from the first block, not eased in from the last project's
}

// A whole project switched to at once (a load, a new project, a new rate): the audio thread's Reload, or the
// loader's while the audio thread is parked. The pack takes the old patterns back, to be freed with it.
void EngineHost::reload(ProjectPack& pack, bool keep_loops) {
    apply({Command::Type::Stop});
    for (unsigned t = 0; t < track_count; ++t) { // freeze plan F2b: another project's prints; the shell freezes again
        auto& slot = slots[t];
        slot.print.cut_all();
        retire_freeze(slot.freeze); retire_freeze(slot.freeze_leaving); retire_freeze(freeze_next[t]);
        slot.freeze = slot.freeze_leaving = freeze_next[t] = nullptr;
        slot.print_on = false; slot.hand.reset(); wake(slot);
    }
    for (auto& s : slots) if (s.engine) s.engine->silence();
    pending.waiting = false; punch.clear(); punch_target = 255;
    for (auto& l : live) { l.pending.waiting = false; l.punch.stop(); l.punch.clear(); }
    for (auto& k : kept) { k.punch.clear(); k.target = 255; } // pad-fx C
    flex.clear(); flex_pad = 0; flex_waiting = false; flex_now.store(0, std::memory_order_relaxed);
    load_state(pack.project, keep_loops);
    snap_mix = true; // its levels from the first block, not eased in from the last project's
    reformatting = false;
    std::swap(patterns, pack.patterns);
    if (!is_parked) return;
    // Parked, there is time the audio thread never has: the delay memory the load took is cleared whole now (each
    // effect plays wet from its first block, not dry while a slice a block is cleared), and so is every free block
    // the last project wrote, ready for the next effect to take.
    for (auto& chain : fx) for (auto& effect : chain) effect.finish_clearing();
    knob.finish_clearing(); knob_in.finish_clearing();
    if (pad_fx) for (auto& pads : *pad_fx) for (auto& pad : pads) for (auto& effect : pad) effect.finish_clearing();
    pool.scrub();
}

uint32_t EngineHost::park() {
    uint32_t serial = ++park_next;
    if (!serial) serial = ++park_next; // 0 is none
    Command c {Command::Type::Park};
    c.position = serial;
    return send(c) ? serial : 0;
}

// Parked: silence out, nothing of the host's state touched (the loader owns it) but the clock MIDI out and timed
// notes are placed by, which runs on with the frames played meanwhile.
void EngineHost::play_parked(int32_t* out, unsigned frames, float* stems) {
    clock.stamp(int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()),
                frame_now + parked_frames);
    frames = std::min(frames, block);
    std::fill(out, out + 2 * frames, 0);
    if (stems) std::fill(stems, stems + size_t(track_count) * block, 0.0f);
    for (auto* level : {&out_levels[0], &out_levels[1], &in_levels[0], &in_levels[1]}) level->store(0, std::memory_order_relaxed);
    parked_frames += frames;
}

// Everything of a project but its patterns (Reload swaps those in by pointer).
void EngineHost::load_state(const Project& project, bool keep_loops) {
    for (unsigned n = track_count; n < chain_count; ++n) { // the buses and the master; each track's in load_track()
        const auto& chain = project.chain(n);
        for (unsigned k = 0; k < max_slots; ++k) {
            const auto& s = chain.slots[k];
            fx[n][k].set_kind(EffectKind(s.kind), pool, false); // worked out once, with its values
            fx[n][k].set_params(s.params);
            fx[n][k].set_bypass(s.bypass);
            fx_key[n][k] = s.key < fx_keys ? s.key : 0;
        }
    }
    for (unsigned b = 0; b < bus_count; ++b) { returns[b] = project.buses[b].level / 100.0f; returns_base[b] = project.buses[b].level; }
    for (unsigned k = 0; k < max_slots; ++k) master_base[k] = project.master.slots[k].params; // roadmap P4: Motion borrows them
    // Knob FX and the master compressor.
    const auto& k = project.knob_fx;
    knob.set_kind(EffectKind(k.kind), pool, false);
    knob_in.set_kind(EffectKind(k.kind), pool, false);
    knob.set_params(k.params); knob_in.set_params(k.params);
    knob_heard = k.active();
    knob_tracks = k.tracks;
    knob_pads = k.pads;
    comp_settings = project.compressor;
    comp.set(comp_settings);
    // Pad FX's K1-K3 and Flex Beat's settings.
    mpc_params = project.pad_fx_params;
    flex_mode = project.flex.mode; flex_mix = project.flex.mix; flex_quantize = project.flex.quantize;
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
    for (unsigned n = 0; n < modulator_count; ++n) { // roadmap P4: each lane where the project has it
        Modulator m = project.modulators[n];
        for (auto& g : m.targets) if (g.used()) g.own = int16_t(lane_own(project, g.track, g.param, g.pad));
        set_modulator(n, m, false);
    }
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
    if (is_lean) { reformatting = true; return; } // the rate is the live host's to set: silent, its print dropped
    dsp::device_rate.store(rate, std::memory_order_relaxed);
    take.set_rate(rate); recall_ring.set_rate(rate); render_out.set_rate(rate);
    reformatting = true;
    format_serial.fetch_add(1, std::memory_order_release);
}

void EngineHost::follow_tempo() {
    unsigned own = pattern_tempo[lead % pattern_count];
    // MIDI clock in leads; then the song's tempo lane while the song plays (roadmap P5); then BPM per pattern.
    uint16_t now = uint16_t(ext_on && ext_tempo ? ext_tempo : song_mode && lane_tempo ? lane_tempo : seq_tempo && own ? own : global_tempo);
    if (now == tempo) return;
    tempo = now;
    for (auto& s : slots) s.sampler.set_tempo(tempo);
    tempo_shown.store(tempo, std::memory_order_relaxed);
}

// Roadmap P5: while the song plays, the tempo its lane has where it is now (a ramp worked out a tick at a time).
void EngineHost::follow_lane() {
    if (song_mode && !song.tempo.empty()) {
        double at = double(song_bar_now) + double(tick_now % std::max(bar, 1u)) / std::max(bar, 1u);
        lane_tempo = uint16_t(std::clamp<long>(std::lround(song.tempo.at(at, global_tempo)), 300, 3000));
    } else {
        lane_tempo = 0;
    }
    follow_tempo();
}

void EngineHost::retire(Pattern* pattern) {
    if (!pattern || retired_patterns.push(pattern)) return;
    for (auto& q : limbo) if (!q) { q = pattern; return; } // tried again each block
    // No room anywhere: it is leaked rather than freed here.
}

// The commands due `offset` frames into this block of `frames`, in the order sent. A timed note (send_at) waits
// for its frame, and whatever came after it waits behind it, so nothing overtakes it. Inside a block only notes
// and values are applied (mid_block); anything else waits for the next block's start, where all of it always was.
// Returns the offset the next command waits for, or `frames`.
unsigned EngineHost::take_commands(unsigned offset, unsigned frames) {
    for (unsigned n = 0; n < 4096; ++n) {
        if (park_wanted) return frames; // a Park taken: what came after it waits for the loader's project
        if (!has_waiting && !queue.pop(waiting)) return frames;
        has_waiting = true;
        if (waiting.timed) {
            int32_t due = int32_t(waiting.at - uint32_t(frame_now));
            if (due > timed_reach) due = 0; // the audio clock off (just after an overrun): at once, as before
            if (due > int32_t(offset)) return unsigned(std::min<int32_t>(due, int32_t(frames)));
            // On its frame, or it came after it (the MIDI thread woke late): at once, and counted.
            uint32_t late = uint32_t(int32_t(offset) - due);
            if (!late) timed_on_time.fetch_add(1, std::memory_order_relaxed);
            else {
                timed_late.fetch_add(1, std::memory_order_relaxed);
                if (late > timed_worst.load(std::memory_order_relaxed)) timed_worst.store(late, std::memory_order_relaxed);
            }
        } else if (offset && !mid_block(waiting.type)) {
            return frames;
        }
        has_waiting = false;
        block_at = offset; // MIDI out of what it plays goes at this frame
        apply(waiting);
    }
    return frames;
}

bool EngineHost::mid_block(Command::Type type) {
    using Type = Command::Type;
    switch (type) {
    case Type::NoteOn: case Type::NoteOff: case Type::PadNote: case Type::ArpKey: case Type::Pressure: case Type::Audition:
    case Type::Param: case Type::Wheel: case Type::Bend: case Type::Mod: case Type::Level: case Type::Pan: case Type::FxParam:
    case Type::PadFxParam: case Type::PatternData: case Type::Recorded:
        return true;
    default:
        return false;
    }
}

// Where `track` was when `frame` was rendered: the tick at or before it, or the next when that is nearer, counted
// back from position_of() through the ticks of this run (a jump, Play or Locate, starts a new run).
bool EngineHost::place_at(unsigned track, uint64_t frame, int& position, uint32_t& ticks) const {
    auto entry = [&](uint32_t n) { return tick_ring[n % tick_ring_size].load(std::memory_order_relaxed); };
    const uint32_t f = uint32_t(frame);
    for (unsigned attempt = 0;; ++attempt) {
        uint32_t head = tick_head.load(std::memory_order_acquire);
        int now = position_of(track);
        if (!head || now < 0 || !playing()) return false;
        uint32_t i = head - 1, reach = std::min(head, tick_ring_size - 16); // clear of what the writer may overwrite
        uint64_t e = entry(i);
        int back = 0;
        while (int32_t(uint32_t(e) - f) > 0 && uint32_t(back) + 1 < reach) {
            uint64_t before = entry(i - 1);
            if (uint32_t(before >> 32) + 1 != uint32_t(e >> 32)) break; // the run starts here
            --i; e = before; ++back;
        }
        bool next = false; // past that tick: the next one, when it is nearer
        if (int32_t(f - uint32_t(e)) > 0) {
            float span = i == head - 1 ? tick_frames.load(std::memory_order_relaxed) : float(uint32_t(entry(i + 1)) - uint32_t(e));
            next = float(f - uint32_t(e)) * 2 > span;
        }
        if (tick_head.load(std::memory_order_acquire) != head && attempt < 3) continue; // a tick came meanwhile
        Speed sp = speed_of(speeds_now[track % track_count].load(std::memory_order_relaxed)); // a track at a speed: ticks as written
        position = now - int(std::lround(double(back - (next ? 1 : 0)) * sp.num / sp.den));
        ticks = uint32_t(e >> 32) + (next ? 2 : 1); // as ticks() reads just after that tick played
        return true;
    }
}

bool EngineHost::heard_place(unsigned track, uint64_t ns, int& position, uint32_t& ticks) const {
    if (!ns || !clock.known()) return false;
    uint64_t heard = clock.heard_frame(int64_t(ns)), late = compensation(); // roadmap P4: heard that much after it rendered
    return place_at(track, heard > late ? heard - late : 0, position, ticks);
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
    slot.plugin_entry = track.plugin;
    speeds_now[t % track_count].store(uint8_t(track.speed < speed_count ? track.speed : speed_normal), std::memory_order_relaxed);
    select_engine(slot, track.engine);
    const auto& info = engine_info(track.engine);
    slot.base = track.params;
    if (slot.engine) for (unsigned r = 0; r < mod_routes; ++r) slot.engine->set_mod(r, track.mods[r]);
    for (unsigned pad = 0; pad < sampler_pads; ++pad) slot.sampler.set_pad(pad, track.pads[pad]);
    slot.sampler.set_trims(track.zone_trims);
    slot.sampler.set_pad_mutes(track.pad_mutes);
    if (slot.engine) for (unsigned p = 0; p < info.param_count; ++p) slot.engine->set_param(int(p), track.params[p]);
    apply({Command::Type::Level, uint8_t(t), 0, 0, track.mix.level});
    apply({Command::Type::Pan, uint8_t(t), 0, 0, track.mix.pan});
    for (unsigned b = 0; b < bus_count; ++b) { slot.sends[b] = track.mix.sends[b] / 100.0f; slot.mix_sends[b] = track.mix.sends[b]; }
    slot.mute = track.mix.mute; slot.solo = track.mix.solo;
    slot.pattern = track.pattern;
    patterns_now[t].store(slot.pattern, std::memory_order_relaxed);
    for (unsigned k = 0; k < max_slots; ++k) {
        const auto& s = track.fx.slots[k];
        fx[t][k].set_kind(EffectKind(s.kind), pool, false); // worked out once, with its values
        fx[t][k].set_params(s.params);
        fx[t][k].set_bypass(s.bypass);
        fx_key[t][k] = s.key < fx_keys ? s.key : 0;
        slot.fx_base[k] = s.params;
    }
    slot.fx_moved = false;
    auto& kinds = pad_fx_kinds[t % track_count];
    for (unsigned pad = 0; pad < sampler_pads; ++pad) {
        const auto& chain = track.pad_fx.at[pad];
        for (unsigned k = 0; k < pad_slots; ++k) {
            const auto& s = chain.slots[k];
            unsigned bit = pad * pad_slots + k;
            if (!s.kind && !kinds.test(bit)) continue; // most pads have none, before and after: their effect not read
            auto& effect = (*pad_fx)[t][pad][k];
            effect.set_kind(EffectKind(s.kind), pool, false);
            effect.set_params(s.params);
            effect.set_bypass(s.bypass);
            kinds.set(bit, effect.kind() != EffectKind::None);
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
    slot.engine = kind == EngineKind::Sampler ? static_cast<Engine*>(&slot.sampler)
                : kind == EngineKind::Midi ? static_cast<Engine*>(&slot.midi)
                : kind == EngineKind::Plugin ? static_cast<Engine*>(&slot.plugin) : nullptr;
    slot.plugin.attach(kind == EngineKind::Plugin ? runner_of(slot.plugin_entry) : nullptr);
    if (kind != EngineKind::Plugin) slot.plugin.release_macros();
    slot.sampler.set_samples(&samples);
    slot.arp.top = kind == EngineKind::Sampler ? int(sampler_last_key) : 127; // a kit's notes run past 127
    for (auto& off : slot.offs) off.key = -1;
    slot.held = {};
    slot.midi.audio = midi_track::AudioNone; // the shell says again what it plays (roadmap P4, M5)
    slot.program_sent = -1;
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
    case Type::TrackPlugin:
        if (c.track >= track_count) break;
        slot.plugin_entry = uint8_t(c.value);
        slot.plugin.attach(slot.kind == EngineKind::Plugin ? runner_of(slot.plugin_entry) : nullptr);
        break;
    case Type::NoteOn:
        if (slot.freeze && slot.freeze->deep) break; // freeze plan F2d: a deep-frozen track's pads play nothing
        if (slot.freeze) { wake(slot); slot.hand.set(c.index & 0xff); } // freeze plan F2b: a hand wakes a frozen track's engine
        if (c.index <= 127 || slot.kind == EngineKind::Sampler) engine_note_on(slot, c.index, c.value);
        if (c.pattern != 1) midi_note(c.track % track_count, c.index & 0xff, c.value, frame_now + block_at, -1); // a pad's note (not one from MIDI in)
        break;
    case Type::NoteOff:
        slot.hand.reset(c.index & 0xff);
        if (c.index <= 127 || slot.kind == EngineKind::Sampler) engine_note_off(slot, c.index);
        if (c.pattern != 1) midi_note_off(c.track % track_count, c.index & 0xff, frame_now + block_at);
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
        engine_param(slot, c.index, c.value);
        if (mods_on) mod_note(c.track % track_count, c.index, 0, c.value, true); // roadmap P4
        if (slot.kind == EngineKind::Midi) midi_param(c.track % track_count, c.index, c.value, frame_now);
        break;
    case Type::Mod:
        if (c.index < mod_routes) {
            EngineCall call;
            call.op = EngineCall::Op::Mod; call.a = c.index; call.b = c.route.source; call.c = c.route.destination; call.d = c.route.amount;
            if (!keep_call(slot, call)) make_call(c.track % track_count, slot, call);
        }
        break;
    case Type::MidiAudio: slot.midi.audio = std::clamp<int>(c.value, 0, midi_track::AudioCount - 1); break; // roadmap P4 (M5)
    case Type::MidiEcho: // roadmap P4 (M3): what came in, on the MIDI track's own port and channel
        if (slot.kind == EngineKind::Midi) {
            MidiOutEvent e {MidiOutEvent::Kind::Control, uint8_t(std::clamp<int>(slot.base[midi_track::Channel], 1, 16)), uint8_t(c.position & 0x7f),
                            uint8_t(c.value & 0x7f), 0, frame_now + block_at};
            if (c.index == 0xa0) e.kind = MidiOutEvent::Kind::PolyPressure;
            else if (c.index == 0xd0) { e.kind = MidiOutEvent::Kind::ChannelPressure; e.b = e.a; }
            else if (c.index != 0xb0) break;
            e.dest = midi_track::dest_of(slot.base[midi_track::Port]); e.track = c.track % track_count;
            emit(e);
        }
        break;
    case Type::Modulator: // roadmap P4
        if (c.track < modulator_count) { Modulator m = mod_runs[c.track].m; unpack_modulator(c, m); set_modulator(c.track, m, true); }
        break;
    case Type::FxKey: // roadmap P4
        if (c.track < chain_count && c.index < max_slots) fx_key[c.track][c.index] = uint8_t(c.value >= 0 && c.value < int(fx_keys) ? c.value : 0);
        break;
    case Type::FxKind:
        if (c.track < chain_count && c.index < max_slots) {
            fx_key[c.track][c.index] = 0; // a new kind hears itself until told otherwise
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
            if (c.track == master_chain && c.pattern < effect_params) master_base[c.index][c.pattern] = c.value; // roadmap P4
            if (mods_on && c.track < track_count) mod_note(c.track, motion_fx_param(c.index, c.pattern), 0, c.value, true);
            if (mods_on && c.track == master_chain) mod_note_any(motion_master_fx(c.index, c.pattern), c.value, true);
        }
        break;
    case Type::FxBypass: if (c.track < chain_count && c.index < max_slots) fx[c.track][c.index].set_bypass(c.value != 0); break;
    case Type::PadFxKind: case Type::PadFxParam: case Type::PadFxBypass: {
        unsigned slot = c.index & 0xff, param = c.index >> 8;
        if (c.track >= track_count || c.pattern >= sampler_pads || slot >= pad_slots) break;
        auto& effect = (*pad_fx)[c.track][c.pattern][slot];
        if (c.type == Type::PadFxKind) {
            effect.set_kind(EffectKind(c.value), pool);
            pad_fx_kinds[c.track].set(c.pattern * pad_slots + slot, effect.kind() != EffectKind::None);
        }
        else if (c.type == Type::PadFxParam) {
            EngineCall call; // inside the sampler's render: at its frame
            call.op = EngineCall::Op::PadFxParam; call.a = c.pattern; call.b = int32_t(slot); call.c = int32_t(param); call.d = c.value;
            if (!keep_call(slots[c.track], call)) effect.set_param(param, c.value);
        }
        else effect.set_bypass(c.value != 0);
        break;
    }
    case Type::SendBus: break; // a track sends into every bus (Send's index), so none is picked
    case Type::Return:
        if (c.index < bus_count) { returns_base[c.index] = int16_t(std::clamp<int>(c.value, 0, 100)); returns[c.index] = returns_base[c.index] / 100.0f; }
        if (mods_on && c.index < bus_count) mod_note_any(motion_return(c.index), returns_base[c.index], true);
        break;
    case Type::Pad:
        slot.sampler.set_pad(c.index, c.pad);
        if (mods_on) for (unsigned p = 0; p < pad_param_count; ++p) mod_note(c.track % track_count, motion_pad_param(PadParam(p)), c.index, pad_param_value(c.pad, PadParam(p)), true);
        break;
    case Type::ZoneTrim: // round 3 "instrument C"
        slot.sampler.set_trim(c.index, c.position, c.micros, int(c.pattern) - 128, c.value);
        break;
    case Type::PadMute:
        if (c.index == 0xffff) for (unsigned pad = 0; pad < sampler_pads; ++pad) slot.sampler.set_pad_mute(pad, c.value != 0);
        else slot.sampler.set_pad_mute(c.index, c.value != 0);
        break;
    case Type::TrackLoad:
        if (c.track_pack) {
            const auto& pack = *c.track_pack;
            auto& target = slots[pack.track % track_count];
            if (target.engine) target.engine->all_notes_off();
            { // freeze plan F2b: a new track plays live (the shell freezes it again)
                unsigned t = pack.track % track_count;
                target.print.cut_all();
                retire_freeze(target.freeze); retire_freeze(target.freeze_leaving); retire_freeze(freeze_next[t]);
                target.freeze = target.freeze_leaving = freeze_next[t] = nullptr;
                target.print_on = false; target.hand.reset(); wake(target);
            }
            load_track(pack.track, pack.value);
            std::swap(patterns[pack.track % track_count], c.track_pack->patterns); // the pack takes the old ones back to be freed
            if (!done_tracks.push(c.track_pack)) {} // the MIDI thread frees it; a full queue only leaks one
        }
        break;
    case Type::Reload:
        if (c.pack) {
            reload(*c.pack, c.value == 1);
            if (!done_packs.push(c.pack)) {} // the MIDI thread frees it; a full queue only leaks one
        }
        break;
    case Type::Park: // a project load on the loader's thread: fade out, then silent and still until resume()
        park_wanted = c.position;
        if (park_over()) park_wanted = 0; // called off already (the loader stopped waiting)
        break;
    case Type::SongPunchSet: if (c.index < song_punches) song.punches[c.index] = c.song_punch; break;
    case Type::SongPunchCount: song.punch_count = uint16_t(std::clamp<int>(c.value, 0, song_punches)); break;
    case Type::SongRecord: song_recording = c.value != 0; break;
    case Type::PunchOn: {
        PendingPunch p {true, c.track, PunchKind((c.index & 0xff) % punch_count), std::clamp<int>(c.value, 0, 100) / 100.0f,
                        unsigned(c.pattern & 3), unsigned((c.pattern >> 4) & 3) % 3};
        unsigned s = c.position ? (c.position - 1) % live_slots : 0;
        live_punch = true;
        if (punch_from_song) { punch.stop(); punch_from_song = false; } // a punch by hand wins over the song's
        if (running && !(c.pattern & 64)) live[s].pending = p; // quantized: from the snap grid's next line
        else { live[s].pending.waiting = false; start_live(s, p); }
        break;
    }
    case Type::PunchOff:
        for (unsigned s = 0; s < live_slots; ++s) {
            if (c.track != 254 && (c.position ? (c.position - 1) % live_slots : 0) != s) continue;
            live[s].pending.waiting = false; // let go before it began
            live[s].punch.stop();
        }
        live_punch = false;
        for (auto& l : live) live_punch |= l.punch.on() || l.pending.waiting;
        break;
    case Type::PunchDepth:
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
        case 5: a.pressure_velocity = c.value != 0; break;
        }
        break;
    }
    case Type::ArpKey:
        if (c.value && slot.freeze && slot.freeze->deep) break; // freeze plan F2d
        if (c.value && slot.freeze) wake(slot);
        if (c.value) slot.arp.press(int(c.index) | int(c.position & 0xff) << 16, c.value); else slot.arp.release(c.index); // position: a 16 Levels cutoff + 1
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
        // The take counts its own (device) frames: the auto end's quiet, kept at the model rate, scaled to them.
        auto quiet = uint32_t(double(auto_quiet) * dsp::sample_rate() / dsp::model_rate + 0.5);
        take.arm(float(c.value), length, c.index, cue_track >= 0, length ? 0 : quiet, auto_silence_db);
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
    case Type::LoopMonitor: loop_monitor.store(uint8_t(std::clamp<int>(c.value, 0, int(Monitor::Count) - 1)), std::memory_order_relaxed); break;
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
        case 6: settings.sidechain = c.value != 0; break;
        case 7: settings.key = uint8_t(std::clamp<int>(c.value, 0, track_count - 1)); break;
        case 8: settings.key_pad = c.value < 0 || c.value >= int(sampler_pads) ? MasterComp::whole_kit : uint8_t(c.value); break;
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
        if (slot.freeze && slot.freeze->deep) break; // freeze plan F2d
        if (slot.freeze) wake(slot);
        if (slot.engine == &slot.sampler) {
            EngineCall call;
            call.op = EngineCall::Op::PlayPad; call.a = c.index; call.b = c.value; call.c = int(c.pattern) - 64; call.d = int(c.position) - 1;
            if (!keep_call(slot, call)) make_call(c.track % track_count, slot, call);
        }
        midi_note(c.track % track_count, int(sampler_first_note + c.index), c.value, frame_now + block_at, -1); // 16 Levels: the pad's note
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
        if (slot.freeze && slot.freeze->deep) break; // freeze plan F2d
        if (slot.freeze) wake(slot);
        if (slot.engine == &slot.sampler) {
            EngineCall call;
            call.op = EngineCall::Op::Audition; call.a = c.pad.sample; call.b = int32_t(c.pad.start); call.c = int32_t(c.pad.end); call.d = c.value;
            if (!keep_call(slot, call)) make_call(c.track % track_count, slot, call);
        }
        break;
    case Type::Pressure: {
        if (!slot.engine) break;
        if (slot.kind == EngineKind::Midi && slot.base[midi_track::PressureOut] != midi_track::PressureOff && c.index < 128) { // roadmap P4 (M2)
            bool poly = slot.base[midi_track::PressureOut] == midi_track::PressurePoly;
            MidiOutEvent e {poly ? MidiOutEvent::Kind::PolyPressure : MidiOutEvent::Kind::ChannelPressure,
                            uint8_t(std::clamp<int>(slot.base[midi_track::Channel], 1, 16)), uint8_t(c.index), uint8_t(std::clamp<int>(c.value, 0, 127)), 0,
                            frame_now + block_at};
            e.dest = midi_track::dest_of(slot.base[midi_track::Port]); e.track = c.track % track_count;
            emit(e);
        }
        float value = std::clamp<int>(c.value, 0, 127) / 127.0f;
        engine_pressure(slot, c.index, value);
        // The arp's notes follow the pressure of the key they came from, while it is held.
        if (slot.arp.on) {
            slot.arp.press_harder(c.index, value);
            if (slot.arp_from >= 0 && arp_named(slot.arp_from) == int(c.index) && slot.arp_played >= 0 && slot.arp_played != int(c.index))
                engine_pressure(slot, slot.arp_played, value);
        }
        break;
    }
    case Type::Wheel:
        slot.wheel = std::clamp(c.value / 1000.0f, 0.0f, 1.0f);
        engine_wheel(slot, slot.wheel);
        if (slot.kind == EngineKind::Midi) { // roadmap P4 (M2): the fader's or MIDI in's mod wheel as CC 1
            MidiOutEvent e {MidiOutEvent::Kind::Control, uint8_t(std::clamp<int>(slot.base[midi_track::Channel], 1, 16)), 1,
                            uint8_t(std::lround(slot.wheel * 127)), 0, frame_now + block_at};
            e.dest = midi_track::dest_of(slot.base[midi_track::Port]); e.track = c.track % track_count;
            emit(e);
        }
        break;
    case Type::Bend: // the fader's pitch bend on a PLUGIN track (a MIDI track's goes out as its BEND)
        slot.bend = std::clamp(c.value / 8192.0f, -1.0f, 1.0f);
        engine_bend(slot, slot.bend);
        break;
    case Type::Level: slot.mix_level = c.value; set_level(slot, c.value); if (mods_on) mod_note(c.track % track_count, motion_level, 0, c.value, true); break; // roadmap P4: kept, Motion borrows it
    case Type::Pan: slot.mix_pan = c.value; set_pan(slot, c.value); if (mods_on) mod_note(c.track % track_count, motion_pan, 0, c.value, true); break;
    case Type::Send: // index: the bus
        slot.mix_sends[c.index % bus_count] = int16_t(std::clamp<int>(c.value, 0, 100));
        slot.sends[c.index % bus_count] = std::clamp<int>(c.value, 0, 100) / 100.0f;
        if (mods_on) mod_note(c.track % track_count, motion_send(c.index), 0, slot.mix_sends[c.index % bus_count], true);
        break;
    case Type::Mute: slot.mute = c.value != 0; midi_silenced(); break;
    case Type::Solo: slot.solo = c.value != 0; midi_silenced(); break;
    case Type::PatternData:
        if (c.data) {
            auto& at = patterns[c.track % track_count][c.pattern % pattern_count];
            Pattern* old = at;
            at = c.data;
            // Roadmap P5: the pattern playing changed its follow action or length: what follows is planned again.
            bool replan = running && !song_mode && slot.pattern == c.pattern % pattern_count && !slot.silent && old &&
                          (old->follow != c.data->follow || old->length != c.data->length);
            bool program = old && slot.kind == EngineKind::Midi && slot.pattern == c.pattern % pattern_count && old->program != c.data->program;
            retire(old);
            if (replan) plan_follow(c.track % track_count);
            if (program) midi_program(c.track % track_count, frame_now, false); // its own program turned: sent now
        }
        break;
    case Type::Pattern:
        if (running && !song_mode) slot.queued = uint8_t(c.value % pattern_count);
        else if (!song_mode) { slot.pattern = uint8_t(c.value % pattern_count); slot.queued = 255; lead = slot.pattern; follow_tempo(); }
        patterns_now[c.track % track_count].store(slot.pattern, std::memory_order_relaxed);
        if (!running && slot.kind == EngineKind::Midi) midi_program(c.track % track_count, frame_now, false); // roadmap P4: its own program
        break;
    case Type::SongClip: if (c.index < song_clips) song.clips[c.index] = c.clip; break;
    case Type::SongCount: song.count = uint16_t(std::clamp<int>(c.value, 0, song_clips)); break;
    case Type::SongLoop: song.loop_start = c.index; song.loop_end = uint16_t(c.value); break;
    case Type::EditLoop:
        slot.loop_from = c.position;
        slot.loop_ticks = uint32_t(c.index) | uint32_t(uint16_t(c.value)) << 16;
        break;
    case Type::Chained: // roadmap P5: a chain is its track's order, its patterns' follow actions wait
        slot.chained = c.value != 0;
        if (slot.chained) slot.follow_do = 0;
        else if (running && !song_mode && !slot.silent) plan_follow(c.track % track_count);
        break;
    case Type::SongTempo: // roadmap P5: the song's tempo lane, a point at a time (sync_song)
        if (c.index < song_tempo_points)
            song.tempo.points[c.index] = {uint16_t(std::min<uint32_t>(c.position, song_bars - 1)), uint16_t(std::clamp<int>(c.value, 300, 3000)), c.pattern != 0};
        break;
    case Type::SongTempoCount:
        song.tempo.count = uint8_t(std::clamp<int>(c.value, 0, int(song_tempo_points)));
        follow_lane();
        break;
    case Type::SceneTempo: // roadmap P5: on the bar the scene's patterns launch on; stopped, at once
        if (running && !song_mode) scene_tempo = uint16_t(std::clamp<int>(c.value, 300, 3000));
        else { global_tempo = uint16_t(std::clamp<int>(c.value, 300, 3000)); scene_tempo = 0; follow_tempo(); }
        break;
    case Type::Play:
        if (running) break;
        mtc_off(); // MTC in places it right after (MtcIn locate)
        running = true; until_tick = 0;
        drop_hits();
        for (auto& s : slots) { s.weighed = s.went = false; s.keys_weighed = {}; s.keys_went = {}; } // PRE and NEI start afresh
        tick_now = c.value == 1 ? 0 : c.position;
        arp_ticks = 0; until_arp = 0; // the repeats line up with the bar, so their swing falls with the sequencer's
        arp_origin = tick_now;         // and Seq Record writes each where it fell (roadmap P1)
        song_mode = c.value == 1; song_bar_now = c.index;
        count_left = c.track == 1 ? bar : 0;
        count_beat.store(count_left ? 1 : 0, std::memory_order_relaxed);
        count_rest.store(count_left, std::memory_order_relaxed);
        count_from.store(tick_now, std::memory_order_relaxed);
        loop_restart = true;
        for (auto& s : slots) {
            s.start = 0; s.clip = -1; s.silent = song_mode;
            if (s.queued != 255) { s.pattern = s.queued; s.queued = 255; }
        }
        in_song.store(song_mode, std::memory_order_relaxed);
        stopped_now.store(0, std::memory_order_relaxed);
        for (unsigned t = 0; t < track_count; ++t) plan_follow(t); // roadmap P5: from each pattern's top (not in the song)
        follow_lane(); // roadmap P5: the song's tempo where it starts, its count-in too
        if (render_out.cued()) { // a render starts with the first frame played
            if (bounce_song != song_mode || count_left) render_out.cancel();
            else render_out.begin(0);
        }
        break;
    case Type::Stop:
        running = false;
        drop_hits(); // a ratchet's hits still to come go with it
        mtc_off();
        sync_stop(frame_now);
        midi_all_off(255, frame_now);
        render_out.cancel(); // a render stopped by hand is dropped; one in its tail ends there
        if (take.state() == TakeRecorder::State::Cued) take.stop();
        cue_track = -1;
        count_left = 0; count_beat.store(0, std::memory_order_relaxed); count_rest.store(0, std::memory_order_relaxed);
        if (punch_from_song) { punch.stop(); punch_from_song = false; }
        for (auto& k : kept) { if (k.on) k.punch.stop(); k.on = k.due = false; } // pad-fx C
        song_mode = false;
        in_song.store(false, std::memory_order_relaxed);
        follow_lane(); // roadmap P5: the project's tempo again
        for (unsigned t = 0; t < track_count; ++t) {
            positions_now[t].store(-1, std::memory_order_relaxed);
            for (auto& skip : slots[t].skips) skip.key = -1;
            if (slots[t].queued != 255) { slots[t].pattern = slots[t].queued; slots[t].queued = 255; }
            slots[t].silent = false; slots[t].clip = -1;
            slots[t].follow_do = 0; // roadmap P5
            patterns_now[t].store(slots[t].pattern, std::memory_order_relaxed);
        }
        for (auto& s : slots) restore(s);
        for (auto& s : slots) {
            for (auto& off : s.offs) if (off.key >= 0 && s.engine) { s.engine->note_off(off.key); off.key = -1; }
            s.held = {};
            s.print.release(dsp::sample_rate()); s.print_on = false; // freeze plan F2b: a Ring print's tail rings on
        }
        freeze_at_bar(); // stopped: no bar to wait for
        break;
    case Type::Tempo:
        global_tempo = uint16_t(std::clamp<int>(c.value, 300, 3000));
        scene_tempo = 0; // set by hand after a scene's: the hand's
        follow_lane();   // the song's lane starts from it (roadmap P5)
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
        arp_ticks = 0; until_arp = 0; arp_origin = tick_now; // the repeats from it
        loop_restart = true;
        for (auto& s : slots) { s.start = 0; for (auto& skip : s.skips) skip.key = -1; }
        for (unsigned t = 0; t < track_count; ++t) plan_follow(t); // roadmap P5: counted from the patterns' top again
        tick_count.store(tick_now, std::memory_order_relaxed);
        break;
    case Type::Recorded: {
        Speed sp = speed_of(speeds_now[c.track % track_count].load(std::memory_order_relaxed)); // written ticks to played ones
        for (auto& skip : slot.skips)
            if (skip.key < 0 || tick_now > skip.until) {
                skip = {int16_t(c.index), c.pattern, uint32_t(uint64_t(c.position) * sp.den / sp.num), tick_now + 2 * ticks_per_quarter};
                break;
            }
        break;
    }
    case Type::Swing: swing = uint8_t(std::clamp<int>(c.value, 50, 75)); break;
    case Type::Fill: fill = c.value != 0; fill_now.store(fill, std::memory_order_relaxed); break;
    case Type::TrackSpeed: speeds_now[c.track % track_count].store(uint8_t(std::clamp<int>(c.value, 0, int(speed_count) - 1)), std::memory_order_relaxed); break;
    case Type::Freeze: { // freeze plan F2b
        if (c.track >= track_count) { retire_freeze(c.freeze); break; }
        retire_freeze(freeze_next[c.track]); // a later one wins over one still waiting for its bar
        freeze_next[c.track] = nullptr;
        if (c.freeze && running && c.value) freeze_next[c.track] = c.freeze;
        else set_freeze(c.track, c.freeze);
        break;
    }
    }
}

// --- Freeze plan F2b: prints played instead of a track's engine and chain ---

void EngineHost::retire_freeze(FreezePack* pack) {
    if (!pack) return;
    if (done_freezes.push(pack)) return;
    for (auto& q : freeze_limbo) if (!q) { q = pack; return; }
    // limbo full too: kept (a leak) rather than freed here
}

// The track's prints from now: the voices of the last ones fade out (their pack freed once they have), the engine
// stays awake until it has been quiet a while (its notes ring on), and the print takes over at the next tick.
void EngineHost::set_freeze(unsigned t, FreezePack* pack) {
    auto& slot = slots[t % track_count];
    if (slot.freeze == pack) return;
    if (slot.freeze || slot.print.sounding()) {
        if (slot.freeze_leaving) { slot.print.cut_all(); retire_freeze(slot.freeze_leaving); slot.freeze_leaving = nullptr; }
        slot.print.fade_all(dsp::sample_rate());
        if (slot.freeze) slot.freeze_leaving = slot.freeze;
    }
    slot.freeze = pack;
    if (pack) slot.print_post = pack->tap == PrintTap::PostFx;
    slot.print_on = false;
    wake(slot);
}

void EngineHost::freeze_at_bar() {
    for (unsigned t = 0; t < track_count; ++t)
        if (freeze_next[t]) { set_freeze(t, freeze_next[t]); freeze_next[t] = nullptr; }
}

double EngineHost::pattern_frames(uint32_t ticks) const {
    const double base = tick_length(), on = base * swing / 50.0, off = base * (100 - swing) / 50.0; // as tick_samples()
    const uint32_t pair = 2 * sixteenth_ticks, pairs = ticks / pair, rest = ticks % pair;
    return double(pairs) * sixteenth_ticks * (on + off) + std::min<uint32_t>(rest, sixteenth_ticks) * on
         + (rest > sixteenth_ticks ? (rest - sixteenth_ticks) * off : 0.0);
}

// On a tick of a frozen track's pattern: its print plays it, where the pattern is, unless it has none, or none that
// fits it now (another rate, or a pass of another length: the tempo moved since it was printed): then it plays live.
void EngineHost::print_tick(unsigned, Slot& slot, uint32_t at, uint32_t length) {
    const float rate = dsp::sample_rate();
    PrintClip* clip = slot.freeze->clips[slot.pattern % pattern_count];
    if (clip && (clip->rate != uint32_t(std::lround(rate)) || std::fabs(double(clip->pass) - pattern_frames(length)) > 64)) clip = nullptr;
    slot.print_on = clip != nullptr;
    if (!clip && !slot.freeze->deep) wake(slot); // freeze plan F2d: a deep freeze stays asleep (its plug-ins are gone)
    slot.print.follow(clip, at, pattern_frames(at), tick_samples(tick_now), rate);
}

// --- One dispatch a block (roadmap P2) ---

// A call the sequencing pass makes to a track's engine: kept for the track's job, at its frame, once the pass is past
// the block's first frame; false when it goes straight in (nothing of the block rendered yet, the old way, or no
// engine to play it). A list that fills up goes in whole at the block's start, and the rest of the block's calls
// after it, so their order holds and only their frames are lost (counted).
bool EngineHost::keep_call(Slot& slot, const EngineCall& call) {
    if (!deferring || !block_at || !slot.engine) return false;
    if (slot.call_count == calls_a_block) {
        unsigned t = unsigned(&slot - slots.data());
        for (unsigned k = 0; k < slot.call_count; ++k) make_call(t, slot, slot.calls[k]);
        slot.call_count = calls_a_block + 1; // full: straight in from now on
        calls_full.fetch_add(1, std::memory_order_relaxed);
    }
    if (slot.call_count > calls_a_block) return false;
    auto& kept = slot.calls[slot.call_count++];
    kept = call;
    kept.frame = uint16_t(block_at);
    return true;
}

void EngineHost::make_call(unsigned t, Slot& slot, const EngineCall& c) {
    using Op = EngineCall::Op;
    Engine* e = slot.engine;
    if (!e) return;
    switch (c.op) {
    case Op::NoteOn:
        if (c.flag) e->set_accent(c.x);
        e->note_on(c.a, c.b);
        if (c.flag) e->set_accent(1);
        break;
    case Op::NoteOff: e->note_off(c.a); break;
    case Op::Slide: e->set_accent(c.x); e->slide(c.a, c.b, c.c, c.flag); e->set_accent(1); break;
    case Op::Param: e->set_param(c.a, c.b); break;
    case Op::Wheel: e->set_wheel(c.x); break;
    case Op::Bend: e->set_bend(c.x); break;
    case Op::Pressure: e->set_pressure(c.a, c.x); break;
    case Op::Mod: e->set_mod(unsigned(c.a), ModRoute {uint8_t(c.b), uint8_t(c.c), int8_t(c.d)}); break;
    case Op::PlayPad: if (e == &slot.sampler) slot.sampler.play_pad(unsigned(c.a), c.b, c.c, c.d); break;
    case Op::Audition: if (e == &slot.sampler) slot.sampler.audition(uint16_t(c.a), uint32_t(c.b), uint32_t(c.c), c.d); break;
    case Op::MovePad: slot.sampler.move_pad(unsigned(c.a), PadParam(c.b), c.c); break;
    case Op::RestorePad: slot.sampler.restore_pad(unsigned(c.a), PadParam(c.b)); break;
    case Op::RestorePads: slot.sampler.restore_pads(); break;
    case Op::PadFxParam: (*pad_fx)[t % track_count][unsigned(c.a) % sampler_pads][unsigned(c.b) % pad_slots].set_param(unsigned(c.c), c.d); break;
    }
}

// accent < 0: none set around it (a note by hand); else the sequencer's, back to 1 after.
void EngineHost::engine_note_on(Slot& slot, int key, int velocity, float accent) {
    EngineCall call;
    call.op = EngineCall::Op::NoteOn; call.a = key; call.b = velocity; call.flag = accent >= 0; call.x = accent;
    if (!keep_call(slot, call)) make_call(unsigned(&slot - slots.data()), slot, call);
}

void EngineHost::engine_note_off(Slot& slot, int key) {
    EngineCall call;
    call.op = EngineCall::Op::NoteOff; call.a = key;
    if (!keep_call(slot, call)) make_call(unsigned(&slot - slots.data()), slot, call);
}

void EngineHost::engine_slide(Slot& slot, int from, int to, int velocity, bool glide, float accent) {
    EngineCall call;
    call.op = EngineCall::Op::Slide; call.a = from; call.b = to; call.c = velocity; call.flag = glide; call.x = accent;
    if (!keep_call(slot, call)) make_call(unsigned(&slot - slots.data()), slot, call);
}

void EngineHost::engine_param(Slot& slot, int id, int value) {
    EngineCall call;
    call.op = EngineCall::Op::Param; call.a = id; call.b = value;
    if (!keep_call(slot, call)) make_call(unsigned(&slot - slots.data()), slot, call);
}

void EngineHost::engine_wheel(Slot& slot, float value) {
    EngineCall call;
    call.op = EngineCall::Op::Wheel; call.x = value;
    if (!keep_call(slot, call)) make_call(unsigned(&slot - slots.data()), slot, call);
}

void EngineHost::engine_bend(Slot& slot, float value) {
    EngineCall call;
    call.op = EngineCall::Op::Bend; call.x = value;
    if (!keep_call(slot, call)) make_call(unsigned(&slot - slots.data()), slot, call);
}

void EngineHost::engine_pressure(Slot& slot, int key, float value) {
    EngineCall call;
    call.op = EngineCall::Op::Pressure; call.a = key; call.x = value;
    if (!keep_call(slot, call)) make_call(unsigned(&slot - slots.data()), slot, call);
}

// The engine's frames [at, at + frames) into its buffers; a kit's pads that go apart (Sidechain's key pad, Knob FX)
// into theirs at the same frames.
void EngineHost::render_engine(unsigned t, Slot& slot, unsigned at, unsigned frames) {
    if (slot.engine == &slot.sampler) {
        bool pad_key = keyed_pad == int(t);
        slot.sampler.set_key_pad(pad_key ? int(comp.key_pad()) : -1, pad_key ? key_pad_l + at : nullptr, pad_key ? key_pad_r + at : nullptr);
        slot.sampler.set_knob_out(knob_heard ? slot.knob_pad_l + at : nullptr, knob_heard ? slot.knob_pad_r + at : nullptr);
        for (unsigned b = 0; b < bus_count; ++b) slot.sampler.set_send_out(b, slot.pad_sends->l[b] + at, slot.pad_sends->r[b] + at);
    }
    if (slot.engine->stereo()) slot.engine->render_stereo(slot.left + at, slot.right + at, frames);
    else slot.engine->render(slot.buffer + at, frames);
    if (slot.engine == &slot.sampler) { slot.knob_pad_fed |= slot.sampler.knob_fed(); slot.send_pad_fed |= slot.sampler.sends_fed(); }
}

// A track's engine over the whole block (its job): in runs from one kept call to the next, each call made on its frame.
void EngineHost::play_calls(unsigned t, Slot& slot, unsigned frames) {
    unsigned count = slot.call_count > calls_a_block ? 0 : slot.call_count, at = 0, k = 0, cut = 0; // spilled: all made
    if (slot.freeze && !slot.awake) { // freeze plan F2b: a frozen track's engine asleep takes its calls, renders nothing
        while (k < count) make_call(t, slot, slot.calls[k++]);
        slot.call_count = 0;
        return;
    }
    while (at < frames) {
        while (k < count && slot.calls[k].frame <= at) make_call(t, slot, slot.calls[k++]);
        unsigned to = k < count ? std::min<unsigned>(slot.calls[k].frame, frames) : frames;
        if (split_cuts) { // tests: where the old way cut it too
            while (cut < cut_count && cuts[cut] <= at) ++cut;
            if (cut < cut_count) to = std::min<unsigned>(to, cuts[cut]);
        }
        render_engine(t, slot, at, to - at);
        at = to;
    }
    while (k < count) make_call(t, slot, slot.calls[k++]); // none: every call falls inside the block
    slot.call_count = 0;
}

void EngineHost::schedule_off(Slot& slot, int key, int32_t samples) {
    for (auto& off : slot.offs)
        if (off.key == key) { off.samples = samples; return; } // a retrigger moves its end
    for (auto& off : slot.offs)
        if (off.key < 0) { off.key = key; off.samples = samples; return; }
    engine_note_off(slot, key); // no room: end it now rather than hang
}

void EngineHost::cancel_off(Slot& slot, int key) {
    for (auto& off : slot.offs) if (off.key == key) off.key = -1;
}

void EngineHost::run_offs(Slot& slot, unsigned frames) {
    for (auto& off : slot.offs) {
        if (off.key < 0) continue;
        off.samples -= int32_t(frames);
        if (off.samples <= 0) {
            engine_note_off(slot, off.key);
            if (off.key == slot.held.key) slot.held = {}; // nothing took it over
            off.key = -1;
        }
    }
}

void EngineHost::run_chain(unsigned chain, float* left, float* right, unsigned frames) {
    for (unsigned k = 0; k < max_slots; ++k) {
        auto& effect = fx[chain][k];
        if (effect.kind() == EffectKind::None) break; // slots are packed from the front
        if (fx_key[chain][k]) key_effect(chain, k, effect);
        if (effect.kind() == EffectKind::Plugin) { // the rack's plug-in for the entry it names; none yet: through
            auto* runner = runner_of(unsigned(effect.param(0)));
            if (runner && !effect.bypassed()) runner->render(left, right, frames);
            continue;
        }
        effect.process(left, right, frames);
    }
}

// Roadmap P4: the frames chain `chain` holds its sound back: each plug-in's latency (its rate converter's in it) and
// each effect's (a Limiter looking ahead), bypassed ones left out (they pass the sound straight through).
unsigned EngineHost::chain_latency(unsigned chain) const {
    unsigned sum = 0;
    for (const auto& effect : fx[chain % chain_count]) {
        if (effect.kind() == EffectKind::None) break;
        if (effect.kind() == EffectKind::Plugin) {
            const auto* runner = runner_of(unsigned(effect.param(0)));
            if (runner && !effect.bypassed()) sum += runner->latency();
        } else {
            sum += effect.latency();
        }
    }
    return sum;
}

// Roadmap P4, modulators. A new one (or one changed) keeps where its lanes stand when they are the same lanes
// (`keep_centers`: Motion may have them elsewhere); a lane it no longer moves goes back to where it stands.
void EngineHost::set_modulator(unsigned n, const Modulator& m, bool keep_centers) {
    auto& r = mod_runs[n % modulator_count];
    for (unsigned i = 0; i < modulator_targets; ++i) {
        const auto& was = r.m.targets[i];
        const auto& now = m.targets[i];
        bool same = was.used() && now.used() && was.track == now.track && was.param == now.param && was.pad == now.pad;
        if (was.used() && (!same || m.kind == ModKind::Off)) mod_write(was.track, was.param, was.pad, r.center[i]); // let go
        if (now.used()) {
            if (!same || !keep_centers) r.center[i] = now.own;
            r.own[i] = now.own;
            r.fresh[i] = true;
        }
    }
    if (r.m.kind != m.kind || r.m.shape != m.shape) { r.last = 0; r.next = 0; }
    r.m = m;
    mods_on = false;
    for (const auto& run : mod_runs) mods_on |= run.m.kind != ModKind::Off && run.m.target_count();
}

void EngineHost::mod_note(unsigned t, unsigned param, unsigned pad, int value, bool own) {
    bool pad_lane = pad_param_of(param) >= 0;
    for (auto& r : mod_runs)
        for (unsigned i = 0; i < modulator_targets; ++i) {
            const auto& g = r.m.targets[i];
            if (!g.used() || g.track != t || g.param != param || (pad_lane && g.pad != pad)) continue;
            r.center[i] = value;
            if (own) r.own[i] = value;
            r.fresh[i] = true;
        }
}

void EngineHost::mod_note_any(unsigned param, int value, bool own) {
    for (auto& r : mod_runs)
        for (unsigned i = 0; i < modulator_targets; ++i) {
            const auto& g = r.m.targets[i];
            if (!g.used() || g.param != param) continue;
            r.center[i] = value;
            if (own) r.own[i] = value;
            r.fresh[i] = true;
        }
}

void EngineHost::mod_back(unsigned t, int param, unsigned pad) {
    for (auto& r : mod_runs)
        for (unsigned i = 0; i < modulator_targets; ++i) {
            const auto& g = r.m.targets[i];
            if (!g.used() || (g.track != t && !is_master_lane(g.param))) continue;
            if (param >= 0 && (g.param != unsigned(param) || (pad_param_of(g.param) >= 0 && g.pad != pad))) continue;
            r.center[i] = r.own[i];
            r.fresh[i] = true;
        }
}

// A modulated value to its lane, as Motion would write it, telling no modulator (at a block's start: straight in).
void EngineHost::mod_write(unsigned t, unsigned param, unsigned pad, int value) {
    auto& slot = slots[t % track_count];
    if (is_mix_lane(param)) {
        bool mix = slot.mix_moved, master = slot.master_moved; // a modulator's swing is not Motion's: Stop leaves it be
        move_mix(t, slot, param, value);
        slot.mix_moved = mix; slot.master_moved = master;
        return;
    }
    if (param == motion_wheel) { if (slot.engine) engine_wheel(slot, std::clamp(value / 127.0f, 0.0f, 1.0f)); return; }
    if (param == motion_bend) { if (slot.engine) engine_bend(slot, std::clamp(value / 64.0f, -1.0f, 1.0f)); return; }
    if (int which = pad_param_of(param); which >= 0) {
        if (slot.kind == EngineKind::Sampler) slot.sampler.move_pad(pad % sampler_pads, PadParam(which), value);
        return;
    }
    if (param >= motion_fx_base) {
        unsigned k = (param - motion_fx_base) / effect_params, q = (param - motion_fx_base) % effect_params;
        if (k < max_slots) fx[t % track_count][k].set_param(q, value);
        return;
    }
    if (slot.engine && param < max_params) engine_param(slot, int(param), value);
}

// Each block's start: every modulator a step on (an LFO's cycle, free or on the song's clock; a follower's level of
// what its source played the block before), and its lanes written where they stand plus its swing, when that moved.
void EngineHost::run_modulators(unsigned frames) {
    for (unsigned n = 0; n < modulator_count; ++n) {
        auto& r = mod_runs[n];
        const auto& m = r.m;
        if (m.kind == ModKind::Off || !m.target_count()) { mod_now[n].store(0, std::memory_order_relaxed); continue; }
        bool follower = m.kind == ModKind::Follower;
        if (follower) {
            unsigned s = m.source % (track_count + bus_count);
            const float* l = s < track_count ? slots[s].left : bus_l[s - track_count];
            const float* rr = s < track_count ? slots[s].right : bus_r[s - track_count];
            float peak = 0;
            if (s >= track_count || slots[s].engine)
                for (unsigned f = 0; f < frames; ++f) peak = std::max(peak, std::max(std::fabs(l[f]), std::fabs(rr[f])));
            double ms = peak > r.env ? follower_attack_ms(m.attack) : follower_release_ms(m.release);
            float k = float(1 - std::exp(-double(frames) / std::max(1.0, ms * 0.001 * dsp::sample_rate())));
            r.env += (peak - r.env) * k;
            r.value = std::min(1.0f, r.env * 2);
        } else {
            double before = r.phase;
            if (m.sync) r.phase = fx_clock->beats / lfo_division_quarters(m.division);
            else r.phase += lfo_hertz(m.rate) * frames / dsp::sample_rate();
            if (std::floor(r.phase) != std::floor(before)) { // a new cycle: a new random value for S&H and Drift
                mod_seed ^= mod_seed << 13; mod_seed ^= mod_seed >> 17; mod_seed ^= mod_seed << 5;
                r.last = r.next;
                r.next = float(mod_seed % 20001) / 10000.0f - 1.0f;
            }
            if (!m.sync && r.phase >= 1) r.phase -= std::floor(r.phase);
            r.value = lfo_value(m.shape, r.phase, r.last, r.next);
        }
        mod_now[n].store(r.value, std::memory_order_relaxed);
        for (unsigned i = 0; i < modulator_targets; ++i) {
            const auto& g = m.targets[i];
            if (!g.used()) continue;
            int v = modulated(g, r.center[i], r.value, follower);
            if (v == r.written[i] && !r.fresh[i]) continue;
            mod_write(g.track, g.param, g.pad, v);
            r.written[i] = v; r.fresh[i] = false;
        }
    }
}

// Roadmap P4, keyed Compressors and Gates, at each block's start: which tracks wait for the tracks keying them, and
// which sources' outputs are kept for the next block. A track keyed by a track that itself waits hears it a block late.
void EngineHost::plan_keys() {
    keyed_track.fill(false);
    key_sources = 0;
    for (unsigned n = 0; n < chain_count; ++n)
        for (unsigned k = 0; k < max_slots; ++k) {
            if (fx[n][k].kind() == EffectKind::None) break;
            unsigned key = fx_key[n][k];
            if (!key || !fx_keyable(uint8_t(fx[n][k].kind()))) continue;
            key_sources |= 1u << (key - 1); // tracks 0-15, buses 16-19
            if (n < track_count && fx_key_is_track(key) && key - 1 != n) keyed_track[n] = true;
        }
}

// The key effect `slot` of `chain` hears now: a track this block when its chain has run (every track before a bus's or
// the master's chain; one that does not wait before a waiting track's), else that source the block before.
void EngineHost::key_effect(unsigned chain, unsigned slot, Effect& effect) {
    unsigned key = fx_key[chain][slot];
    if (!fx_keyable(uint8_t(effect.kind())) || !key || key >= fx_keys) return;
    unsigned source = key - 1;
    if (fx_key_is_track(key)) {
        unsigned t = source;
        bool now = slots[t].engine && ((chain >= track_count && chain <= master_chain) || (chain < track_count && keyed_track[chain] && !keyed_track[t] && t != chain));
        if (now) { effect.set_key(slots[t].left, slots[t].right); return; }
        if (!slots[t].engine) { effect.set_key(silence_block, silence_block); return; }
    } else if (chain == master_chain) {
        unsigned b = source - track_count;
        effect.set_key(bus_l[b], bus_r[b]);
        return;
    }
    effect.set_key(key_prev_l[source].data(), key_prev_r[source].data());
}

// What each key source played this block (a track after its chain, a bus after its chain), for keys a block late.
void EngineHost::keep_keys(unsigned frames) {
    for (unsigned s = 0; s < track_count + bus_count; ++s) {
        if (!((key_sources >> s) & 1)) continue;
        const float* l = s < track_count ? slots[s].left : bus_l[s - track_count];
        const float* r = s < track_count ? slots[s].right : bus_r[s - track_count];
        bool silent = s < track_count && !slots[s].engine;
        for (unsigned f = 0; f < frames; ++f) { key_prev_l[s][f] = silent ? 0.0f : l[f]; key_prev_r[s][f] = silent ? 0.0f : r[f]; }
    }
}

// Roadmap P4, plug-in delay compensation, at each block's start: every track and loop track meets the latest of them
// as it leaves its chain (a PLUGIN track's instrument and its chain); the direct mix waits for the latest bus or Knob
// FX, and each of those for the latest; the master's chain delays everything alike. Kits' pads into Knob FX and the
// Sidechain's key pad leave their track before its chain, so they wait as long as the latest track. Nothing
// allocates: the lines were made at start and only change where they read.
void EngineHost::plan_compensation() {
    Compensation plan;
    std::array<unsigned, track_count> own {};
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& slot = slots[t];
        if (!slot.engine) continue;
        unsigned engine_own = slot.engine == &slot.plugin && slot.plugin.attached() ? slot.plugin.attached()->latency() : 0;
        pdc_engines[t].store(engine_own, std::memory_order_relaxed);
        own[t] = chain_latency(t) + engine_own;
        // Freeze plan F2b: a print starts on its pattern's first tick, its track's latency taken out; at Pre FX the
        // chain after it is still late by its own.
        if (slot.freeze && slot.freeze->clips[slot.pattern % pattern_count]) own[t] = slot.freeze->tap == PrintTap::PostFx ? 0 : chain_latency(t);
        plan.tracks = std::max(plan.tracks, own[t]);
    }
    std::array<unsigned, loop_tracks> loop_own {};
    if (loops.on())
        for (unsigned n = 0; n < loop_tracks; ++n) { loop_own[n] = chain_latency(loop_chain(n)); plan.tracks = std::max(plan.tracks, loop_own[n]); }
    std::array<unsigned, bus_count> bus_own {};
    for (unsigned b = 0; b < bus_count; ++b) { bus_own[b] = chain_latency(bus_chain(b)); plan.returns = std::max(plan.returns, bus_own[b]); }
    unsigned knob_own = knob.kind() != EffectKind::None ? knob.latency() : 0;
    plan.returns = std::max(plan.returns, knob_own);
    plan.master = chain_latency(master_chain);
    constexpr unsigned reach = CompensationLines::most - block;
    plan.tracks = std::min(plan.tracks, reach); plan.returns = std::min(plan.returns, reach);
    plan.total = plan.tracks + plan.returns + plan.master;
    for (unsigned t = 0; t < track_count; ++t) plan.delay[CompensationLines::track0 + t] = plan.tracks - std::min(own[t], plan.tracks);
    for (unsigned n = 0; n < loop_tracks; ++n) plan.delay[CompensationLines::loop0 + n] = plan.tracks - std::min(loop_own[n], plan.tracks);
    for (unsigned b = 0; b < bus_count; ++b) plan.delay[CompensationLines::bus0 + b] = plan.returns - std::min(bus_own[b], plan.returns);
    plan.delay[CompensationLines::knob] = plan.returns - knob_own;
    plan.delay[CompensationLines::direct] = plan.delay[CompensationLines::aside] = plan.delay[CompensationLines::key] = plan.returns;
    plan.delay[CompensationLines::knob_pads] = plan.delay[CompensationLines::key_pad] = plan.tracks;
    for (unsigned b = 0; b < bus_count; ++b) plan.delay[CompensationLines::pad_sends0 + b] = plan.tracks; // they leave before the chain
    // On while anything is late, and for one block after it all came back to none (the lines ease back).
    bool late = plan.tracks || plan.returns;
    plan.on = late || (pdc.on && (pdc.tracks || pdc.returns));
    pdc = plan;
    pdc_total.store(plan.total, std::memory_order_relaxed);
    pdc_master.store(plan.master, std::memory_order_relaxed);
    for (unsigned t = 0; t < track_count; ++t) pdc_tracks[t].store(own[t], std::memory_order_relaxed);
    for (unsigned b = 0; b < bus_count; ++b) pdc_buses[b].store(bus_own[b], std::memory_order_relaxed);
}

// The rack's plug-ins, each into its entry; the one it replaces goes back to be freed (held in limbo while the queue
// is full; with limbo full too the offer waits in its queue for the next block).
void EngineHost::take_plugins() {
    for (auto& q : plugin_limbo) if (q && retired_plugins.push(q)) q = nullptr;
    PluginOffer offer;
    while (true) {
        bool room = false;
        for (auto q : plugin_limbo) room |= q == nullptr;
        if (!room || !plugin_offers.pop(offer)) break;
        if (offer.entry >= max_plugins) continue;
        PluginRunner* old = runners[offer.entry];
        if (old && !retired_plugins.push(old))
            for (auto& q : plugin_limbo) if (!q) { q = old; break; }
        runners[offer.entry] = offer.runner;
        for (auto& slot : slots)
            if (slot.kind == EngineKind::Plugin && slot.plugin_entry == offer.entry) slot.plugin.attach(offer.runner);
            else if (slot.plugin.attached() == old && old) slot.plugin.attach(nullptr);
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
    if (slot.kind == EngineKind::Sampler) { // a kit pad's own values, if motion moved them
        EngineCall call;
        call.op = EngineCall::Op::RestorePads;
        if (!keep_call(slot, call)) slot.sampler.restore_pads();
    }
    for (auto& l : slot.locks) l.param = -1; // locks end with everything they moved
    slot.locks_on = 0;
    if (mods_on) mod_back(unsigned(&slot - slots.data())); // roadmap P4: modulators swing around the values as set again
    if (slot.mix_moved) { // roadmap P4: the strip back where it was set
        set_level(slot, slot.mix_level); set_pan(slot, slot.mix_pan);
        for (unsigned b = 0; b < bus_count; ++b) slot.sends[b] = slot.mix_sends[b] / 100.0f;
        slot.mix_moved = false;
    }
    if (slot.master_moved) { // and the master's chain and returns
        for (unsigned k = 0; k < max_slots; ++k) for (unsigned q = 0; q < effect_params; ++q) fx[master_chain][k].set_param(q, master_base[k][q]);
        for (unsigned b = 0; b < bus_count; ++b) returns[b] = returns_base[b] / 100.0f;
        slot.master_moved = false;
    }
    if (slot.wheel_moved) engine_wheel(slot, slot.wheel); // the mod wheel, back where the fader left it
    slot.wheel_moved = false;
    if (slot.bend_moved) engine_bend(slot, slot.bend); // and the pitch bend
    slot.bend_moved = false;
    if (!slot.moved || !slot.engine) return;
    for (unsigned p = 0; p < max_params; ++p) engine_param(slot, int(p), slot.base[p]);
    slot.moved = false;
}

// Motion at tick `at` of the track's pattern (roadmap P1: the pattern's own, at
// tick resolution): each lane starts the pattern at the track's own value,
// then follows its points; a lock holds for its ticks, then its lane goes
// back to what it holds as recorded there (else the track's own value).
void EngineHost::play_motion(unsigned t, Slot& slot, const Pattern& pattern, uint32_t at) {
    if (at == 0) restore(slot);
    if (slot.locks_on)
        for (auto& l : slot.locks) {
            if (l.param < 0 || --l.left > 0) continue;
            unsigned param = unsigned(l.param), pad = l.pad;
            l.param = -1; --slot.locks_on;
            constexpr int none = INT32_MIN;
            int value = pattern.lane_value(at, param, pad, none);
            if (value == none) restore_lane(t, slot, param, pad);
            else move_lane(t, slot, param, pad, value);
        }
    for (size_t n = pattern.motion_lower(at); n < pattern.motion.size() && pattern.motion[n].tick == at; ++n) {
        const auto& m = pattern.motion[n];
        move_lane(t, slot, m.param, m.pad, m.value);
        if (!m.lock()) continue;
        bool again = false; // a lock already on this lane starts over; else a free one
        for (auto& l : slot.locks)
            if (l.param == m.param && l.pad == m.pad) { l.left = m.hold; again = true; }
        if (again) continue;
        for (auto& l : slot.locks)
            if (l.param < 0) { l = {int16_t(m.param), m.pad, m.hold}; ++slot.locks_on; break; }
    }
}

void EngineHost::set_level(Slot& slot, int level) { slot.gain = level_gain(level); }
void EngineHost::set_pan(Slot& slot, int pan) {
    float angle = (std::clamp<int>(pan, -50, 50) + 50) / 100.0f * dsp::pi / 2;
    slot.pan_left = std::cos(angle); slot.pan_right = std::sin(angle);
}

// Roadmap P4: Motion on the mix. The track's strip eases there as a turned knob does (Ease); the master's chain and
// the returns as set from the shell.
void EngineHost::move_mix(unsigned, Slot& slot, unsigned param, int value) {
    if (param == motion_level) { set_level(slot, value); slot.mix_moved = true; }
    else if (param == motion_pan) { set_pan(slot, value); slot.mix_moved = true; }
    else if (param < motion_master_fx_base) { slot.sends[(param - motion_send(0)) % bus_count] = std::clamp(value, 0, 100) / 100.0f; slot.mix_moved = true; }
    else if (param < motion_return_base) {
        unsigned k = (param - motion_master_fx_base) / effect_params, q = (param - motion_master_fx_base) % effect_params;
        if (k < max_slots) { fx[master_chain][k].set_param(q, value); slot.master_moved = true; }
    } else {
        returns[(param - motion_return_base) % bus_count] = std::clamp(value, 0, 100) / 100.0f;
        slot.master_moved = true;
    }
}

void EngineHost::restore_mix(Slot& slot, unsigned param) {
    if (param == motion_level) set_level(slot, slot.mix_level);
    else if (param == motion_pan) set_pan(slot, slot.mix_pan);
    else if (param < motion_master_fx_base) { unsigned b = (param - motion_send(0)) % bus_count; slot.sends[b] = slot.mix_sends[b] / 100.0f; }
    else if (param < motion_return_base) {
        unsigned k = (param - motion_master_fx_base) / effect_params, q = (param - motion_master_fx_base) % effect_params;
        if (k < max_slots) fx[master_chain][k].set_param(q, master_base[k][q]);
    } else {
        unsigned b = (param - motion_return_base) % bus_count;
        returns[b] = returns_base[b] / 100.0f;
    }
}

void EngineHost::move_lane(unsigned t, Slot& slot, unsigned param, unsigned pad, int value) {
    if (mods_on) { if (is_master_lane(param)) mod_note_any(param, value, false); else mod_note(t, param, pad, value, false); } // roadmap P4: modulated around it
    if (is_mix_lane(param)) { move_mix(t, slot, param, value); return; } // roadmap P4
    if (param == motion_wheel) { // the mod wheel's lane (parity phase 8)
        if (slot.engine) { engine_wheel(slot, std::clamp(value / 127.0f, 0.0f, 1.0f)); slot.wheel_moved = true; }
        return;
    }
    if (param == motion_bend) { // a plug-in track's pitch bend lane
        if (slot.engine) { engine_bend(slot, std::clamp(value / 64.0f, -1.0f, 1.0f)); slot.bend_moved = true; }
        return;
    }
    if (param >= motion_pad_base) { // a kit pad's own value
        if (slot.kind == EngineKind::Sampler) {
            EngineCall call;
            call.op = EngineCall::Op::MovePad; call.a = int32_t(pad); call.b = int32_t(param - motion_pad_base); call.c = value;
            if (!keep_call(slot, call)) slot.sampler.move_pad(pad, PadParam(param - motion_pad_base), value);
        }
        return;
    }
    if (param >= motion_fx_base) { // one of the track's effects
        unsigned k = (param - motion_fx_base) / effect_params, q = (param - motion_fx_base) % effect_params;
        if (k < max_slots) { fx[t][k].set_param(q, value); slot.fx_moved = true; }
        return;
    }
    if (!slot.engine) return;
    engine_param(slot, int(param), value);
    if (slot.kind == EngineKind::Midi) midi_param(t, int(param), value, frame_now + block_at); // a CC lane plays out
    slot.moved = true;
}

void EngineHost::restore_lane(unsigned t, Slot& slot, unsigned param, unsigned pad) {
    if (mods_on) mod_back(t, int(param), pad); // roadmap P4: modulated around its own again
    if (is_mix_lane(param)) { restore_mix(slot, param); return; } // roadmap P4
    if (param == motion_wheel) { engine_wheel(slot, slot.wheel); return; }
    if (param == motion_bend) { engine_bend(slot, slot.bend); return; }
    if (param >= motion_pad_base) {
        if (slot.kind == EngineKind::Sampler) {
            EngineCall call;
            call.op = EngineCall::Op::RestorePad; call.a = int32_t(pad); call.b = int32_t(param - motion_pad_base);
            if (!keep_call(slot, call)) slot.sampler.restore_pad(pad, PadParam(param - motion_pad_base));
        }
        return;
    }
    if (param >= motion_fx_base) {
        unsigned k = (param - motion_fx_base) / effect_params, q = (param - motion_fx_base) % effect_params;
        if (k < max_slots) fx[t][k].set_param(q, slot.fx_base[k][q]);
        return;
    }
    if (!slot.engine || param >= max_params) return;
    engine_param(slot, int(param), slot.base[param]);
    if (slot.kind == EngineKind::Midi) midi_param(t, int(param), slot.base[param], frame_now + block_at);
}

// Swing stretches the first sixteenth of every eighth and shortens the second.
uint64_t EngineHost::tick_frame(uint32_t ticks) const {
    double until = 0;
    uint64_t frame = 0;
    for (uint32_t k = 0; k < ticks; ++k) {
        until += tick_samples(k); // as tick() adds each tick's length
        double run = std::ceil(until); // and render() plays up to the next one, until_tick going to 0 or just below
        frame += uint64_t(run);
        until -= run;
    }
    return frame;
}

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
            if (slot.kind == EngineKind::Midi) midi_program(t, frame_now + block_at, false); // roadmap P4: a pattern's own program, ahead of its notes
            patterns_now[t].store(slot.pattern, std::memory_order_relaxed);
            lead = slot.pattern;
            if (slot.silent) { slot.silent = false; stopped_now.fetch_and(~(1u << t), std::memory_order_relaxed); } // after a STOP (roadmap P5)
            plan_follow(t);
        }
        if (scene_tempo) { global_tempo = scene_tempo; scene_tempo = 0; } // roadmap P5: a scene's, with its patterns
        follow_tempo(); // BPM per pattern: the one launched last leads
        return;
    }
    if (tick_now) ++song_bar_now;
    bool rendering = bounce_song && render_out.running(); // the mixdown plays the song once, loop or not
    if (!rendering && song.looping() && song_bar_now == song.loop_end) song_bar_now = song.loop_start;
    if ((rendering || !song.looping()) && !song_recording && song_bar_now >= std::max(song.length(), 1u)) { // the end of the song
        running = false;
        if (rendering) render_out.tail(); // on until the mix falls silent
        for (auto& s : slots) { for (auto& off : s.offs) if (off.key >= 0) { engine_note_off(s, off.key); off.key = -1; } s.held = {}; }
        return;
    }
    clips_at_bar(false);
}

// Roadmap P5: what follows the pattern track t plays, from where it started (patterns, not the song; not a chained
// or stopped track): the dice say which action and after how many stretches (model/follow.h). A plan already passed
// (Play from a place, Locate) comes at the next stretch from here.
void EngineHost::plan_follow(unsigned t) {
    auto& slot = slots[t % track_count];
    slot.follow_do = 0;
    if (song_mode || slot.chained || slot.silent) return;
    const Pattern& pattern = *patterns[t % track_count][slot.pattern % pattern_count];
    unsigned action = 0, stretches = 0;
    if (!follow_plan(pattern.follow, dice, action, stretches)) return;
    uint64_t after = follow_after(pattern.follow, std::max<uint32_t>(pattern.length, 1), bar);
    uint64_t at = uint64_t(slot.start) + stretches * after;
    if (at <= tick_now) at += ((tick_now - at) / after + 1) * after;
    slot.follow_at = uint32_t(std::min<uint64_t>(at, UINT32_MAX));
    slot.follow_do = uint8_t(action);
}

// Its follow action comes, on this tick: another of the track's patterns with notes from its top, the same again, or
// the track stops (silent until a launch or Play). The shell follows by follows_of().
void EngineHost::follow(unsigned t) {
    auto& slot = slots[t % track_count];
    unsigned action = slot.follow_do;
    slot.follow_do = 0;
    PatternSet filled;
    for (unsigned p = 0; p < pattern_count; ++p) if (!patterns[t][p]->empty()) filled.set(p);
    int to = follow_target(action, slot.pattern, filled, dice);
    follows_now[t].store(follows_now[t].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (to < 0) { // STOP: what it holds lets go
        for (auto& off : slot.offs) if (off.key >= 0) { engine_note_off(slot, off.key); off.key = -1; }
        slot.held = {};
        if (slot.kind == EngineKind::Midi) midi_all_off(t, frame_now + block_at);
        slot.silent = true;
        positions_now[t].store(-1, std::memory_order_relaxed);
        stopped_now.fetch_or(1u << t, std::memory_order_relaxed);
        return;
    }
    if (slot.kind == EngineKind::Midi && unsigned(to) != slot.pattern) midi_all_off(t, frame_now + block_at); // as a launch
    slot.pattern = uint8_t(to); slot.start = tick_now;
    if (slot.kind == EngineKind::Midi) midi_program(t, frame_now + block_at, false);
    patterns_now[t].store(slot.pattern, std::memory_order_relaxed);
    lead = slot.pattern;
    follow_tempo();
    plan_follow(t);
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
            if (slot.kind == EngineKind::Midi) midi_program(t, frame_now + block_at, false); // roadmap P4
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
    if (song_mode && !song.tempo.empty()) // roadmap P5: through the song's tempo lane, bar by bar
        ticks = song.tempo.bars_at(std::max(0.0, seconds), double(bar) / ticks_per_quarter, global_tempo) * bar;
    auto at = uint32_t(std::ceil(ticks - 1e-9));
    sync_stop(frame_now); // the clocks out go on from the new place
    count_left = 0; count_beat.store(0, std::memory_order_relaxed); count_rest.store(0, std::memory_order_relaxed);
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
            for (auto& s : slots) { for (auto& off : s.offs) if (off.key >= 0) { engine_note_off(s, off.key); off.key = -1; } s.held = {}; }
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
        unsigned t = unsigned(&slot - slots.data());
        auto& a = slot.arp;
        if (!a.on || !slot.engine) continue;
        unsigned rate = arp_rate_ticks(a.rate);
        if (arp_ticks % rate) continue;
        int velocity = 0, key = a.next(velocity);
        if (key < 0) continue;
        velocity = a.velocity_of(a.from, velocity); // a held pad's pressure, as on the MPC (roadmap P1)
        if (song_recording && running && !count_left && !slot.silent) { // Seq Record: the repeat goes into the pattern as a note
            const auto& p = *patterns[t][slot.pattern];
            int64_t len = std::max<uint32_t>(p.length, 1), at = (int64_t(arp_origin) + arp_ticks - int64_t(slot.start)) % len;
            ArpWritten w;
            w.track = uint8_t(t); w.pattern = slot.pattern; w.key = uint16_t(key); w.cutoff = uint8_t(key >> 16); w.velocity = uint8_t(std::clamp(velocity, 1, 127));
            Speed sp = speed_of(speeds_now[t].load(std::memory_order_relaxed)); // its pattern plays scaled: written as it reads
            w.tick = uint32_t(uint64_t(at < 0 ? at + len : at) * sp.num / sp.den); w.length = uint16_t(std::max(1u, rate * a.gate * sp.num / sp.den / 100));
            if (!arp_written.push(w)) {}
        }
        auto length = std::max<int32_t>(64, int32_t(arp_tick_length() * rate * a.gate / 100.0));
        double late = arp_swing_ticks(a.rate, (arp_ticks % (4 * arp_ticks_per_quarter)) / rate, swing); // every second straight note, as the project swings
        if (late > 0) { slot.arp_late = {key, int16_t(velocity), a.from, int32_t(arp_tick_length() * late), length}; continue; }
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

void EngineHost::run_hits(unsigned t, Slot& slot, unsigned frames) {
    if (!slot.hits_waiting) return;
    for (auto& h : slot.hits) {
        if (h.key < 0) continue;
        if (h.samples > 0) { h.samples -= int32_t(frames); continue; } // its frame comes later in or after this run
        PendingHit hit = h;
        h.key = -1;
        --slot.hits_waiting;
        play_hit(t, slot, hit, frame_now + block_at);
    }
}

void EngineHost::play_hit(unsigned t, Slot& slot, const PendingHit& hit, uint64_t at) {
    if (slot.engine && hit.sound) { // a frozen track's sequenced hits sound in its print (freeze plan F2b)
        engine_note_on(slot, hit.key, hit.velocity, hit.accent ? accent_gain : 1.0f);
        schedule_off(slot, hit.key, hit.length);
    }
    if (hit.midi >= 0) midi_note(t, hit.midi, hit.velocity, at, hit.length);
    hits_played.fetch_add(1, std::memory_order_relaxed);
}

bool EngineHost::trig_plays(unsigned t, Slot& slot, const Event& e, uint32_t pass) {
    random = random * 1664525u + 1013904223u;
    bool chance = e.chance >= 100 || (random >> 8) % 100 < e.chance;
    if (e.condition == trig_always) return chance;
    // PRE: the last weighed note of its lane (a kit's key; else the track's); NEI: of the lane before (a kit's key
    // below, its first the track before's; else the track before). Nothing weighed there yet: counted as played.
    bool kit = slot.kind == EngineKind::Sampler;
    auto bit = [](const std::array<uint64_t, 4>& set, unsigned key) { return ((set[(key / 64) % 4] >> (key % 64)) & 1) != 0; };
    unsigned key = e.key;
    bool pre = kit ? bit(slot.keys_went, key) : slot.went;
    bool nei = true;
    const unsigned first = sampler_first_note;
    if (kit && key > first && bit(slot.keys_weighed, key - 1)) nei = bit(slot.keys_went, key - 1);
    else if ((!kit || key <= first) && t > 0 && slots[t - 1].weighed) nei = slots[t - 1].went;
    bool go = trig_met(e.condition, pass, fill, pre, nei) && chance;
    (go ? trig_kept : trig_dropped).fetch_add(1, std::memory_order_relaxed);
    return go;
}

unsigned EngineHost::next_hit() const {
    int32_t next = INT32_MAX;
    for (const auto& slot : slots) {
        if (!slot.hits_waiting) continue;
        for (const auto& h : slot.hits) if (h.key >= 0 && h.samples > 0) next = std::min(next, h.samples);
    }
    return unsigned(next);
}

void EngineHost::drop_hits() {
    for (auto& slot : slots) {
        for (auto& h : slot.hits) h.key = -1;
        slot.hits_waiting = 0;
    }
}

void EngineHost::arp_note(Slot& slot, int key, int velocity, int from, int32_t length) {
    engine_note_on(slot, key, velocity);
    float pressure = slot.arp.pressure_of(from);
    if (pressure > 0) engine_pressure(slot, key, pressure);
    slot.arp_played = key; slot.arp_from = from;
    schedule_off(slot, key, length);
    midi_note(unsigned(&slot - slots.data()), key & 0xff, velocity, frame_now, length); // a kit pad's note, its semitones off
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
    if (is_program(id) || (id >= PatternProgram0 && id < PatternProgram0 + 16)) { midi_program(t, at, false); return; }
    if (id == Bend) { // pitch bend: -64..63 around the centre
        MidiOutEvent e {MidiOutEvent::Kind::PitchBend, uint8_t(std::clamp<int>(base[Channel], 1, 16)), 0, 0, uint32_t(std::clamp((value + 64) * 128, 0, 16383)), at};
        e.dest = dest_of(base[Port]); e.track = uint8_t(t);
        emit(e);
        return;
    }
    if (!is_cc(id)) return;
    midi_slot(t, id - Cc1, value, at);
}

// Roadmap P4 (docs/MIDI_TRACKS.md M2): a slot's value as its address says, at most one message a slot every 2 ms (the
// latest waits for its time; DIN carries about a thousand a second and a turn must not starve the notes).
void EngineHost::midi_slot(unsigned t, int slot, int value, uint64_t at) {
    using namespace midi_track;
    auto& s = slots[t % track_count];
    if (slot < 0 || slot >= midi_track::slots) return;
    auto& thin = s.thin[size_t(slot)];
    uint64_t gap = uint64_t(dsp::sample_rate() * 0.002);
    if (thin.last && at < thin.last + gap) {
        if (thin.pending >= 0) cc_thinned.fetch_add(1, std::memory_order_relaxed);
        thin.pending = value;
        return;
    }
    thin.last = std::max<uint64_t>(at, 1);
    thin.pending = -1;
    int address = s.base[size_t(slot_address(slot))];
    uint8_t channel = uint8_t(std::clamp<int>(s.base[Channel], 1, 16));
    MidiOutEvent e {MidiOutEvent::Kind::Control, channel, 0, 0, 0, at};
    int v = std::clamp(value, 0, 127), wide = std::min(v * 129, 16383); // a knob's 0-127 over 14 bits, ends to ends
    switch (slot_kind(address)) {
    case SlotKind::Off: return;
    case SlotKind::Cc: e.a = uint8_t(slot_number(address)); e.b = uint8_t(v); break;
    case SlotKind::Cc14: e.kind = MidiOutEvent::Kind::Cc14; e.a = uint8_t(slot_number(address)); e.value = uint32_t(wide); break;
    case SlotKind::PitchBend: e.kind = MidiOutEvent::Kind::PitchBend; e.value = uint32_t(std::min(v * 128, 16383)); break; // 64: the centre
    case SlotKind::ChannelPressure: e.kind = MidiOutEvent::Kind::ChannelPressure; e.b = uint8_t(v); break;
    case SlotKind::Nrpn: e.kind = MidiOutEvent::Kind::Nrpn; e.value = uint32_t(slot_number(address)) << 14 | uint32_t(wide); break;
    }
    e.dest = dest_of(s.base[Port]); e.track = uint8_t(t % track_count);
    emit(e);
    cc_sent.fetch_add(1, std::memory_order_relaxed);
}

void EngineHost::midi_thinned(unsigned frames) {
    uint64_t gap = uint64_t(dsp::sample_rate() * 0.002), end = frame_now + frames;
    for (unsigned t = 0; t < track_count; ++t) {
        if (!is_midi(t)) continue;
        auto& s = slots[t];
        for (int k = 0; k < int(s.thin.size()); ++k) {
            auto& thin = s.thin[size_t(k)];
            if (thin.pending < 0 || thin.last + gap >= end) continue;
            int value = thin.pending;
            uint64_t at = std::max(thin.last + gap, frame_now);
            thin.last = 0; // its time has come: it goes
            midi_slot(t, k, value, at);
        }
    }
}

// Roadmap P4 (docs/MIDI_TRACKS.md M5): a MIDI track's input as its sound, at its LEVEL (MONITOR off: silent), into its
// engine before the block renders; from there on it is any track's (chain, sends, stems, bounce).
void EngineHost::midi_inputs(const int32_t* input, const float* usb, unsigned frames) {
    constexpr float scale = 1.0f / 2147483648.0f;
    for (auto& s : slots) {
        if (s.kind != EngineKind::Midi || s.midi.audio == midi_track::AudioNone) continue;
        float g = s.base[midi_track::AudioMonitor] ? level_gain(s.base[midi_track::AudioLevel]) : 0.0f;
        for (unsigned f = 0; f < frames && f < 128; ++f) {
            float l = 0, r = 0;
            if (s.midi.audio == midi_track::AudioUsb) { if (usb) { l = usb[2 * f]; r = usb[2 * f + 1]; } }
            else if (input) { l = input[2 * f] * scale; r = input[2 * f + 1] * scale; }
            switch (s.midi.audio) {
            case midi_track::AudioRearL: r = l; break;
            case midi_track::AudioRearR: l = r; break;
            case midi_track::AudioMic: l = r = (l + r) * 0.5f; break;
            default: break;
            }
            s.midi.in_l[f] = l * g; s.midi.in_r[f] = r * g;
        }
    }
}

// Roadmap P4, stems to the card: each track as it joins the mix (after its chain, level, pan and mute, eased as heard)
// and each bus's return, the same frames as the mixdown.
void EngineHost::take_stems(unsigned frames) {
    StemRing* ring = stem_ring.load(std::memory_order_acquire);
    unsigned from = 0, to = 0;
    if (!ring || !render_out.span(frames, from, to)) return;
    static thread_local float l[StemRing::stem_count][block], r[StemRing::stem_count][block];
    std::array<const float*, StemRing::stem_count> left {}, right {};
    uint32_t mask = ring->mask();
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& slot = slots[t];
        if (!((mask >> t) & 1) || !slot.engine) continue;
        for (unsigned f = 0; f < frames; ++f) { float a = slot.audible.at(f); l[t][f] = slot.left[f] * a; r[t][f] = slot.right[f] * a; }
        left[t] = l[t]; right[t] = r[t];
    }
    for (unsigned b = 0; b < bus_count; ++b) {
        unsigned s = track_count + b;
        if (!((mask >> s) & 1)) continue;
        for (unsigned f = 0; f < frames; ++f) { float g = returns_now[b].at(f); l[s][f] = bus_l[b][f] * g; r[s][f] = bus_r[b][f] * g; }
        left[s] = l[s]; right[s] = r[s];
    }
    ring->write(left.data(), right.data(), from, to);
}

int64_t EngineHost::track_delay(unsigned t) const {
    int ms = slots[t % track_count].base[midi_track::Delay];
    if (!ms || !is_midi(t)) return 0;
    int64_t frames = int64_t(std::lround(ms * dsp::sample_rate() / 1000.0));
    return std::max<int64_t>(frames, -int64_t(clock.latency_frames())); // decided as its block renders: no earlier than that
}

void EngineHost::midi_program(unsigned t, uint64_t at, bool force) {
    using namespace midi_track;
    t %= track_count;
    auto& slot = slots[t];
    const auto& base = slot.base;
    int own = patterns[t][slot.pattern % pattern_count]->program; // the pattern's own, else the track's
    int program = own ? own : base[Program];
    if (!program) return; // Off: the device keeps its own
    if (!force && program == slot.program_sent) return;
    slot.program_sent = program;
    uint8_t channel = uint8_t(std::clamp<int>(base[Channel], 1, 16));
    auto out = [&](MidiOutEvent::Kind kind, int a, int b) {
        MidiOutEvent e {kind, channel, uint8_t(a), uint8_t(b), 0, at};
        e.dest = dest_of(base[Port]); e.track = uint8_t(t);
        emit(e);
    };
    if (base[BankMsb]) out(MidiOutEvent::Kind::Control, 0, std::clamp<int>(base[BankMsb] - 1, 0, 127));
    if (base[BankLsb]) out(MidiOutEvent::Kind::Control, 32, std::clamp<int>(base[BankLsb] - 1, 0, 127));
    out(MidiOutEvent::Kind::Program, std::clamp<int>(program - 1, 0, 127), 0);
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

// Every slot on this target runs, held or not, so its history always holds what just played: a repeat's turns
// crossfade into what came before its slice, and the tape kinds read behind their start.
void EngineHost::run_live(uint8_t target, float* left, float* right, unsigned frames) {
    for (auto& k : kept) if (k.target == target) k.punch.process(left, right, frames); // pad-fx C: the pattern's, first
    for (auto& l : live) if (l.target == target) l.punch.process(left, right, frames);
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

// pad-fx C (round 2): punches kept with a track's pattern, each due from its tick as the pattern plays, in a kept
// slot of its own (a free one, else the one ending soonest), and let go after its length; heard at once unless a
// live punch sounds (it wins).
void EngineHost::play_pattern_punches(const Pattern& pattern, uint32_t at) {
    bool hand = hand_punch();
    for (const auto& e : pattern.punches) {
        if (e.tick != at) continue;
        Kept* slot = nullptr;
        for (auto& k : kept) if (!k.due) { slot = &k; break; }
        if (!slot) { slot = &kept[0]; for (auto& k : kept) if (k.until < slot->until) slot = &k; }
        if (e.target != slot->target) { slot->punch.clear(); slot->target = e.target; } // a new signal: the old history is not its own
        slot->kind = PunchKind(e.kind % punch_count);
        slot->depth = e.depth / 100.0f;
        slot->snap = e.snap & 3u;
        slot->tail = e.tail % 3u;
        slot->due = true;
        slot->until = tick_now + std::max<uint32_t>(1, e.length);
        if (!hand) sound_kept(*slot);
        else if (slot->on) { slot->punch.stop(); slot->on = false; }
    }
}

void EngineHost::sound_kept(Kept& k) {
    k.punch.start(k.kind, k.depth, bar_length(), bar_length() / (4u << k.snap), k.tail, params_of(k.kind));
    k.on = true;
}

bool EngineHost::hand_punch() const {
    for (const auto& l : live) if (l.punch.on()) return true;
    return false;
}

// One note of a track's pattern on its tick: `at` where it starts in a loop of `loop` ticks (the pattern's, or its
// kit lane's own), on pass `pass` of that loop (trig conditions).
void EngineHost::play_event(unsigned t, Slot& slot, const Pattern& pattern, size_t n, uint32_t at, uint32_t loop, uint32_t pass, bool sound,
                            Slot::Held& holding) {
    const auto& events = pattern.events;
    const auto& e = events[n];
    if (e.flags & step_mute) return; // muted on Roll: kept, not played
    if (e.key > 127 && slot.kind != EngineKind::Sampler) return; // past 127: kit pads only
    const int played = pad_note(e.key, e.pitch); // the key, and over it what a kit pad plays at
    if (skipped(slot, unsigned(played), at)) return;
    // Roadmap P3: a kit pad's note kept with 16 Levels' cutoff plays at it (the sampler reads it above the semitones).
    const int cutoff = slot.kind == EngineKind::Sampler && e.cutoff ? e.cutoff - 1 : -1;
    const int sounded = cutoff >= 0 ? pad_note(e.key, e.pitch, cutoff) : played;
    bool go = trig_plays(t, slot, e, pass);
    if (trig_weighed(e.condition, e.chance)) { // how it went, for PRE and NEI after it
        slot.weighed = true; slot.went = go;
        uint64_t bit = uint64_t(1) << (e.key % 64);
        auto& word = slot.keys_went[e.key / 64 % 4];
        slot.keys_weighed[e.key / 64 % 4] |= bit;
        word = go ? word | bit : word & ~bit;
    }
    if (!go) return;
    // Held for its length, a touch short; with Slide or Tie, until the next note starts (and past
    // it, so that note finds it held and takes it over: no new attack; a slide glides there).
    // A ratchet: its hits spread over its length, each held as long as a hit of that length would be;
    // Slide and Tie wait for a note of one hit.
    unsigned hits = e.hits();
    bool legato = hits == 1 && (e.flags & (step_slide | step_tie));
    double each = double(e.length) / hits;
    double ticks = each - std::min<double>(pattern.q(), each) * 0.1;
    if (legato) {
        size_t next = n + 1;
        while (next < events.size() && events[next].tick == at) ++next;
        uint32_t to = next < events.size() && events[next].tick < loop ? events[next].tick : events.front().tick + loop;
        ticks = double(to - at) + sixteenth_ticks;
    }
    auto samples = std::max<int32_t>(int32_t(tick_length() * ticks), 64);
    // Accent: louder, and harder where velocity moves the sound.
    bool accent = e.flags & step_accent;
    int velocity = accent ? std::min(127, e.velocity + 24) : e.velocity;
    auto from = slot.held; // the note held into this tick takes this one, the first note on it
    slot.held = {};
    if (sound) {
        float gain = accent ? accent_gain : 1.0f;
        if (from.key >= 0) {
            engine_slide(slot, from.key, played, velocity, from.glide, gain);
            if (from.key != played) cancel_off(slot, from.key);
        } else {
            engine_note_on(slot, sounded, velocity, gain);
        }
        schedule_off(slot, played, samples);
    }
    // MIDI: a tie onto the same key goes on; else the notes overlap, which a mono synth plays legato.
    if (from.midi != e.key || !midi_hold(t, e.key, samples, frame_now + block_at))
        midi_note(t, e.key, velocity, frame_now + block_at, samples);
    if (legato) holding = {int16_t(played), int16_t(e.key), (e.flags & step_slide) != 0};
    for (unsigned k = 1; k < hits; ++k) { // the ratchet's other hits, each on its frame (run_hits)
        if (slot.hits_waiting == slot.hits.size()) break;
        PendingHit hit;
        hit.key = pad_note(e.key, std::clamp(e.pitch + e.hit_pitch(k), -48, 48), cutoff);
        hit.midi = int16_t(e.key);
        hit.velocity = uint8_t(e.hit_velocity(velocity, k));
        hit.accent = accent;
        hit.sound = sound;
        hit.samples = std::max<int32_t>(1, int32_t(std::lround(k * each * tick_length())));
        hit.length = samples;
        for (auto& h : slot.hits)
            if (h.key < 0) { h = hit; ++slot.hits_waiting; break; }
    }
}

void EngineHost::tick() {
    // Count-In: a bar of the metronome before the song starts.
    if (count_left) {
        unsigned done = bar - count_left;
        if (done % beat == 0) {
            click_env = 1; click_phase = 0; click_wait = pdc.total; // as late as what it counts (roadmap P4)
            click_step = (done == 0 ? 1568.0f : 1046.0f) / dsp::sample_rate();
            click_fall = std::pow(0.9985f, dsp::model_rate / dsp::sample_rate());
            count_beat.store(done / beat + 1, std::memory_order_relaxed);
        }
        --count_left;
        count_rest.store(count_left, std::memory_order_relaxed);
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
        click_env = 1; click_phase = 0; click_wait = pdc.total; // as late as what it counts (roadmap P4)
        click_step = (tick_now % bar == 0 ? 1568.0f : 1046.0f) / dsp::sample_rate();
        click_fall = std::pow(0.9985f, dsp::model_rate / dsp::sample_rate());
    }
    // A pattern render ends after its ticks, on the frame.
    if (!bounce_song && render_out.running() && tick_now >= bounce_ticks) render_out.finish(block_at);
    if (tick_now % bar == 0) { freeze_at_bar(); bar_start(); } // freeze plan F2b: a frozen track switches on a bar line
    if (!running) return;
    if (song_mode && !song.tempo.empty()) follow_lane(); // roadmap P5: the tempo lane, this tick's tempo before its length
    if (!song_mode) // roadmap P5: follow actions due on this tick, before the tick's notes (a launch waiting for its bar first)
        for (unsigned t = 0; t < track_count; ++t) {
            auto& slot = slots[t];
            if (slot.follow_do && slot.queued == 255 && !slot.chained && !slot.silent && tick_now >= slot.follow_at) follow(t);
        }
    if (song_mode && tick_now % sixteenth_ticks == 0) play_song_punches();
    { // pad-fx C: a kept punch's length; a live punch wins while it sounds, the kept ones back for the rest of theirs after
        bool hand = hand_punch();
        for (auto& k : kept) {
            if (k.due && tick_now >= k.until) k.due = false;
            if (k.on && (!k.due || hand)) { k.punch.stop(); k.on = false; }
            else if (!k.on && k.due && !hand) sound_kept(k);
        }
    }
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        bool sound = slot.engine != nullptr, midi = out_channel[t] && out_mode[t]; // an empty track may still play MIDI out
        if ((!sound && !midi) || slot.silent) {
            if (slot.print_on || slot.print.playing()) { slot.print.release(dsp::sample_rate()); slot.print_on = false; } // a STOP, no clip
            continue;
        }
        const auto& pattern = *patterns[t][slot.pattern];
        uint32_t length = std::max<uint32_t>(pattern.length, 1);
        uint32_t elapsed = tick_now - slot.start;
        uint32_t at = elapsed % length;
        uint32_t pass = elapsed / length; // trig conditions count the pattern's passes from where it began
        if (slot.loop_ticks && slot.loop_from < length) { // an editor's loop: its bars over and over
            uint32_t span = std::min(slot.loop_ticks, length - slot.loop_from);
            at = slot.loop_from + elapsed % span; pass = elapsed / span;
        }
        Speed sp = speed_of(speeds_now[t].load(std::memory_order_relaxed)); // published as written (its pattern plays scaled)
        positions_now[t].store(int(uint64_t(at) * sp.num / sp.den), std::memory_order_relaxed);
        if (!pattern.punches.empty()) play_pattern_punches(pattern, at); // pad-fx C: punches kept with it
        passes_now[t].store(pass, std::memory_order_relaxed);
        if (at == 0 && int(t) == cue_track) { take.go(block_at); cue_track = -1; } // Rec length Seq: the take starts with the pattern
        if (sound && (at == 0 || !pattern.motion.empty() || slot.locks_on)) play_motion(t, slot, pattern, at);
        if (slot.freeze && sound) print_tick(t, slot, at, length); // freeze plan F2b
        // Its print plays the notes: they go out by MIDI alone (freeze plan F2d: a deep freeze's engine never hears them).
        const bool seq = sound && !slot.print_on && !(slot.freeze && slot.freeze->deep);
        const auto& events = pattern.events;
        Slot::Held holding; // this tick's own Slide or Tie, for the notes of a later one
        bool lanes = !pattern.lanes.empty();
        for (size_t n = pattern.lower(at); n < events.size() && events[n].tick == at; ++n)
            if (!lanes || !pattern.own_lane(events[n].key)) play_event(t, slot, pattern, n, at, length, pass, seq, holding);
        for (const auto& l : pattern.lanes) { // kit lanes of their own length (roadmap P1): each loops, and counts its passes, alone
            uint32_t loop = pattern.lane_length(l.key);
            if (loop >= length) continue;
            uint32_t lane_at = elapsed % loop;
            for (size_t n = pattern.lower(lane_at); n < events.size() && events[n].tick == lane_at; ++n)
                if (events[n].key == l.key) play_event(t, slot, pattern, n, lane_at, loop, elapsed / loop, seq, holding);
        }
        if (holding.key >= 0) slot.held = holding;
    }
    // Where this tick fell, for a hit heard then (place_at): after the positions, which the release publishes too.
    uint32_t head = tick_head.load(std::memory_order_relaxed);
    tick_ring[head % tick_ring_size].store(uint64_t(uint32_t(frame_now + block_at)) | uint64_t(tick_now) << 32, std::memory_order_relaxed);
    tick_frames.store(float(tick_samples(tick_now) / speed), std::memory_order_relaxed);
    tick_head.store(head + 1, std::memory_order_release);
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
    if (!flushed) { // the thread's first block: denormals off; kept to its core when pinned, and known (roadmap P2)
        flush_denormals();
        flushed = true;
        if (pin_audio >= 0) keep_to_core(pin_audio);
#if defined(__linux__)
        audio_tid.store(int(syscall(SYS_gettid)), std::memory_order_release);
#endif
    }
    blocks_seen.fetch_add(1, std::memory_order_relaxed);
    if (is_parked) { // a project load on the loader's thread (park()): silent and still until it lets go
        if (!park_over()) { play_parked(out, frames, stems); return; }
        is_parked = false; park_wanted = 0; // the loader's project plays from here, faded in from silence
        frame_now += parked_frames; parked_frames = 0;
    } else if (park_wanted && park_over()) {
        park_wanted = 0; // called off while fading out: back up, and the commands behind it go on
    }
    for (auto& q : limbo) if (q && retired_patterns.push(q)) q = nullptr; // retired copies the queue had no room for
    for (auto& q : freeze_limbo) if (q && done_freezes.push(q)) q = nullptr; // and prints' packs (freeze plan F2b)
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
    take_plugins();
    // Where this block sits against CLOCK_MONOTONIC, for MIDI out and in.
    clock.stamp(int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()), frame_now);
    frames = std::min(frames, block);
    // The commands due at the block's start; a timed note later in it splits the block at its frame, below.
    unsigned next_command = take_commands(0, frames);
    plan_compensation(); // roadmap P4: how late each path plays this block
    plan_keys();         // and which chains wait for the tracks keying them
    midi_thinned(frames); // MIDI tracks' slot values that waited their 2 ms
    midi_inputs(input, usb, frames); // and their inputs as their sound
    ++block_count;
    auto block_start = std::chrono::steady_clock::now();
    // The tracks' own work goes to the render workers too once it is worth waking them for, and never while one
    // plug-in plays in two places (it would be asked for two blocks at once).
    bool parallel = workers.workers() && parallel_allowed.load(std::memory_order_relaxed) && track_work_ns >= parallel_from_ns
                 && !plugin_in_two_places();
    // One dispatch a block (roadmap P2): the workers woken now wait awake while this block is sequenced, so their
    // wake-up is not added to the block's time; after the block's last run they sleep until the next.
    bool whole = whole_blocks.load(std::memory_order_relaxed);
    if (whole && parallel) workers.wake();
    guard(); // the overload guard, at the block's first frame (roadmap P2)
    track_ns.fill(0);
    if (reformatting) { // a new rate: silent until the Reload has built everything again at it
        std::fill(out, out + 2 * frames, 0);
        if (stems) std::fill(stems, stems + size_t(track_count) * block, 0.0f);
        for (auto* level : {&out_levels[0], &out_levels[1], &in_levels[0], &in_levels[1]}) level->store(0, std::memory_order_relaxed);
        frame_now += frames;
        if (park_wanted) { park_gain = Ease {0, 0, 0}; go_parked(); } // silent already: no fade to wait for
        return;
    }
    // The clock tempo-synced effects follow: quarters since Play (on from where it was while stopped).
    effect_clock_now = fx_clock; // this thread's effects hear this host's clock (a print host beside the live one)
    fx_clock->quarter = tick_length() * ticks_per_quarter;
    if (running) fx_clock->beats = (double(tick_now) - std::max(until_tick, 0.0) / tick_length()) / ticks_per_quarter;
    if (mods_on) run_modulators(frames); // roadmap P4: before the tracks' buffers are cleared (a follower hears the last block)
    // The arps' clock, at the block's start (within 3 ms of the beat).
    until_arp -= frames;
    while (until_arp <= 0) { arp_tick(); until_arp += arp_tick_length(); }
    bool solo = false;
    for (auto& strip : loop_strips) solo |= strip.solo && loops.on();
    for (auto& slot : slots) {
        solo |= slot.solo;
        std::fill(slot.buffer, slot.buffer + frames, 0.0f);
        std::fill(slot.left, slot.left + frames, 0.0f); std::fill(slot.right, slot.right + frames, 0.0f);
        slot.knob_pad_fed = false;
        for (unsigned b = 0; b < bus_count; ++b) // pad sends: only what the last block wrote needs clearing
            if ((slot.send_pad_fed >> b) & 1) { std::fill(slot.pad_sends->l[b], slot.pad_sends->l[b] + block, 0.0f); std::fill(slot.pad_sends->r[b], slot.pad_sends->r[b] + block, 0.0f); }
        slot.send_pad_fed = 0;
        slot.print_fed = false; // freeze plan F2b: this block's print, from the first run a voice sounds in
        if (knob_heard && slot.engine == &slot.sampler) { // Knob FX on kit pads: they play apart, into the slot's knob_pad_l / knob_pad_r
            std::fill(slot.knob_pad_l, slot.knob_pad_l + frames, 0.0f); std::fill(slot.knob_pad_r, slot.knob_pad_r + frames, 0.0f);
        }
    }

    // Sidechain on one kit pad: the sampler plays that pad apart, into key_pad_l / key_pad_r.
    int pad_keyed = -1; // the key track, when its key is one of its pads
    if (comp.keyed() && comp.key_pad() < sampler_pads) {
        auto& ks = slots[comp.key() % track_count];
        if (ks.engine == &ks.sampler) pad_keyed = int(comp.key() % track_count);
    }
    if (pad_keyed >= 0) { std::fill(key_pad_l, key_pad_l + frames, 0.0f); std::fill(key_pad_r, key_pad_r + frames, 0.0f); }

    // The Looper's time is the sequencer's at the block's first frame (run_loops() comes after the block is played).
    loop_running = running; loop_counting = count_left > 0;
    loop_tick = running ? double(tick_now) - until_tick / tick_length() : 0;
    // Render up to each step, play the step, carry on: steps land on the sample, and so do notes played by hand.
    // One dispatch a block (roadmap P2): this pass only sequences, each track's engine calls past the first frame
    // kept for its job below; the old way renders every track up to each step here.
    deferring = whole;
    keyed_pad = pad_keyed;
    cut_count = 0;
    unsigned done = 0;
    while (done < frames) {
        if (next_command <= done) next_command = take_commands(done, frames);
        unsigned n = std::min(frames, next_command) - done;
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
        // Ratchets' hits on their frames: the run ends where the next one is due.
        n = std::min(n, next_hit());
        block_at = done;
        for (unsigned t = 0; t < track_count; ++t) run_hits(t, slots[t], n);
        // Note-offs and late arp notes first, track by track (they may send MIDI out), then the engines, each
        // track's apart: on the render workers too when there is enough to share (render_pool.h).
        for (auto& slot : slots) {
            if (!slot.engine) continue;
            run_offs(slot, n);
            run_late(slot, n);
            if (slot.engine == &slot.sampler) {
                bool pad_key = pad_keyed >= 0 && &slot == &slots[unsigned(pad_keyed)];
                slot.sampler.set_key_pad(pad_key ? int(comp.key_pad()) : -1, pad_key ? key_pad_l + done : nullptr, pad_key ? key_pad_r + done : nullptr);
                slot.sampler.set_knob_out(knob_heard ? slot.knob_pad_l + done : nullptr, knob_heard ? slot.knob_pad_r + done : nullptr);
                for (unsigned b = 0; b < bus_count; ++b) slot.sampler.set_send_out(b, slot.pad_sends->l[b] + done, slot.pad_sends->r[b] + done);
            }
        }
        cuts[cut_count++] = uint16_t(done); // a run starts here (at most one a frame)
        unsigned active = 0; // the heaviest first, so the longest job never starts last
        if (!whole) for (unsigned k = 0; k < track_count; ++k) if (slots[order[k]].engine) sounding_tracks[active++] = order[k];
        auto engine_job = [&](unsigned i) {
            unsigned t = sounding_tracks[i];
            auto& slot = slots[t];
            if (slot.freeze && !slot.awake) return; // freeze plan F2b: its print plays instead
            auto t0 = std::chrono::steady_clock::now();
            if (slot.engine->stereo()) slot.engine->render_stereo(slot.left + done, slot.right + done, n);
            else slot.engine->render(slot.buffer + done, n);
            if (slot.engine == &slot.sampler) { slot.knob_pad_fed |= slot.sampler.knob_fed(); slot.send_pad_fed |= slot.sampler.sends_fed(); }
            track_ns[t] += uint32_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        };
        if (parallel) workers.run(active, engine_job, true); // the old way keeps the workers awake between its runs
        else for (unsigned i = 0; i < active; ++i) engine_job(i);
        // Freeze plan F2b: frozen tracks' prints, each where its pattern is (memory the pager has pinned; a copy).
        for (auto& slot : slots) {
            if (!slot.print.sounding()) continue;
            if (!slot.print_fed) {
                std::fill(slot.print_l, slot.print_l + frames, 0.0f); std::fill(slot.print_r, slot.print_r + frames, 0.0f);
                slot.print_fed = true;
            }
            // A kit's pad sends printed beside it go where its live pads' would (mixed at the track's level, pan, mute).
            float* to_l[bus_count];
            float* to_r[bus_count];
            for (unsigned b = 0; b < bus_count; ++b) { to_l[b] = slot.pad_sends->l[b] + done; to_r[b] = slot.pad_sends->r[b] + done; }
            slot.print.render(slot.print_l + done, slot.print_r + done, n, to_l, to_r);
            slot.send_pad_fed |= slot.print.sends_fed();
        }
        sync.run(frame_now + done, n, quarter_samples(), [&](const MidiOutEvent& e) { emit(e); });
        if (running && !held) {
            until_tick -= n * speed;                                     // MTC in trims the pace; 1 otherwise
            if (mtc_on) mtc_at += n * speed / clock.frames_a_second();
        }
        done += n;
    }
    deferring = false;
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
    if (pdc.on) { std::fill(knob_pad_sum_l, knob_pad_sum_l + frames, 0.0f); std::fill(knob_pad_sum_r, knob_pad_sum_r + frames, 0.0f); }
    if (pdc.on) for (unsigned b = 0; b < bus_count; ++b) { std::fill(pad_send_sum_l[b], pad_send_sum_l[b] + frames, 0.0f); std::fill(pad_send_sum_r[b], pad_send_sum_r[b] + frames, 0.0f); }
    bool knob_fed = false;
    // Sidechain: the key track keys the compressor and stays aside while the rest ducks under it.
    bool keyed = comp.keyed();
    unsigned key = comp.key() % track_count;
    if (keyed) {
        std::fill(key_l, key_l + frames, 0.0f); std::fill(key_r, key_r + frames, 0.0f);
        std::fill(aside_l, aside_l + frames, 0.0f); std::fill(aside_r, aside_r + frames, 0.0f);
    }
    // The Track input source: one track on its own, the engine alone (pre) or as it leaves its chain (post).
    bool tapping = source_now.load(std::memory_order_relaxed) == unsigned(InputSource::Track);
    unsigned tapped = source_track.load(std::memory_order_relaxed) % track_count;
    bool tap_post = source_post.load(std::memory_order_relaxed);
    if (tapping) { std::fill(tap_l, tap_l + frames, 0.0f); std::fill(tap_r, tap_r + frames, 0.0f); }
    // Each track on its own: level, pan, its chain, punches on it, and what it taps or keys (only one track does
    // each); on the render workers too. Then into the master, the buses and the rest, in track order, here.
    unsigned active = 0;
    for (unsigned k = 0; k < track_count; ++k) if (slots[order[k]].engine) sounding_tracks[active++] = order[k];
    // Level and pan ease across the block (Ease), aimed here before the workers take the tracks.
    for (unsigned i = 0; i < active; ++i) {
        auto& slot = slots[sounding_tracks[i]];
        float balance = slot.engine->stereo() ? 1.4142f : 1.0f; // a stereo engine's pan balances it, unity at the centre
        slot.mix_l.aim(slot.gain * slot.pan_left * balance, frames, snap_mix);
        slot.mix_r.aim(slot.gain * slot.pan_right * balance, frames, snap_mix);
        // Freeze plan F2b: a Post FX print was made at a level and pan (that pan law in it: model/print.h print_mix(),
        // freeze plan F2c): the level and pan now taken relative to those, after it. A side the print's pan left
        // (nearly) silent keeps its own sound as it is while the pan stays.
        float ref_gain = 1, ref_l = pan_centre, ref_r = pan_centre;
        if (const FreezePack* pack = slot.freeze) {
            ref_gain = std::max(level_gain(pack->level), 1e-4f);
            float angle = (std::clamp<int>(pack->pan, -50, 50) + 50) / 100.0f * dsp::pi / 2;
            ref_l = std::cos(angle); ref_r = std::sin(angle);
        }
        auto side = [](float now, float ref) { return std::fabs(now - ref) < 1e-6f ? 1.0f : now / std::max(ref, 0.05f); };
        slot.print_mix_l.aim(slot.gain / ref_gain * side(slot.pan_left, ref_l), frames, snap_mix);
        slot.print_mix_r.aim(slot.gain / ref_gain * side(slot.pan_right, ref_r), frames, snap_mix);
    }
    auto chain_of = [&](unsigned t) {
        auto& slot = slots[t];
        auto t0 = std::chrono::steady_clock::now();
        bool tap = tapping && t == tapped;
        // Freeze plan F2b: a frozen track's engine asleep leaves its buffers silent; its print joins before the
        // chain (Pre FX) or after it (Post FX, the chain then skipped while the engine sleeps).
        const bool frozen = slot.freeze != nullptr, asleep = frozen && !slot.awake;
        const bool post = slot.print_post;
        if (tap && !tap_post) { // before the track's level, pan and chain; a mono engine on both sides
            const float* l = slot.engine->stereo() ? slot.left : slot.buffer;
            const float* r = slot.engine->stereo() ? slot.right : slot.buffer;
            std::copy(l, l + frames, tap_l); std::copy(r, r + frames, tap_r);
        }
        if (asleep) {
            // left and right were cleared at the block's start
        } else if (slot.engine->stereo()) { // its own stereo picture; the track's pan balances it
            for (unsigned f = 0; f < frames; ++f) { slot.left[f] *= slot.mix_l.at(f); slot.right[f] *= slot.mix_r.at(f); }
        } else {
            for (unsigned f = 0; f < frames; ++f) {
                float x = slot.buffer[f];
                slot.left[f] = x * slot.mix_l.at(f); slot.right[f] = x * slot.mix_r.at(f);
            }
        }
        float awake_peak = 0; // what the engine (and at Post FX its chain) plays, for when it may sleep
        auto peak_of = [&] { for (unsigned f = 0; f < frames; ++f) awake_peak = std::max({awake_peak, std::fabs(slot.left[f]), std::fabs(slot.right[f])}); };
        if (frozen && !asleep && !post) peak_of();
        if (slot.print_fed && !post) // at the track's level and pan, as the engine would have been
            for (unsigned f = 0; f < frames; ++f) { slot.left[f] += slot.print_l[f] * slot.mix_l.at(f); slot.right[f] += slot.print_r[f] * slot.mix_r.at(f); }
        if (!(asleep && post)) run_chain(t, slot.left, slot.right, frames);
        if (frozen && !asleep && post) peak_of();
        if (slot.print_fed && post)
            for (unsigned f = 0; f < frames; ++f) { slot.left[f] += slot.print_l[f] * slot.print_mix_l.at(f); slot.right[f] += slot.print_r[f] * slot.print_mix_r.at(f); }
        if (frozen && !asleep) { // asleep after a second below the floor, nothing held by hand and no pattern played live
            bool keep = !slot.freeze->deep && (slot.hand.any() || slot.arp.count || (running && !slot.silent && !slot.print_on));
            slot.quiet = keep || awake_peak >= sleep_floor ? 0 : slot.quiet + frames;
            if (slot.quiet >= uint32_t(dsp::sample_rate())) slot.awake = false;
        }
        if (punch_target == t) punch.process(slot.left, slot.right, frames);
        run_live(uint8_t(t), slot.left, slot.right, frames);
        if (pdc.on) compensate(CompensationLines::track0 + t, slot.left, slot.right, frames); // meets the latest track (roadmap P4)
        if (tap && tap_post) { std::copy(slot.left, slot.left + frames, tap_l); std::copy(slot.right, slot.right + frames, tap_r); }
        bool keying = keyed && t == key && pad_keyed < 0; // the whole track keys
        if (keying) { std::copy(slot.left, slot.left + frames, key_l); std::copy(slot.right, slot.right + frames, key_r); } // muted too: a ghost key
        if (pad_keyed == int(t)) { // one kit pad keys: at the track's level and pan, aside if heard; the rest of the kit ducks
            for (unsigned f = 0; f < frames; ++f) { key_l[f] = key_pad_l[f] * slot.mix_l.at(f); key_r[f] = key_pad_r[f] * slot.mix_r.at(f); }
            if (pdc.on) compensate(CompensationLines::key_pad, key_l, key_r, frames); // it left before the chain (roadmap P4)
        }
        track_ns[t] += uint32_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    };
    // Roadmap P4: a track keyed by another track runs its chain after the rest (the keyed run, below).
    auto chain_job = [&](unsigned i) { if (!keyed_track[sounding_tracks[i]]) chain_of(sounding_tracks[i]); };
    // One dispatch a block: each track's engine over the whole block, its calls at their frames, then its chain.
    auto track_job = [&](unsigned i) {
        unsigned t = sounding_tracks[i];
        auto t0 = std::chrono::steady_clock::now();
        play_calls(t, slots[t], frames);
        track_ns[t] += uint32_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        chain_job(i);
    };
    // The Looper beside them, one more job of the same run (roadmap P2): first when it took longer than the tracks'
    // mean last block, else last. Then the buses' chains and Knob FX's, each a job of a second run.
    std::array<uint8_t, bus_count + 1> seconds {};
    unsigned second_jobs = 0;
    for (unsigned b = 0; b < bus_count; ++b) if (fx[bus_chain(b)][0].kind() != EffectKind::None) seconds[second_jobs++] = uint8_t(b);
    unsigned looper_at = loop_ns > track_work_ns / std::max(active, 1u) ? 0 : active;
    auto first_job = [&](unsigned i) {
        if (i == looper_at) {
            auto t0 = std::chrono::steady_clock::now();
            play_loops(frames);
            loop_ns = uint32_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            return;
        }
        unsigned k = i > looper_at ? i - 1 : i;
        if (whole) track_job(k);
        else chain_job(k);
    };
    // The workers stay awake for the second run when it has jobs to share (a Knob FX tail may join them).
    std::array<uint8_t, track_count> waiting {}; // roadmap P4: the keyed run, the tracks keyed by tracks
    unsigned waiting_count = 0;
    for (unsigned i = 0; i < active; ++i) if (keyed_track[sounding_tracks[i]]) waiting[waiting_count++] = sounding_tracks[i];
    if (parallel) workers.run(active + 1, first_job, waiting_count || (whole ? second_jobs + (knob_ringing || knob_heard) >= 2 : true));
    else for (unsigned i = 0; i < active + 1; ++i) first_job(i);
    if (waiting_count) {
        auto keyed_job = [&](unsigned i) { chain_of(waiting[i]); };
        if (parallel && waiting_count > 1) workers.run(waiting_count, keyed_job, whole ? second_jobs + (knob_ringing || knob_heard) >= 2 : true);
        else for (unsigned i = 0; i < waiting_count; ++i) keyed_job(i);
    }
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        float level = 0;
        float* stem = stems ? stems + size_t(t) * block : nullptr;
        if (stem) std::fill(stem, stem + frames, 0.0f);
        bool tap = tapping && t == tapped;
        if (slot.engine) {
            bool keying = keyed && t == key && pad_keyed < 0;
            // Monitored, the tapped track is heard through the input (gain, Rec Input FX) instead: once, as it records.
            bool through_input = tap && monitor_on.load(std::memory_order_relaxed);
            bool heard = !slot.mute && !(solo && !slot.solo) && !through_input;
            bool knobbed = knob_heard && ((knob_tracks >> t) & 1);
            // Mute and solo, Knob FX's pick and the sends ease across the block too, so none of them clicks.
            slot.audible.aim(heard ? 1.0f : 0.0f, frames, snap_mix);
            slot.knob_share.aim(knobbed ? 1.0f : 0.0f, frames, snap_mix);
            for (unsigned b = 0; b < bus_count; ++b) slot.sends_now[b].aim(slot.sends[b], frames, snap_mix);
            if (!slot.audible.silent()) {
                // Knob FX: a kit's pads switched on come out apart, after their own chain (not the track's), at the track's level and pan.
                if (slot.engine == &slot.sampler && slot.knob_pad_fed) {
                    float* to_l = pdc.on ? knob_pad_sum_l : knob_l; // compensating: held back apart, below (roadmap P4)
                    float* to_r = pdc.on ? knob_pad_sum_r : knob_r;
                    for (unsigned f = 0; f < frames; ++f) {
                        float a = slot.audible.at(f);
                        to_l[f] += slot.knob_pad_l[f] * slot.mix_l.at(f) * a; to_r[f] += slot.knob_pad_r[f] * slot.mix_r.at(f) * a;
                    }
                    knob_fed = true;
                }
                // Into Knob FX as much as it is picked, the rest to the master (or aside, the Sidechain's key).
                float* rest_l = keying ? aside_l : master_l;
                float* rest_r = keying ? aside_r : master_r;
                knob_fed |= !slot.knob_share.silent();
                float unpan = 1.0f / std::max(slot.pan_left + slot.pan_right, 1.0f); // a centred mono track's stem sums to itself
                for (unsigned f = 0; f < frames; ++f) {
                    float a = slot.audible.at(f), k = slot.knob_share.at(f);
                    float l = slot.left[f] * a, r = slot.right[f] * a;
                    knob_l[f] += l * k; knob_r[f] += r * k;
                    rest_l[f] += l - l * k; rest_r[f] += r - r * k;
                    level = std::max(level, std::max(std::fabs(l), std::fabs(r)));
                    if (stem) stem[f] = (l + r) * unpan;
                }
                for (unsigned b = 0; b < bus_count; ++b) { // into each bus as much as its send
                    const auto& send = slot.sends_now[b];
                    if (send.silent()) continue;
                    for (unsigned f = 0; f < frames; ++f) {
                        float g = slot.audible.at(f) * send.at(f);
                        bus_l[b][f] += slot.left[f] * g; bus_r[b][f] += slot.right[f] * g;
                    }
                }
                // A kit's own pad sends: each pad's after its own chain, at the track's level and pan, muted with it;
                // they left before the track's chain (compensating: held back apart, below).
                for (unsigned b = 0; b < bus_count; ++b) {
                    if (!((slot.send_pad_fed >> b) & 1)) continue;
                    float* to_l = pdc.on ? pad_send_sum_l[b] : bus_l[b];
                    float* to_r = pdc.on ? pad_send_sum_r[b] : bus_r[b];
                    for (unsigned f = 0; f < frames; ++f) {
                        float a = slot.audible.at(f);
                        to_l[f] += slot.pad_sends->l[b][f] * slot.mix_l.at(f) * a; to_r[f] += slot.pad_sends->r[b][f] * slot.mix_r.at(f) * a;
                    }
                }
                if (pad_keyed == int(t)) // the key pad joins after the ducking, untouched (no send: it left before the chain)
                    for (unsigned f = 0; f < frames; ++f) {
                        float a = slot.audible.at(f), l = key_l[f] * a, r = key_r[f] * a;
                        aside_l[f] += l; aside_r[f] += r;
                        level = std::max(level, std::max(std::fabs(l), std::fabs(r)));
                        if (stem) stem[f] += (l + r) * unpan;
                    }
            }
        }
        if (tapping && t == tapped) { // a print host: the kit's pad sends beside the print (render_print())
            tap_sends = slot.engine ? slot.send_pad_fed : 0;
            for (unsigned b = 0; b < bus_count; ++b) {
                if ((tap_sends >> b) & 1) { std::copy(slot.pad_sends->l[b], slot.pad_sends->l[b] + frames, tap_send_l[b]); std::copy(slot.pad_sends->r[b], slot.pad_sends->r[b] + frames, tap_send_r[b]); }
                else { std::fill(tap_send_l[b], tap_send_l[b] + frames, 0.0f); std::fill(tap_send_r[b], tap_send_r[b] + frames, 0.0f); }
            }
        }
        float shown = meters[t].load(std::memory_order_relaxed) * dsp::fall_per_block(0.97f, frames); // a short fall, as fast at any rate
        meters[t].store(std::max(level, shown), std::memory_order_relaxed);
    }
    mix_loops(frames, solo);
    if (pdc.on) { // roadmap P4: kits' pads into Knob FX as late as the latest track; the direct mix as late as the latest return
        compensate(CompensationLines::knob_pads, knob_pad_sum_l, knob_pad_sum_r, frames);
        for (unsigned f = 0; f < frames; ++f) { knob_l[f] += knob_pad_sum_l[f]; knob_r[f] += knob_pad_sum_r[f]; }
        for (unsigned b = 0; b < bus_count; ++b) { // kits' pad sends as late as the latest track, then into their buses
            compensate(CompensationLines::pad_sends0 + b, pad_send_sum_l[b], pad_send_sum_r[b], frames);
            for (unsigned f = 0; f < frames; ++f) { bus_l[b][f] += pad_send_sum_l[b][f]; bus_r[b][f] += pad_send_sum_r[b][f]; }
        }
        compensate(CompensationLines::direct, master_l, master_r, frames);
        if (keyed) { compensate(CompensationLines::aside, aside_l, aside_r, frames); compensate(CompensationLines::key, key_l, key_r, frames); }
    }
    // The second run: each bus through its chain, and Knob FX while it is fed or rings, apart; then the returns into
    // the master in the buses' order, and Knob FX's after them, here.
    if (knob_fed) { knob_ringing = true; knob_quiet = knob_rung = 0; }
    bool knob_runs = knob_ringing && knob.kind() != EffectKind::None;
    if (knob_runs) seconds[second_jobs++] = uint8_t(bus_count);
    auto second_job = [&](unsigned i) {
        unsigned b = seconds[i];
        if (b == bus_count) knob.process(knob_l, knob_r, frames);
        else run_chain(bus_chain(b), bus_l[b], bus_r[b], frames);
        if (pdc.on) compensate(b == bus_count ? unsigned(CompensationLines::knob) : CompensationLines::bus0 + b, // as late as the latest return
                               b == bus_count ? knob_l : bus_l[b], b == bus_count ? knob_r : bus_r[b], frames);
    };
    if (parallel && !plugin_on_two_buses()) workers.run(second_jobs, second_job);
    else for (unsigned i = 0; i < second_jobs; ++i) second_job(i);
    if (pdc.on) { // roadmap P4: a bus with no chain, and Knob FX not running, wait as long as the direct mix
        for (unsigned b = 0; b < bus_count; ++b)
            if (fx[bus_chain(b)][0].kind() == EffectKind::None) compensate(CompensationLines::bus0 + b, bus_l[b], bus_r[b], frames);
        if (!knob_runs && knob_fed) compensate(CompensationLines::knob, knob_l, knob_r, frames);
    }
    for (unsigned b = 0; b < bus_count; ++b) {
        auto& back = returns_now[b];
        back.aim(returns[b], frames, snap_mix);
        for (unsigned f = 0; f < frames; ++f) { float g = back.at(f); master_l[f] += bus_l[b][f] * g; master_r[f] += bus_r[b][f] * g; }
    }
    // Knob FX into the master. After its targets let go it rings on until its tail has been quiet a while, or at
    // most knob_ring_seconds (then cleared), and rests: nothing it held comes back when it is next fed.
    if (knob_runs) {
        float loudest = 0;
        for (unsigned f = 0; f < frames; ++f) {
            master_l[f] += knob_l[f]; master_r[f] += knob_r[f];
            loudest = std::max(loudest, std::max(std::fabs(knob_l[f]), std::fabs(knob_r[f])));
        }
        if (!knob_fed) {
            knob_rung += frames;
            knob_quiet = loudest < knob_floor ? knob_quiet + frames : 0;
            bool rung = knob_rung >= uint32_t(knob_ring_seconds * dsp::sample_rate());
            if (rung) knob.clear();
            if (rung || knob_quiet >= uint32_t(knob_quiet_seconds * dsp::sample_rate())) knob_ringing = false;
        }
    } else if (knob_fed) {
        for (unsigned f = 0; f < frames; ++f) { master_l[f] += knob_l[f]; master_r[f] += knob_r[f]; }
    }
    if (keyed) { // everything but the key ducks under it, then the key joins, untouched
        comp.duck(key_l, key_r, master_l, master_r, frames);
        for (unsigned f = 0; f < frames; ++f) { master_l[f] += aside_l[f]; master_r[f] += aside_r[f]; }
    }
    // The input source, after the gain: the take, the Recall ring and the meter hear it. Rec Input FX: Knob FX on it
    // while it is monitored or a take waits or runs, fading in and out as Bypass does. Resample takes the mix itself,
    // so it is taken after the master (below); any other source here, so Monitor can join the mix before the master.
    const bool resampling = source_now.load(std::memory_order_relaxed) == unsigned(InputSource::Resample);
    const bool monitored = monitor_on.load(std::memory_order_relaxed) && !resampling;
    auto take_input = [&] {
        select_input(input, usb, frames);
        auto taking = take.state();
        knob_in.set_bypass(!(knob_rec && knob_heard && (monitored || taking == TakeRecorder::State::Armed
                                                        || taking == TakeRecorder::State::Recording || taking == TakeRecorder::State::Cued)));
        knob_in.process(record_l, record_r, frames);
    };
    // Monitor: the source heard with the mix, eased in and out, through the master chain and compressor as a take
    // played back from a pad is. A hardware or USB source at a centred pad's pan law, the level a kit pad at unity
    // plays its take at; a tapped track as it is. Never into a render.
    monitor_now.aim(monitored && !render_out.running() && !render_out.cued() ? 1.0f : 0.0f, frames, snap_mix);
    if (!resampling) {
        take_input();
        if (!monitor_now.silent()) {
            const float pan = source_now.load(std::memory_order_relaxed) == unsigned(InputSource::Track) ? 1.0f : pan_centre;
            for (unsigned f = 0; f < frames; ++f) { float g = monitor_now.at(f) * pan; master_l[f] += record_l[f] * g; master_r[f] += record_r[f] * g; }
        }
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
    flex.process(master_l, master_r, frames); // held or not: its history always holds what just played (run_live())
    run_live(255, master_l, master_r, frames);
    comp.process(master_l, master_r, frames); // the master compressor (Shift + Pad 5), before main volume
    if (loops.on()) { // the mix as a loop records it (as Resample hears it)
        std::copy(master_l, master_l + frames, source_l[looper::source_mix]);
        std::copy(master_r, master_r + frames, source_r[looper::source_mix]);
    }
    unsigned sounding = 0;
    for (unsigned s = 0; s < live_slots; ++s) sounding |= unsigned(live[s].punch.on() || live[s].pending.waiting) << s;
    live_on.store(sounding, std::memory_order_relaxed);
    unsigned kept_sounding = 0; // pad-fx C
    for (unsigned s = 0; s < live_slots; ++s) kept_sounding |= unsigned(kept[s].on) << s;
    kept_on.store(kept_sounding, std::memory_order_relaxed);
    punch_on.store(punch.on() || sounding, std::memory_order_relaxed);
    if (resampling) take_input(); // the mix itself, before main volume
    take.process(record_l, record_r, frames);
    // The meters' fall over this block: meter_fall_db a second at the device's rate.
    float meter_fall = std::pow(10.0f, -meter_fall_db / 20 * float(frames) / float(dsp::sample_rate()));
    auto meter = [&](std::atomic<float>& held, float peak) {
        held.store(std::max(peak, held.load(std::memory_order_relaxed) * meter_fall), std::memory_order_relaxed);
    };
    {
        float in_l = 0, in_r = 0;
        for (unsigned f = 0; f < frames; ++f) { in_l = std::max(in_l, std::fabs(record_l[f])); in_r = std::max(in_r, std::fabs(record_r[f])); }
        meter(in_levels[0], in_l); meter(in_levels[1], in_r);
    }
    recall_ring.write(record_l, record_r, frames);
    keep_sources(input, usb, frames);
    take_stems(frames); // roadmap P4: the stems by the same bounds, before the bounce moves on
    render_out.process(master_l, master_r, frames);
    volume_now.aim(volume, frames, snap_mix); // main volume eased too: the knob's steps are up to dB apart
    park_gain.aim(park_wanted ? 0.0f : 1.0f, frames); // a Park fades out (and the next project in), never a jump
    bool park_fading = !(park_gain.flat() && park_gain.to == 1.0f);
    if (park_fading && stems)
        for (unsigned t = 0; t < track_count; ++t)
            for (unsigned f = 0; f < frames; ++f) stems[size_t(t) * block + f] *= park_gain.at(f);
    float peak = 0, out_l = 0, out_r = 0;
    for (unsigned f = 0; f < frames; ++f) {
        if (click_wait) --click_wait;
        else if (click_env > 0.0005f) { // a short blip on top of the mix
            float blip = std::sin(2 * dsp::pi * click_phase) * click_env * click_level * 0.5f;
            click_phase += click_step; click_phase -= std::floor(click_phase);
            click_env *= click_fall;
            master_l[f] += blip; master_r[f] += blip;
        }
        out_l = std::max(out_l, std::fabs(master_l[f])); out_r = std::max(out_r, std::fabs(master_r[f])); // the mix, for the meters
        float v = park_fading ? volume_now.at(f) * park_gain.at(f) : volume_now.at(f);
        float left = dsp::soft_clip(master_l[f] * v), right = dsp::soft_clip(master_r[f] * v);
        peak = std::max(peak, std::max(std::fabs(left), std::fabs(right)));
        out[2 * f] = int32_t(std::clamp(left, -0.999f, 0.999f) * 2147483647.0f);
        out[2 * f + 1] = int32_t(std::clamp(right, -0.999f, 0.999f) * 2147483647.0f);
    }
    output_peak.store(peak, std::memory_order_relaxed);
    meter(out_levels[0], out_l); meter(out_levels[1], out_r);
    if (!running) fx_clock->beats += frames / fx_clock->quarter; // tempo-synced effects run on while stopped
    snap_mix = false;
    publish_taps(frames);
    if (key_sources) keep_keys(frames); // roadmap P4: keys a block late hear this one
    frame_now += frames;
    // What the tracks took this block (all cores), heaviest first next time; and this block's time against its
    // budget, the peak of the last few, for status.
    uint64_t work = 0;
    for (unsigned t = 0; t < track_count; ++t) work += track_ns[t];
    track_work_ns = work;
    for (unsigned k = 1; k < track_count; ++k) // insertion sort: sixteen, mostly in order already
        for (unsigned j = k; j > 0 && track_ns[order[j]] > track_ns[order[j - 1]]; --j) std::swap(order[j], order[j - 1]);
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - block_start).count();
    float share = float(took * dsp::sample_rate() / std::max(frames, 1u));
    float tested = guard_test.load(std::memory_order_relaxed);
    last_share = tested >= 0 ? tested : share;
    const float load_fall = dsp::fall_per_block(0.995f, frames); // 0.995 a 128-frame block at 44.1 kHz: as long at any rate
    float shown = block_load.load(std::memory_order_relaxed) * load_fall;
    block_load.store(std::max(share, shown), std::memory_order_relaxed);
    for (unsigned t = 0; t < track_count; ++t) { // each track's own, the same way (roadmap P2)
        float own = float(track_ns[t] * 1e-9 * dsp::sample_rate() / std::max(frames, 1u));
        track_loads[t].store(std::max(own, track_loads[t].load(std::memory_order_relaxed) * load_fall), std::memory_order_relaxed);
    }
    if (parallel) parallel_blocks.fetch_add(1, std::memory_order_relaxed);
    { // freeze plan F2b: packs whose voices have faded handed back; what is frozen, playing and asleep, published
        uint32_t frozen_bits = 0, playing_bits = 0, sleeping_bits = 0, under = 0, waited = 0;
        for (unsigned t = 0; t < track_count; ++t) {
            auto& slot = slots[t];
            if (slot.freeze_leaving && !slot.print.leaving()) { retire_freeze(slot.freeze_leaving); slot.freeze_leaving = nullptr; }
            frozen_bits |= uint32_t(slot.freeze != nullptr) << t;
            playing_bits |= uint32_t(slot.print.playing()) << t;
            sleeping_bits |= uint32_t(slot.freeze && !slot.awake) << t;
            under += slot.print.underruns; waited += slot.print.waits;
        }
        frozen_now.store(frozen_bits, std::memory_order_relaxed);
        printing_now.store(playing_bits, std::memory_order_relaxed);
        sleeping_now.store(sleeping_bits, std::memory_order_relaxed);
        print_under.store(under, std::memory_order_relaxed);
        print_waited.store(waited, std::memory_order_relaxed);
    }
    // Last of all: a Park whose fade has gone silent hands the state to the loader, nothing touched after it.
    if (park_wanted && park_gain.silent()) go_parked();
}

void EngineHost::go_parked() {
    is_parked = true;
    parked_frames = 0;
    parks_done.fetch_add(1, std::memory_order_relaxed);
    parked_serial.store(park_wanted, std::memory_order_release); // the loader owns the state from here
}

void EngineHost::set_render_threads(unsigned count, uint64_t from_ns, bool pinned) {
    if (is_lean) count = 0, pinned = false; // a print host renders on its own thread alone
    parallel_from_ns = from_ns;
    std::array<int, 8> cores {};
    for (unsigned n = 0; n < cores.size(); ++n) cores[n] = int(2 + n);
    pin_audio = pinned ? 1 : -1;
    workers.start(count, render_priority, pinned ? cores.data() : nullptr);
}

// The overload guard (roadmap P2): a block that took nearly its whole time makes the next ones lighter before one runs
// late. From guard_on the busiest track of the last block (its engine and chain) gives up its oldest voice, and every
// engine's released voices end their tails in a few tens of ms; down to guard_off the tails are still hurried. Audio
// thread, at a block's first frame (the engines told at once: nothing of the block is rendered yet).
void EngineHost::guard() {
    if (!guard_allowed.load(std::memory_order_relaxed)) { guarding = false; return; }
    if (float tested = guard_test.load(std::memory_order_relaxed); tested >= 0) last_share = tested;
    bool over = last_share >= guard_on;
    guarding = over || (guarding && last_share >= guard_off);
    if (!guarding) return;
    int busiest = -1;
    for (unsigned k = 0; k < track_count && busiest < 0; ++k) if (slots[order[k]].engine && track_ns[order[k]]) busiest = order[k];
    for (unsigned t = 0; t < track_count; ++t)
        if (slots[t].engine && slots[t].engine->relieve(over && int(t) == busiest)) guard_stolen.fetch_add(1, std::memory_order_relaxed);
    guard_eased.fetch_add(1, std::memory_order_relaxed);
}

int EngineHost::busiest_track() const {
    int most = -1;
    float load = 0;
    for (unsigned t = 0; t < track_count; ++t)
        if (float l = track_load(t); l > load) { load = l; most = int(t); }
    return most;
}

bool EngineHost::rendering_thread(int tid) const {
    if (tid <= 0) return false;
    if (tid == audio_tid.load(std::memory_order_acquire)) return true;
    for (unsigned n = 0; n < workers.workers(); ++n) if (workers.worker_tid(n) == tid) return true;
    return false;
}

// One plug-in instance named by two of the places that run apart: two PLUGIN tracks, or a track's chain and
// another's (split_shared_plugins() parts them unless every entry is taken), or a loop's chain (the Looper's job runs
// beside the tracks); plugin_on_two_buses(): two buses' chains (they run apart after the tracks).
bool EngineHost::plugin_in_two_places() const {
    std::array<const PluginRunner*, track_count * (max_slots + 1) + loop_tracks * max_slots> seen {};
    unsigned count = 0;
    auto check = [&](const PluginRunner* r) {
        if (!r) return false;
        for (unsigned i = 0; i < count; ++i) if (seen[i] == r) return true;
        seen[count++] = r;
        return false;
    };
    auto chain = [&](unsigned c) {
        for (const auto& effect : fx[c]) {
            if (effect.kind() == EffectKind::None) break;
            if (effect.kind() == EffectKind::Plugin && check(runner_of(unsigned(effect.param(0))))) return true;
        }
        return false;
    };
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& slot = slots[t];
        if (slot.engine == &slot.plugin && check(slot.plugin.attached())) return true;
        if (chain(t)) return true;
    }
    for (unsigned n = 0; n < loop_tracks; ++n) if (chain(loop_chain(n))) return true;
    return false;
}

bool EngineHost::plugin_on_two_buses() const {
    std::array<const PluginRunner*, bus_count * max_slots> seen {};
    unsigned count = 0;
    for (unsigned b = 0; b < bus_count; ++b)
        for (const auto& effect : fx[bus_chain(b)]) {
            if (effect.kind() == EffectKind::None) break;
            if (effect.kind() != EffectKind::Plugin) continue;
            const PluginRunner* r = runner_of(unsigned(effect.param(0)));
            if (!r) continue;
            for (unsigned i = 0; i < count; ++i) if (seen[i] == r) return true;
            seen[count++] = r;
        }
    return false;
}

// The watched track's voices after this block, stamped with the frame they stand at (the block's end).
void EngineHost::publish_taps(unsigned frames) {
    int watched = tap_track.load(std::memory_order_relaxed);
    if (watched < 0 || watched >= int(track_count)) return;
    TrackTaps taps;
    taps.track = uint8_t(watched);
    taps.stamp = frame_now + frames;
    auto& slot = slots[size_t(watched)];
    if (slot.engine == &slot.sampler) slot.sampler.taps(taps);
    tap_board.publish(taps);
}

// The Looper's block: its clock from the sequencer's, its loops into L1-L4, each through its strip (a stereo
// picture its pan balances) and its chain. A job of the tracks' run (roadmap P2): the loops play what came in the
// block before, never this block's tracks, so they run beside them; mix_loops() sums them in after.
void EngineHost::play_loops(unsigned frames) {
    Looper::Clock time;
    time.rate = dsp::sample_rate();
    time.bar = bar; time.beat = beat;
    time.running = loop_running;
    time.counting = loop_counting;
    time.per_frame = speed / tick_length();
    time.tick = loop_tick;
    time.restart = loop_restart && loop_running && !loop_counting;
    if (time.restart) loop_restart = false;
    time.input_lag = unsigned(clock.latency_frames()) + pdc.total; // what is played along to is heard that much later (roadmap P4)
    Looper::Sources sources;
    sources.frames = source_frames;
    for (unsigned s = 0; s < looper::source_count; ++s) sources.at[s] = {source_l[s], source_r[s]};
    float out_l[loop_tracks][looper::block], out_r[loop_tracks][looper::block];
    loops.run(time, sources, frames, out_l, out_r);
    loops_heard = loops.on();
    if (!loops_heard) return;
    // A loop on a hardware input of its own hears it through its track, as Input Config's Monitor says (Auto: while
    // it waits for or records a take, or overdubs). What came in the block before: one block later than Monitor.
    auto monitor = Monitor(loop_monitor.load(std::memory_order_relaxed));
    for (unsigned n = 0; n < loop_tracks && monitor != Monitor::Off; ++n) {
        uint8_t s = loops.source(n);
        if (!looper::source_is_direct(s) || (monitor == Monitor::Auto && !loops.listening(n))) continue;
        for (unsigned f = 0; f < std::min(frames, source_frames); ++f) { out_l[n][f] += source_l[s][f]; out_r[n][f] += source_r[s][f]; }
    }
    for (unsigned n = 0; n < loop_tracks; ++n) {
        auto& strip = loop_strips[n];
        // Level and pan ease across the block, as a track's do (Ease).
        strip.mix_l.aim(strip.gain * strip.pan_left * 1.4142f, frames, snap_mix);
        strip.mix_r.aim(strip.gain * strip.pan_right * 1.4142f, frames, snap_mix);
        for (unsigned f = 0; f < frames; ++f) { strip.left[f] = out_l[n][f] * strip.mix_l.at(f); strip.right[f] = out_r[n][f] * strip.mix_r.at(f); }
        run_chain(loop_chain(n), strip.left, strip.right, frames);
        if (pdc.on) compensate(CompensationLines::loop0 + n, strip.left, strip.right, frames); // meets the latest track (roadmap P4)
    }
}

// L1-L4 after the tracks, in order: mute and solo, into the master and their send's bus.
void EngineHost::mix_loops(unsigned frames, bool solo) {
    if (!loops_heard) { for (auto& m : loop_meters) m.store(0, std::memory_order_relaxed); return; }
    for (unsigned n = 0; n < loop_tracks; ++n) {
        auto& strip = loop_strips[n];
        float level = 0;
        // Mute, solo and send ease across the block too (Ease).
        bool heard = !strip.mute && !(solo && !strip.solo);
        strip.audible.aim(heard ? 1.0f : 0.0f, frames, snap_mix);
        strip.send_now.aim(strip.send, frames, snap_mix);
        if (!strip.audible.silent()) {
            for (unsigned f = 0; f < frames; ++f) {
                float a = strip.audible.at(f), l = strip.left[f] * a, r = strip.right[f] * a;
                master_l[f] += l; master_r[f] += r;
                level = std::max(level, std::max(std::fabs(l), std::fabs(r)));
            }
            if (!strip.send_now.silent())
                for (unsigned f = 0; f < frames; ++f) {
                    float g = strip.audible.at(f) * strip.send_now.at(f);
                    bus_l[strip.bus][f] += strip.left[f] * g; bus_r[strip.bus][f] += strip.right[f] * g;
                }
        }
        float shown = loop_meters[n].load(std::memory_order_relaxed) * dsp::fall_per_block(0.97f, frames);
        loop_meters[n].store(std::max(level, shown), std::memory_order_relaxed);
    }
}

// What each source played this block, for the loops to write in the next (the mix was kept after the compressor).
// The hardware inputs a loop takes on its own: the ADC (the rear line inputs or the mic, as routed) and USB-C,
// each after Input Config's gain as the input is.
void EngineHost::keep_sources(const int32_t* input, const float* usb, unsigned frames) {
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
    constexpr float scale = 1.0f / 2147483648.0f;
    for (unsigned i = 0; i < looper::direct_inputs.size(); ++i) {
        uint8_t s = looper::source_direct(i);
        if (!((used >> s) & 1)) continue;
        auto in = looper::direct_inputs[i];
        for (unsigned f = 0; f < frames; ++f) {
            float l = 0, r = 0;
            if (from_usb(in)) { if (usb) { l = usb[2 * f]; r = usb[2 * f + 1]; } }
            else if (input) { l = input[2 * f] * scale; r = input[2 * f + 1] * scale; }
            if (in == InputSource::Mic) l = r = (l + r) * 0.5f;
            else if (in == InputSource::RearL || in == InputSource::UsbL) r = l;
            else if (in == InputSource::RearR || in == InputSource::UsbR) l = r;
            source_l[s][f] = l * record_gain; source_r[s][f] = r * record_gain;
        }
    }
    source_frames = frames;
}

} // namespace melodee
