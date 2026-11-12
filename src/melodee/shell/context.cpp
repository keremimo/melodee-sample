// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/context.h"
#include "melodee/model/pad_params.h"
#include "melodee/shell/input_choices.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/step_lane.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/engine/sampler.h"
#include <cmath>
#include "melodee/engine/take_recorder.h"
#include "melodee/model/chords.h"
#include "melodee/model/scales.h"
#include "melodee/model/project_file.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/record_place.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/track_copy.h"
#include "melodee/shell/name_suggest.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/page_stack.h"
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace melodee {
using Type = Command::Type;

void Context::send(const Command& command) {
    // Anything but playing changes the project (the metronome is Melodee's setting, not the project's).
    switch (command.type) {
    case Type::NoteOn: case Type::NoteOff: case Type::AllNotesOff: case Type::Wheel: case Type::Pressure:
    case Type::Audition: case Type::PadNote: case Type::Play: case Type::Stop: case Type::Reload:
    case Type::RecordArm: case Type::RecordNow: case Type::RecordStop: case Type::RecordAutoEnd: case Type::Locate: case Type::Recorded:
    case Type::Click: break;
    default: dirty = true;
    }
    // A note played by hand or by MIDI in plays at the frame it came in, a block late, not at the next block's start.
    switch (command.type) {
    case Type::NoteOn: case Type::NoteOff: case Type::PadNote: case Type::ArpKey: case Type::Pressure: case Type::Audition:
        host.send_at(command, hit_ns);
        break;
    default: host.send(command);
    }
}

void Context::save_project_to(const std::string& path) {
    if (!store || path.empty()) return;
    auto name = std::filesystem::path(path).stem().string();
    std::snprintf(project.name, sizeof(project.name), "%s", name.c_str());
    save_loop_files(*this, path); // the loops' lengths into the project, their audio beside it (docs/LOOPER.md)
    PluginStates states;
    if (plugins) states = plugins->states(); // each plug-in's state as it is now, beside the project
    store->save(path, melodee::save_project(project, patterns, &states));
    project_path = path;
    dirty = false;
}

void Context::save_project() { save_project_to(project_path.empty() && store ? store->free_path("") : project_path); }

void Context::new_project() {
    auto fresh = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    starting_project(*fresh, *bank);
    switch_to(*fresh, *bank, {});
}

void Context::switch_to(const Project& next, const PatternBank& bank, const std::string& path,
                        const std::array<std::vector<uint8_t>, max_plugins>* plugin_states) {
    send({Type::AllNotesOff, 255});
    if (plugins) plugins->set_states(plugin_states ? *plugin_states : PluginStates {});
    project = next;
    patterns = bank;
    auto keep = ui.pots;
    auto settings = ui.settings; // Melodee's own: they belong to no project
    auto pads_down = ui.held_pads; // the hand on the pads stays where it is
    ui = UiState {};
    ui.pots = keep;
    ui.held_pads = pads_down;
    ui.settings = settings;
    for (auto& pot : ui.pots) pot.bound = 0; // pick the new values up again
    edits.clear();                            // its edits were made to another project
    take_edit.reset(); take_track = -1;
    for (auto& h : held_notes) h.key = -1;
    ui.lead_pattern = project.tracks[0].pattern; // as the audio thread starts it
    free_retired();
    auto* pack = new ProjectPack(project, patterns);
    Command c {Type::Reload};
    c.pack = pack;
    if (!host.send(c)) delete pack;
    send_click(); // the metronome is Melodee's, and no take is running now
    project_path = path;
    if (store) store->opened(path); // a file of its own: the one the next start resumes
    dirty = false;
    samples_to_load = 0; // every sample slot again, a few each tick
    slots_waiting.clear();
    load_loop_files(*this, path); // its loops, from beside its file (docs/LOOPER.md)
}

bool Context::reformat() {
    send({Type::AllNotesOff, 255});
    free_retired();
    auto* pack = new ProjectPack(project, patterns);
    Command c {Type::Reload};
    c.pack = pack;
    c.value = 1; // the same project: its loops stay
    if (host.send(c)) return true;
    delete pack;
    return false;
}

void Context::open(PageId page) {
    // A PLUGIN track's sounds are the plug-ins on the card (Sample Select, or Sounds from anywhere).
    if (page == PageId::Sounds && track().engine == EngineKind::Plugin && plugins) { open_plugin_picker(UiState::PluginTarget::Instrument); return; }
    if (page == PageId::Trim) { // Trim lives on Stage now: a kit's B1 TRIM (the sampler pages' B1 and Chop come here)
        if (track().engine == EngineKind::Sampler) stage_go(ui, 0, 0);
        page = PageId::Stage;
    }
    if (page != ui.page) punch_release(); // a pad held over a page change would never let go
    ui.sheet_open = false;
    ui.move_open = false;
    ui.map_open = false;
    ui.loop_sheet = false;
    ui.fx_loop = -1; // FX Select on a loop track is set by its caller after this
    ui.picker_open = false;
    ui.song_export = false;
    if (ui.page == PageId::Sounds && page != PageId::Sounds) audition(-1);
    if (page != ui.page) {
        push_back_stack(ui, page);
        ui.page = page;
    }
    remember_editor(ui, page); // Seq on Patterns comes back to it (shell/page_stack.h)
    if (page == PageId::Roll) { // a kit's Roll opens on its pads' rows: the bank in view, the last pad's row
        const auto& t = track();
        if (t.engine == EngineKind::Sampler) {
            unsigned low = sampler_first_note + t.bank % sampler_banks * 16;
            if (ui.roll_key < low || ui.roll_key >= low + 16) ui.roll_key = uint8_t(sampler_first_note + t.last_pad % sampler_pads);
        } else if (t.engine == EngineKind::Kit909 && (ui.roll_key < 36 || ui.roll_key > 47)) {
            ui.roll_key = uint8_t(t.last_note >= 36 && t.last_note <= 47 ? t.last_note : 36);
        }
    }
    if (page == PageId::Sounds) {
        // The cursor starts on the loaded sound.
        ui.sounds_filter = 0;
        ui.sounds_cursor = track().preset;
    }
}

void Context::back() {
    if (ui.sheet_open || ui.map_open || ui.picker_open || ui.move_open) { // a sheet, map or picker first
        ui.sheet_open = ui.map_open = ui.picker_open = ui.move_open = false;
        return;
    }
    open(back_target(ui)); // open() finds it on the stack and drops it and what came after
}

void Context::select_track(unsigned t) {
    if (ui.page == PageId::Sounds) audition(-1);
    stage_tick(UINT32_MAX); // a sound still waiting to load lands on the track it was turned on
    ui.track = uint8_t(t % track_count);
    ui.held_step = -1;
    if (ui.page == PageId::Sounds) ui.sounds_cursor = track().preset;
}

void Context::note(unsigned t, int key, int velocity) {
    int top = project.tracks[t % track_count].engine == EngineKind::Sampler ? int(sampler_last_key) : 127; // kit pads in banks F-H
    if (key < 0 || (key & 0xff) > top) return;
    Command c {velocity ? Type::NoteOn : Type::NoteOff, uint8_t(t), uint8_t(from_midi ? 1 : 0), uint16_t(key), int16_t(velocity)};
    send(c);
}

// One key of a pad press, to the arp when it is on, else straight to the
// engine; every press also goes into the history Capture reads.
void Context::key(int k, int velocity, bool down) {
    auto& t = track();
    if (t.arp) {
        if (down || !t.arp_latch) send({Type::ArpKey, ui.track, 0, uint16_t(k), int16_t(down ? velocity : 0)});
    } else {
        note(ui.track, k, down ? velocity : 0);
    }
    if (!down) record_release(k);
    if (down) {
        if (!t.arp) record(k, velocity); // with the arp on the press is not heard: its notes are written as they play (take_arp_notes)
        history[history_at % history.size()] = {ui.track, uint8_t(k), uint8_t(velocity), now, 0};
        ++history_at;
    } else {
        for (unsigned n = 0; n < std::min<unsigned>(history_at, history.size()); ++n) {
            auto& h = history[(history_at - 1 - n) % history.size()];
            if (h.track == ui.track && h.key == uint8_t(k) && !h.length) { h.length = std::max<uint32_t>(1, now - h.at); break; }
        }
    }
}

// Let go of whatever this pad started, on the track it played (as record_key() plays its source track).
// A latched arp plays on, without the pad's pressure.
void Context::release_pad(unsigned pad) {
    pad %= 16;
    auto& keys = pad_keys[pad];
    if (std::none_of(keys.begin(), keys.end(), [](int32_t k) { return k >= 0; })) return;
    uint8_t selected = ui.track;
    ui.track = uint8_t(pad_track[pad] % track_count);
    auto& t = track();
    for (auto& k : keys)
        if (k >= 0) {
            if (t.arp && t.arp_latch && t.engine != EngineKind::Sampler) send({Type::Pressure, ui.track, 0, uint16_t(k), 0});
            key(k, 0, false); k = -1;
        }
    ui.track = selected;
}

void Context::play_pad(unsigned pad, uint8_t velocity, bool down) {
    pad %= 16;
    release_pad(pad);
    if (!down) return;
    pad_track[pad] = ui.track;
    auto& t = track();
    if (t.arp && t.arp_latch && !held_pads()) { // a fresh latch: the old keys go
        send({Type::Arp, ui.track, 0, 0, 0}); send({Type::Arp, ui.track, 0, 0, 1});
    }
    if (t.engine == EngineKind::Sampler) { // the pads are the kit's, in the bank in view
        if (t.levels) { // 16 Levels: one kit pad, soft to loud, its filter swept, or in pitch
            auto level = levels_of(project, t, pad);
            ui.levels_hit = int8_t(pad); ui.levels_value = int16_t(level.shown);
            pad_keys[pad][0] = pad_note(int(sampler_first_note + t.levels_pad), level.semitones); // what it plays, so Seq Record keeps it
            int v = level.velocity ? level.velocity : velocity;
            Command c {Type::PadNote, ui.track, uint8_t(level.semitones + 64), t.levels_pad, int16_t(v)};
            c.position = uint32_t(level.cutoff + 1);
            send(c);
            record(pad_keys[pad][0], v);
            return;
        }
        unsigned kit = t.bank % sampler_banks * 16 + pad;
        t.last_pad = uint8_t(kit);
        pad_keys[pad][0] = int16_t(sampler_first_note + kit);
        key(pad_keys[pad][0], velocity, true);
        return;
    }
    if (!t.used()) return;
    if (t.engine == EngineKind::Kit909) { // the pads are the drums
        if (pad >= 11) { notice("909: drums on pads 1-11"); return; } // dark, and says why
        pad_keys[pad][0] = int16_t(36 + pad);
        t.last_note = uint8_t(36 + pad);
        key(pad_keys[pad][0], velocity, true);
        return;
    }
    if (t.levels) { // 16 Levels on a synth: the last note on all sixteen, soft to loud or in pitch; Scale the key's notes
        auto level = levels_of(project, t, pad);
        ui.levels_hit = int8_t(pad); ui.levels_value = int16_t(level.shown);
        pad_keys[pad][0] = int16_t(level.key >= 0 ? level.key : std::clamp(int(t.last_note) + level.semitones, 0, 127));
        key(pad_keys[pad][0], level.velocity ? level.velocity : velocity, true);
        return;
    }
    int v = t.layout == PadLayout::Levels ? level_velocity(pad) : velocity;
    if (ui.page == PageId::Sounds) v = ui.audition_velocity;
    std::array<int, notes_per_step> keys {};
    unsigned count = 1;
    keys[0] = pad_note(project, t, pad).key;
    if (t.chord && t.layout != PadLayout::Levels) count = chord_notes(project, t, pad, keys);
    if (t.layout != PadLayout::Levels) t.last_note = uint8_t(std::clamp(keys[0], 0, 127));
    for (unsigned n = 0; n < count; ++n) {
        pad_keys[pad][n] = int16_t(std::clamp(keys[n], 0, 127));
        key(pad_keys[pad][n], v, true);
    }
    ui.chord_pad = int8_t(pad);
}

// Seq Record while playing: the note lands where it was heard (its pad's or MIDI
// message's stamp less the output latency, on the nearest tick: by the time the
// MIDI thread sees it the playhead has moved on), or with Rec Quantize on
// (Shift + Pad 10) on the nearest step of its grid (the pattern's Q, or one of
// its own: Settings::rec_grid). A note moved ahead of the playhead is skipped
// once by the audio thread, as it was heard live.
void Context::record(int key, int velocity) {
    if (!ui.recording || !host.playing() || host.count_in()) return;
    auto& p = pattern();
    int position = 0;
    uint32_t ticks = 0;
    if (!host.heard_place(ui.track, hit_ns, position, ticks)) { position = host.position_of(ui.track); ticks = host.ticks(); }
    int playhead = host.position_of(ui.track);
    if (playhead < 0) return;
    uint32_t at = wrap_tick(position, p.length);
    bool ahead = false;
    if (ui.settings.rec_quantize) {
        unsigned grid = division_ticks(grid_division(ui.settings.rec_grid, p.division));
        uint32_t snapped = quantise_tick(p, at, grid); // past the last step: the first, next time round
        ahead = still_ahead(at, snapped, playhead, p.length);
        at = snapped;
    }
    Event e;
    e.tick = at; e.length = uint16_t(p.q()); e.key = uint8_t(key); e.pitch = int8_t(key >> 8); e.velocity = uint8_t(std::clamp(velocity, 1, 127));
    if (!p.add(e)) { notice("Pattern full"); return; }
    sync_pattern(ui.track, track().pattern);
    if (ahead) { Command c {Type::Recorded, ui.track, track().pattern, uint16_t(key)}; c.position = at; send(c); }
    for (auto& h : held_notes)
        if (h.key < 0) { h = {key, ui.track, track().pattern, at, ticks}; break; }
}

// A recorded note let go: it is as long as it was held (from its press to its release, both as heard).
void Context::record_release(int key) {
    for (auto& h : held_notes) {
        if (h.key != key) continue;
        h.key = -1;
        auto& p = patterns.at[h.track % track_count][h.pattern % pattern_count];
        int position = 0;
        uint32_t ticks = 0;
        if (!host.heard_place(h.track, hit_ns, position, ticks)) ticks = host.ticks();
        uint32_t held = ticks - h.since;
        for (size_t n = p.lower(h.tick); n < p.events.size() && p.events[n].tick == h.tick; ++n)
            if (p.events[n].key == uint8_t(key) && p.events[n].pitch == int8_t(key >> 8)) {
                p.events[n].length = uint16_t(std::clamp<uint32_t>(held, 1, 65535));
                sync_pattern(h.track, h.pattern);
                break;
            }
        return;
    }
}

void Context::set_param(unsigned t, int id, int value) {
    auto& tr = project.tracks[t % track_count];
    if (id < 0 || unsigned(id) >= max_params) return;
    tr.params[size_t(id)] = int16_t(value);
    send({Type::Param, uint8_t(t), 0, uint16_t(id), int16_t(value)});
}

void Context::load_sound(unsigned t, EngineKind kind, unsigned sound) {
    auto& tr = project.tracks[t % track_count];
    bool engine_changed = tr.engine != kind;
    const auto& info = engine_info(kind);
    const UserSound* own = sound >= info.sound_count ? user_sound(kind, sound - info.sound_count) : nullptr;
    if (own) { // a sound of one's own: its values and routes, as saved
        tr.engine = kind; tr.preset = uint16_t(sound); tr.params = own->params; tr.mods = own->mods; tr.set_name(own->name);
        if (engine_changed) send({Type::Engine, uint8_t(t), 0, uint16_t(kind), 0});
        for (unsigned p = 0; p < info.param_count; ++p) send({Type::Param, uint8_t(t), 0, uint16_t(p), tr.params[p]});
        for (unsigned r = 0; r < mod_routes; ++r) { Command c {Type::Mod, uint8_t(t), 0, uint16_t(r)}; c.route = tr.mods[r]; send(c); }
    } else {
        melodee::load_sound(tr, kind, sound);
        if (engine_changed) send({Type::Engine, uint8_t(t), 0, uint16_t(kind), 0});
        send({Type::Sound, uint8_t(t), 0, uint16_t(tr.preset), 0});
    }
    if (t == ui.track) {
        // Keep it at the top of the recent list.
        unsigned w = 1;
        std::array<uint16_t, 8> recent {};
        recent[0] = tr.preset;
        for (unsigned r = 0; r < ui.recent_count && w < recent.size(); ++r) if (ui.recent[r] != tr.preset) recent[w++] = ui.recent[r];
        ui.recent = recent; ui.recent_count = uint8_t(w);
        ui.auditioning = -1;
    }
}

void Context::audition(int sound) {
    auto& t = track();
    if (sound == ui.auditioning) return;
    if (sound < 0 || sound == t.preset) {
        // Back to the sound the track holds, edits included.
        const auto& info = engine_info(t.engine);
        for (unsigned p = 0; p < info.param_count; ++p) send({Type::Param, ui.track, 0, uint16_t(p), t.params[p]});
        ui.auditioning = -1;
        return;
    }
    const auto& info = engine_info(t.engine);
    if (unsigned(sound) >= info.sound_count) { // one's own: its values
        const UserSound* own = user_sound(t.engine, unsigned(sound) - info.sound_count);
        if (!own) return;
        for (unsigned p = 0; p < info.param_count; ++p) send({Type::Param, ui.track, 0, uint16_t(p), own->params[p]});
    } else {
        send({Type::Sound, ui.track, 0, uint16_t(sound), 0});
    }
    ui.auditioning = int16_t(sound);
}

void Context::apply(const Binding& b, int value) {
    using Kind = Binding::Kind;
    value = std::clamp(value, b.min, b.max);
    touch_gesture(b); // one undo step per control's turns (shell/undo_scope.cpp)
    auto& t = track();
    switch (b.kind) {
    case Kind::Param:
        set_param(ui.track, b.id, value);
        if (t.engine != EngineKind::Midi || midi_track::is_cc(b.id)) record_motion(b.id, value); // of a MIDI track's own, only its CCs are lanes
        if (t.engine == EngineKind::Sampler && b.id == sampler::Follow && value) follow_tempos();
        break;
    case Kind::Level: t.mix.level = uint8_t(value); send({Type::Level, ui.track, 0, 0, int16_t(value)}); break;
    case Kind::Pan: t.mix.pan = int8_t(value); send({Type::Pan, ui.track, 0, 0, int16_t(value)}); break;
    case Kind::Send: { // a track's send into bus b.id (round 2: one into each bus)
        unsigned bus = unsigned(b.id) % bus_count;
        t.mix.sends[bus] = uint8_t(std::clamp(value, 0, 100));
        send({Type::Send, ui.track, 0, uint16_t(bus), int16_t(t.mix.sends[bus])});
        break;
    }
    case Kind::Root: project.root = uint8_t(value); break;
    case Kind::Scale: project.scale = uint8_t(value); break;
    case Kind::Layout: t.layout = PadLayout(value); break;
    case Kind::Transpose: t.transpose = int8_t(std::clamp(value, -12, 12)); break;
    case Kind::Engine: {
        std::array<EngineKind, size_t(EngineKind::Count)> engines {};
        unsigned count = available_engines(engines);
        if (unsigned(value) < count && engines[size_t(value)] != t.engine) {
            load_sound(ui.track, engines[size_t(value)], 0);
            if (t.engine == EngineKind::Plugin) open_plugin_picker(UiState::PluginTarget::Instrument); // which plug-in
        }
        break;
    }
    case Kind::Jump: ui.sounds_cursor = uint16_t(value); break;
    case Kind::Audition: ui.audition_velocity = uint8_t(value); break;
    case Kind::StepLength:
    case Kind::StepVelocity:
    case Kind::StepChance:
    case Kind::StepRatchet:
    case Kind::StepRamp:
    case Kind::StepPitchRamp:
    case Kind::StepCondition: { // the step in hand's notes, on a kit its lane's (the last pad's)
        auto& p = pattern();
        unsigned at = unsigned(std::max<int>(ui.step_cursor, 0)) % p.steps();
        uint32_t q = p.q();
        bool any = edit_lane(p, at, steps_lane(t), [&](Event& e) {
            switch (b.kind) {
            case Kind::StepLength: e.length = uint16_t(std::clamp<uint32_t>(uint32_t(std::max(value, 1)) * q, 1, 0xffff)); break;
            case Kind::StepVelocity: e.velocity = uint8_t(std::clamp(value, 1, 127)); break;
            case Kind::StepChance: e.chance = uint8_t(std::clamp(value, 0, 100)); break;
            case Kind::StepRatchet: e.ratchet = uint8_t(std::clamp<int>(value, 1, int(max_ratchet))); break;
            case Kind::StepPitchRamp: e.pitch_ramp = int8_t(std::clamp(value, -max_pitch_ramp, max_pitch_ramp)); break;
            case Kind::StepCondition: e.condition = uint8_t(std::clamp(value, 0, int(trig_count) - 1)); break;
            default: e.ramp = int8_t(std::clamp(value, -100, 100)); break;
            }
        });
        if (!any) break;
        sync_pattern(ui.track, t.pattern);
        ui.held_edited = true;
        break;
    }
    case Kind::ModSource:
    case Kind::ModDest:
    case Kind::ModAmount: {
        auto route = t.mods[size_t(b.id) % mod_routes];
        if (b.kind == Kind::ModSource) route.source = uint8_t(value);
        else if (b.kind == Kind::ModDest) { // past the destinations this engine has none of, the way it turned
            const auto& info = engine_info(t.engine);
            int way = value < route.destination ? -1 : 1, last = int(ModDest::Count) - 1;
            while (value >= 1 && value <= last && !mod_dest_label(info, ModDest(value))) value += way;
            if (value < 1 || value > last) break;
            route.destination = uint8_t(value);
        }
        else route.amount = int8_t(value);
        set_route(unsigned(b.id), route);
        break;
    }
    case Kind::PatternBars: set_length(uint32_t(value) * project.bar_ticks()); break;
    case Kind::PatternLength: set_length(pattern_lengths[unsigned(value) % pattern_length_count] * project.bar_ticks()); break;
    case Kind::TimeSignature: set_signature(unsigned(value)); break;
    case Kind::TcShift: ui.tc_shift = int16_t(value); break;
    case Kind::TcSwing: ui.tc_swing = uint8_t(value); break;
    case Kind::Division: set_division(Division(value)); break;
    case Kind::Swing:
        project.swing = uint8_t(value);
        send({Type::Swing, 0, 0, 0, int16_t(value)});
        break;
    case Kind::SongZoom: ui.song_zoom = uint8_t(value); break;
    case Kind::SongPattern:
    case Kind::SongLength:
        if (b.id >= 0) { // the clip under the cursor
            auto& clip = project.song.clips[size_t(b.id)];
            if (b.kind == Kind::SongPattern) clip.pattern = uint8_t(value);
            else { Clip changed = clip; changed.bars = uint16_t(value); project.song.erase(unsigned(b.id)); project.song.place(changed); }
            sync_song();
        } else if (b.kind == Kind::SongPattern) ui.song_pattern = uint8_t(value);
        else ui.song_length = uint8_t(value);
        break;
    case Kind::FxParam: {
        unsigned chain = unsigned(b.id) / 64, slot = (unsigned(b.id) / 8) % 8, q = unsigned(b.id) % 8;
        set_effect_param(chain, slot, q, value);
        if (chain == ui.track) record_motion(int(motion_fx_param(slot, q)), value); // the track's own effects land in Motion
        break;
    }
    case Kind::PluginParam: set_plugin_param(b.id, value); break;
    case Kind::SendBus: ui.sends_bus = uint8_t(unsigned(value) % bus_count); break; // the bus in view on Sends
    case Kind::BusReturn: set_return(unsigned(b.id), value); break;
    case Kind::PadPitch: pad().pitch = int8_t(value); send_pad(ui.track, t.last_pad); record_pad_motion(PadParam::Pitch, value); break;
    case Kind::PadLevel: pad().level = uint8_t(value); send_pad(ui.track, t.last_pad); record_pad_motion(PadParam::Level, value); break;
    case Kind::PadPan: pad().pan = int8_t(value); send_pad(ui.track, t.last_pad); record_pad_motion(PadParam::Pan, value); break;
    case Kind::PadStart: pad().start = uint32_t(uint64_t(value) * 44100 / 1000); send_pad(ui.track, t.last_pad); break;
    case Kind::PadEnd: {
        uint32_t frames = project.samples[pad().sample % sample_slots].frames, at = uint32_t(uint64_t(value) * 44100 / 1000);
        pad().end = at >= frames ? 0 : at; // the sample's end is kept as 0, so it follows a longer reload
        send_pad(ui.track, t.last_pad);
        break;
    }
    case Kind::TrimZoom: ui.trim_zoom = uint8_t(value); ui.trim_focus = uint8_t(b.id % 3); break;
    case Kind::PadLoopPoint: // turning it lets go of Loop Lock, as the point moves off the start
        pad().loop_start = uint32_t(uint64_t(value) * 44100 / 1000);
        if (pad().loop_lock) { pad().loop_lock = false; notice("Loop Lock off"); }
        send_pad(ui.track, t.last_pad);
        break;
    case Kind::PadLoop: pad().loop = value != 0; send_pad(ui.track, t.last_pad); break;
    case Kind::PadMono: pad().mono = value != 0; send_pad(ui.track, t.last_pad); break;
    case Kind::PadLink: pad().link = uint8_t(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadDecayFrom: pad().decay_from_start = value == 0; send_pad(ui.track, t.last_pad); break;
    case Kind::PadFilterType: pad().filter = PadFilter(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadWarp:
        pad().warp = warp_of_card(value);
        if (pad().warp == warp_seq && !pad().beats && !pad().own_tempo) follow_tempos(); // Seq needs the pad's own tempo
        send_pad(ui.track, t.last_pad);
        break;
    case Kind::PadBeats: pad().beats = uint8_t(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadOffset: case Kind::PadVelSense: case Kind::PadCutoff: case Kind::PadResonance:
    case Kind::PadFAttack: case Kind::PadFDecay: case Kind::PadFSustain: case Kind::PadFRelease: case Kind::PadFAmount: {
        // values Motion records as lanes of the pad
        PadParam which = b.kind == Kind::PadOffset ? PadParam::Offset : b.kind == Kind::PadVelSense ? PadParam::VelocitySense
                       : b.kind == Kind::PadCutoff ? PadParam::FilterCutoff : b.kind == Kind::PadResonance ? PadParam::FilterResonance
                       : b.kind == Kind::PadFAttack ? PadParam::FilterAttack : b.kind == Kind::PadFDecay ? PadParam::FilterDecay
                       : b.kind == Kind::PadFSustain ? PadParam::FilterSustain : b.kind == Kind::PadFRelease ? PadParam::FilterRelease
                       : PadParam::FilterAmount;
        set_pad_param(pad(), which, value);
        send_pad(ui.track, t.last_pad);
        record_pad_motion(which, value);
        break;
    }
    case Kind::ChopMode: ui.chop_mode = ChopMode(value); rechop(); break;
    case Kind::ChopAmount: ui.chop_amount[size_t(ui.chop_mode) % ui.chop_amount.size()] = value; rechop(); break;
    case Kind::ChopStart: case Kind::ChopEnd: { // editing a slice makes the chop Manual, as on the MPC
        uint32_t frames = ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].frames : 0, at = uint32_t(uint64_t(value) * 44100 / 1000);
        at = zero_cut(std::min(at, frames)); // onto a zero crossing near the knob's millisecond
        if (b.kind == Kind::ChopStart) ui.slices.move_start(unsigned(b.id), at, frames);
        else ui.slices.move_end(unsigned(b.id), at, frames);
        ui.chop_mode = ChopMode::Manual; ui.chop_armed = false;
        break;
    }
    case Kind::ChopZoom: ui.chop_zoom = uint8_t(value); ui.chop_focus = uint8_t(b.id % 2); break;
    case Kind::FileJump: ui.files_cursor = uint16_t(value); break;
    case Kind::PluginJump: ui.plugins_cursor = uint16_t(value); break;
    case Kind::ProjectJump: ui.project_cursor = uint16_t(value); break;
    case Kind::ProjectSort: ui.project_sort = uint8_t(value); break;
    case Kind::PadFine: pad().fine = int8_t(value); send_pad(ui.track, t.last_pad); record_pad_motion(PadParam::Fine, value); break;
    case Kind::PadPlayMode: pad().play = PadPlay(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadGroup: pad().group = uint8_t(value); send_pad(ui.track, t.last_pad); break; // 0 none, 1..16
    case Kind::PadReverse: pad().reverse = value != 0; send_pad(ui.track, t.last_pad); break;
    case Kind::PadAttack: pad().attack = uint8_t(value); send_pad(ui.track, t.last_pad); record_pad_motion(PadParam::Attack, value); break;
    case Kind::PadDecay: pad().decay = uint8_t(value); send_pad(ui.track, t.last_pad); record_pad_motion(PadParam::Decay, value); break;
    case Kind::PadRelease: pad().release = uint8_t(value); send_pad(ui.track, t.last_pad); record_pad_motion(PadParam::Release, value); break;
    case Kind::PadSustain: pad().sustain = uint8_t(value); send_pad(ui.track, t.last_pad); record_pad_motion(PadParam::Sustain, value); break;
    case Kind::ModWheel: // the fader's mod wheel, recorded as its own lane
        t.wheel = uint8_t(value);
        send({Type::Wheel, ui.track, 0, 0, int16_t(value * 1000 / 127)});
        record_motion(motion_wheel, value);
        break;
    case Kind::Tempo: set_tempo(value * 10); break;
    case Kind::NameSuggest: {
        std::array<const char*, max_suggestions> names {};
        unsigned count = name_suggestions(ui.name, names);
        if (unsigned(value) < count) { ui.name.suggestion = uint8_t(value); ui.name.set(names[value]); }
        break;
    }
    case Kind::NameChar: ui.name.set_char(unsigned(value)); break;
    case Kind::PunchDepth:
        project.punch_depth = uint8_t(value);
        for (unsigned n = 0; n < ui.engaged_count; ++n) ui.engaged[n].depth = uint8_t(value);
        if (ui.engaged_count) send({Type::PunchDepth, 254, 0, 0, int16_t(value)}); // every live slot
        break;
    case Kind::PunchSnap: project.punch_snap = uint8_t(value); break;
    case Kind::PunchTail: project.punch_tail = uint8_t(value); break;
    case Kind::PadPunchKind: pad().punch = uint8_t(value); break;
    case Kind::PadPunchRate: pad().punch_rate = uint8_t(value); break;
    case Kind::PadPunchDepth: pad().punch_depth = uint8_t(value); break;
    case Kind::RecGrid: // the REC QUANTIZE page's K1: the pattern's Q, or a grid of its own
        ui.settings.rec_grid = uint8_t(std::clamp(value, 0, int(Division::Count)));
        settings_save_at = std::max<uint32_t>(now, 1) + 1000;
        break;
    case Kind::ClickLevel:
        ui.settings.click_level = uint8_t(value);
        send_click();
        settings_save_at = std::max<uint32_t>(now, 1) + 1000; // pots move in many steps: the card gets the last
        break;
    case Kind::ArpMode: t.arp_mode = uint8_t(value); send({Type::Arp, ui.track, 0, 1, int16_t(value)}); break;
    case Kind::ArpRate: t.arp_rate = uint8_t(value); send({Type::Arp, ui.track, 0, 2, int16_t(value)}); break;
    case Kind::ArpOctaves: t.arp_octaves = uint8_t(value); send({Type::Arp, ui.track, 0, 3, int16_t(value)}); break;
    case Kind::ChordType: t.chord_type = uint8_t(value); break;
    case Kind::ChordVoicing: t.chord_voicing = uint8_t(value); break;
    case Kind::ChordInvert: t.chord_invert = uint8_t(value); break;
    case Kind::RecSource: {
        unsigned code = input_choices(project, ui.track, ui.settings.input).code_at(value);
        if (code >= input_track_code) set_input_source(InputSource::Track, code - input_track_code);
        else set_input_source(InputSource(code), ui.settings.input.track);
        break;
    }
    case Kind::RecTap: set_input_tap(value != 0); break;
    case Kind::KnobParam: knob_fx_param(unsigned(b.id), value); break;
    case Kind::PadFxParam: set_pad_fx_param(unsigned(b.id) / 4, unsigned(b.id) % 4, value); break;
    case Kind::FlexMode: set_flex(1, value); break;
    case Kind::FlexMix: set_flex(2, value); break;
    case Kind::CompAttack: set_compressor(2, value); break;
    case Kind::CompRelease: set_compressor(3, value); break;
    case Kind::CompAmount: set_compressor(4, value); break;
    case Kind::CompBoost: set_compressor(5, value); break;
    case Kind::RecThreshold: ui.settings.input.threshold = int8_t(value); input_changed(); break;
    case Kind::RecLength: ui.settings.input.length = uint8_t(value); input_changed(); break;
    case Kind::RecGain: ui.settings.input.gain = int8_t(value); input_changed(); break;
    case Kind::EventVelocity: set_event_velocity(*this, value); break;
    case Kind::EventLength: set_event_length(*this, value); break;
    case Kind::EventChance: set_event_chance(*this, value); break;
    case Kind::EventRatchet: set_event_ratchet(*this, value); break;
    case Kind::EventRamp: set_event_ramp(*this, value); break;
    case Kind::EventPitchRamp: set_event_pitch_ramp(*this, value); break;
    case Kind::EventCondition: set_event_condition(*this, value); break;
    case Kind::EntryLength: ui.entry_length = uint8_t(std::clamp(value, 1, 16)); break;     // step entry: what a pad writes
    case Kind::EntryVelocity: ui.entry_velocity = uint8_t(std::clamp(value, 0, 127)); break;
    case Kind::EntryChance: ui.entry_chance = uint8_t(std::clamp(value, 0, 100)); break;
    case Kind::EventNudge: set_event_nudge(*this, value); break;                             // Roll's Shift + K1
    case Kind::EuclidHits: case Kind::EuclidSteps: case Kind::EuclidRotate: set_euclid(b.kind, value); break;
    case Kind::TrackSpeed: set_speed(unsigned(b.id), value); break;
    case Kind::VaryAmount: ui.vary_amount = uint8_t(std::clamp(value, 0, 100)); break;
    case Kind::FeelStrength: case Kind::FeelSwing: case Kind::FeelHumanize: case Kind::FeelVelocity: case Kind::GrooveFrom:
    case Kind::GrooveAmount:
        set_feel(b.kind, value);
        break;
    case Kind::LaneLength: { // Steps on a kit: the last pad's lane loops over so many steps of Q (all of them: with the pattern)
        auto& p = pattern();
        uint32_t ticks = uint32_t(std::clamp(value, 1, int(p.steps()))) * p.q();
        if (p.lane_length(unsigned(b.id)) == std::min(ticks, p.length)) break;
        p.set_lane_length(unsigned(b.id), ticks);
        sync_pattern(ui.track, t.pattern);
        break;
    }
    case Kind::Lock: { // a step or a note held: the lane locked there (roadmap P1)
        uint32_t tick = 0, hold = 0;
        if (!lock_place(project, ui, pattern(), tick, hold)) break;
        write_lock(int(b.id), tick, uint16_t(std::min<uint32_t>(hold, 0xffff)), value);
        ui.held_edited = true; // Steps: letting the step go keeps it
        break;
    }
    case Kind::StageSound: preset_turn(value); break;
    case Kind::SoundList: ui.sounds_filter = uint8_t(value); ui.preset_due = 0; break;
    case Kind::ArpGate: t.arp_gate = uint8_t(value); send({Type::Arp, ui.track, 0, 4, int16_t(value)}); break;
    case Kind::ArpHold: // Hold, as Arp's B3: let go of it and the held keys go
        t.arp_latch = value != 0;
        if (!t.arp_latch) { send({Type::Arp, ui.track, 0, 0, 0}); if (t.arp) send({Type::Arp, ui.track, 0, 0, 1}); }
        break;
    case Kind::ChordOn: t.chord = value != 0; break; // Shift + K1 on Stage's CHORD
    case Kind::LevelsType: { // K1 on 16 Levels: Scale, Tune, Velocity (and Filter on a kit)
        std::array<LevelsKind, 4> types {};
        unsigned count = levels_types(t.engine == EngineKind::Sampler, types);
        t.levels_kind = uint8_t(types[unsigned(std::clamp(value, 0, int(count) - 1))]);
        ui.levels_hit = -1;
        break;
    }
    case Kind::LevelsLow: case Kind::LevelsHigh: { // K2 / K3: pad 1's end, pad 16's
        auto kind = LevelsKind(t.levels_kind % unsigned(LevelsKind::Count));
        if (unsigned(kind) >= 4) break;
        auto range = levels_range(kind);
        t.levels_ends[unsigned(kind)][b.kind == Kind::LevelsHigh ? 1 : 0] = int8_t(std::clamp(value, range.low, range.high));
        break;
    }
    case Kind::NewEngine: ui.new_engine = uint8_t(value); ui.new_sound = 0; break;
    case Kind::NewSound: ui.new_sound = uint8_t(value); break;
    case Kind::NewSource: ui.new_source = uint8_t(value); break;
    case Kind::NewPort: ui.new_port = uint8_t(value); break;
    case Kind::NewChannel: ui.new_channel = uint8_t(value); break;
    case Kind::TrackPick: select_track(unsigned(value)); break;
    case Kind::TrackPattern: launch(ui.track, unsigned(value)); break;
    // The Looper (docs/LOOPER.md, shell/looper_jobs.cpp).
    case Kind::LoopPick: ui.loop = uint8_t(value % loop_tracks); break;
    case Kind::LoopLength: looper_do(*this, looper::Do::Length, ui.loop, looper::lengths[size_t(value) % looper::lengths.size()]); break;
    case Kind::LoopFeedback: looper_do(*this, looper::Do::Feedback, ui.loop, value); break;
    case Kind::LoopQuantize: looper_do(*this, looper::Do::Quantize, ui.loop, value); break;
    case Kind::LoopThen: looper_do(*this, looper::Do::Then, ui.loop, value); break;
    case Kind::LoopLevel: case Kind::LoopPan: case Kind::LoopSend: case Kind::LoopBus: set_loop_mix(*this, ui.loop, b.kind, value); break;
    case Kind::None: break;
    }
}

void Context::set_route(unsigned r, const ModRoute& value) {
    if (r >= mod_routes) return;
    track().mods[r] = value;
    Command c {Type::Mod, ui.track, 0, uint16_t(r)};
    c.route = value;
    send(c);
}

void Context::pad_punch(unsigned pad, uint8_t velocity, bool down) {
    auto& t = track();
    unsigned kit = t.bank % sampler_banks * 16 + pad % 16;
    if (t.engine == EngineKind::Sampler && !t.levels && down && t.pads[kit].punch) { // the punch first, so the hit goes through it
        const auto& sound = t.pads[kit];
        send({Type::PunchOn, ui.track, uint8_t(sound.punch_rate | project.punch_tail << 4), uint16_t((sound.punch - 1) | (kit + 1) << 8),
              int16_t(sound.punch_depth)});
        ui.pad_punch_held = int8_t(kit); ui.pad_punch_pressure = 0;
    }
    play_pad(pad, velocity, down);
    if (!down && ui.pad_punch_held == int(kit)) { send({Type::PunchOff, ui.track, 0, uint16_t((kit + 1) << 8)}); ui.pad_punch_held = -1; }
}

void Context::change_setting(unsigned row, int way) {
    ui.settings.step(row, way);
    if (store) store->save_settings(ui.settings.text());
}

const UserSound* Context::user_sound(EngineKind engine, unsigned k) const {
    for (const auto& s : user_sounds) if (s.engine == engine && k-- == 0) return &s;
    return nullptr;
}
unsigned Context::user_sound_count(EngineKind engine) const {
    unsigned n = 0;
    for (const auto& s : user_sounds) n += s.engine == engine;
    return n;
}

void Context::open_name(NameTarget target, const std::string& initial, unsigned limit, bool capitals) {
    ui.name.start(target, initial.c_str(), limit, capitals);
    ui.sheet_open = false;
}

void Context::name_project(bool as_new) {
    if (!store) return;
    std::string suggestion = as_new ? std::string(project.name) : std::filesystem::path(store->free_path("")).stem().string();
    open_name(as_new ? NameTarget::ProjectSaveAs : NameTarget::ProjectSave, suggestion, sizeof(project.name) - 1, false);
}

void Context::rename_project(const std::string& path, const std::string& name) {
    name_path = path;
    open_name(NameTarget::ProjectRename, name, sizeof(project.name) - 1, false);
}

void Context::name_marker(unsigned bar) {
    int at = project.song.marker_at(bar);
    if (at < 0) return;
    name_bar = bar;
    open_name(NameTarget::Marker, project.song.markers[size_t(at)].name, sizeof(Marker::name) - 1, true);
}

void Context::save_sound() {
    auto& t = track();
    if (t.engine == EngineKind::None || t.engine == EngineKind::Sampler || !store) return;
    open_name(NameTarget::Sound, t.name, name_length - 1, true);
}

// Shift + Sample Select on a sampler track: the last pad's region as a WAV
// file under the card's Samples, named on the Name sheet (save_sample_as()).
// Jobs that cut or read one region of one file have nothing to do on an instrument.
bool Context::refuse_instrument(const char* job) {
    const auto& sound = pad();
    if (track().engine != EngineKind::Sampler || !sound.used() || !project.samples[sound.sample % sample_slots].instrument()) return false;
    notice(std::string(job) + ": not on an instrument");
    return true;
}

void Context::save_sample() {
    auto& t = track();
    const auto& sound = pad();
    if (t.engine != EngineKind::Sampler || !sound.used()) { notice("Save Sample: no sample"); return; }
    if (refuse_instrument("Save Sample")) return;
    const auto& ref = project.samples[sound.sample % sample_slots];
    open_name(NameTarget::Sample, ref.name, sizeof(project.name) - 1, false); // a file's name may run as long as a project's
}

// Shift + Sample Select on the Samples browser: the kit's bank in view as a
// bank file, named on the Name sheet (finish_name()).
void Context::save_bank() {
    auto& t = track();
    if (t.engine != EngineKind::Sampler || !library) return;
    char letter = char('A' + t.bank % sampler_banks);
    if (!bank_of(project, ui.track, t.bank, "").used()) { notice(std::string("Save Bank: bank ") + letter + " is empty"); return; }
    std::string name = t.name[0] ? std::string(t.name) + " " + letter : std::string("BANK ") + letter;
    open_name(NameTarget::Bank, name, name_length - 1, true);
}

void Context::finish_name() {
    auto& e = ui.name;
    std::string text(e.text, e.length);
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    NameTarget target = e.target;
    e.target = NameTarget::None;
    if (text.empty()) return; // an empty name changes nothing
    switch (target) {
    case NameTarget::ProjectSave: case NameTarget::ProjectSaveAs:
        if (store) { save_project_to(store->named_path(text)); store->list(project_folder); }
        break;
    case NameTarget::ProjectRename:
        if (name_path.empty() || name_path == project_path) std::snprintf(project.name, sizeof(project.name), "%s", text.c_str());
        if (!name_path.empty() && store) {
            store->rename(name_path, text);
            if (name_path == project_path) {
                auto p = std::filesystem::path(name_path);
                project_path = (p.parent_path() / (text + p.extension().string())).string();
            }
            store->list(project_folder);
        }
        break;
    case NameTarget::Marker: {
        int at = project.song.marker_at(name_bar);
        if (at >= 0) {
            auto& m = project.song.markers[size_t(at)];
            std::memset(m.name, 0, sizeof(m.name));
            std::strncpy(m.name, text.c_str(), sizeof(m.name) - 1);
            dirty = true;
        }
        break;
    }
    case NameTarget::Sound: {
        auto& t = track();
        if (!store || t.engine == EngineKind::None || t.engine == EngineKind::Sampler) break;
        t.set_name(text.c_str());
        store->save_sound(text, melodee::save_sound(sound_of(t, text.c_str())));
        break;
    }
    case NameTarget::Sample: save_sample_as(text); break;
    case NameTarget::Bank: {
        auto& t = track();
        if (!library || t.engine != EngineKind::Sampler) break;
        library->write_bank(text, melodee::save_bank(bank_of(project, ui.track, t.bank, text.c_str())));
        break;
    }
    case NameTarget::Mixdown: mixdown(text); break;
    case NameTarget::None: break;
    }
}

void Context::toggle_favourite() {
    auto& t = track();
    if (t.engine == EngineKind::Sampler || t.engine == EngineKind::None) return;
    unsigned sound = t.preset;
    if (ui.page == PageId::Sounds) {
        std::array<uint16_t, max_listed> list {};
        unsigned count = sound_list(t, ui, list);
        if (!count) return;
        sound = list[std::min<unsigned>(ui.sounds_cursor, count - 1)];
    }
    ui.settings.toggle_favourite(unsigned(t.engine), sound);
    if (store) store->save_settings(ui.settings.text());
}

void Context::pressure(unsigned pad, uint8_t value) {
    if (pad >= 16 || !ui.settings.pressure) return; // Settings: Pad pressure off
    if (punch_pressure(pad, value)) return;         // Pad FX: a held punch's depth
    if (ui.page == PageId::PadPunch && ui.pad_punch_held >= 0 && ui.pad_punch_held % 16 == int(pad)) { // pressure is depth
        ui.pad_punch_pressure = value;
        send({Type::PunchDepth, ui.track, 0, uint16_t((ui.pad_punch_held + 1) << 8), int16_t(value * 100 / 127)});
        return;
    }
    // Every note of a chord, on the track the pad played (a Record take's source track presses that track's notes).
    unsigned to = pad_track[pad] % track_count;
    for (auto k : pad_keys[pad]) if (k >= 0) send({Type::Pressure, uint8_t(to), 0, uint16_t(k & 0xff), int16_t(value)});
}

void Context::set_mute(unsigned t, bool on) {
    project.tracks[t % track_count].mix.mute = on;
    send({Type::Mute, uint8_t(t), 0, 0, int16_t(on)});
}
void Context::set_solo(unsigned t, bool on) {
    project.tracks[t % track_count].mix.solo = on;
    send({Type::Solo, uint8_t(t), 0, 0, int16_t(on)});
}

void Context::set_step(unsigned t, unsigned at, const Step& value) {
    auto& tr = project.tracks[t % track_count];
    auto& p = patterns.at[t % track_count][tr.pattern];
    if (at >= p.steps()) return;
    p.set_step(at, value);
    sync_pattern(t, tr.pattern);
}

void Context::free_retired() {
    Pattern* old = nullptr;
    while (host.take_retired_pattern(old)) delete old;
}

void Context::sync_pattern(unsigned t, unsigned p) {
    free_retired();
    Command c {Type::PatternData, uint8_t(t % track_count), uint8_t(p % pattern_count)};
    c.data = new Pattern(played_pattern(patterns.at[t % track_count][p % pattern_count], project.tracks[t % track_count].speed)); // as it plays: its feel, its track's speed (roadmap P1)
    if (!host.send(c)) delete c.data;
    dirty = true;
}

// Seq Record + a pot: a point of the playing pattern's Motion at the tick playing (roadmap P1: tick resolution).
void Context::record_motion(int lane, int value) {
    if (!ui.recording || !host.playing() || host.count_in() || lane < 0) return;
    auto& t = track();
    auto& p = pattern();
    MotionPoint m;
    m.tick = uint32_t(std::max(host.position_of(ui.track), 0)) % std::max<uint32_t>(p.length, 1);
    m.param = uint8_t(lane_param(lane)); m.pad = uint8_t(lane_pad(lane) % sampler_pads); m.value = int16_t(value);
    if (!p.write_motion(m)) return; // full: the move still sounds, it is just not kept
    sync_motion(ui.track, t.pattern);
}

void Context::write_lock(int lane, uint32_t tick, uint16_t hold, int value) {
    if (lane < 0) return;
    auto& t = track();
    auto& p = pattern();
    MotionPoint m;
    m.tick = tick % std::max<uint32_t>(p.length, 1); m.hold = std::max<uint16_t>(hold, 1);
    m.param = uint8_t(lane_param(lane)); m.pad = uint8_t(lane_pad(lane) % sampler_pads); m.value = int16_t(value);
    if (!p.write_motion(m)) { notice("Motion full"); return; }
    sync_motion(ui.track, t.pattern);
}

void Context::sync_motion(unsigned t, unsigned p) {
    int which = int((t % track_count) << 8 | (p % pattern_count));
    if (motion_waiting >= 0 && motion_waiting != which) sync_pattern(unsigned(motion_waiting) >> 8, unsigned(motion_waiting) & 0xff);
    motion_waiting = which;
    dirty = true;
    flush_motion();
}

// Seq Record with the arp on (Note Repeat): each note it played goes into the pattern it fell in, where it fell, as
// long as the arp held it (roadmap P1); one pattern copy for the audio thread for all that came.
void Context::take_arp_notes() {
    EngineHost::ArpWritten w;
    int synced = -1;
    while (host.take_arp_written(w)) {
        if (!ui.recording) continue;
        auto& p = patterns.at[w.track % track_count][w.pattern % pattern_count];
        Event e;
        e.tick = w.tick % std::max<uint32_t>(p.length, 1); e.length = std::max<uint16_t>(w.length, 1);
        e.key = uint8_t(w.key & 0xff); e.pitch = int8_t(w.key >> 8); e.velocity = uint8_t(std::clamp<int>(w.velocity, 1, 127));
        if (!p.add(e)) { notice("Pattern full"); continue; }
        int which = int(w.track % track_count) << 8 | int(w.pattern % pattern_count);
        if (synced >= 0 && synced != which) sync_pattern(unsigned(synced) >> 8, unsigned(synced) & 0xff);
        synced = which;
    }
    if (synced >= 0) sync_pattern(unsigned(synced) >> 8, unsigned(synced) & 0xff);
}

void Context::flush_motion() {
    if (motion_waiting < 0 || now - motion_synced < motion_sync_ms) return;
    sync_pattern(unsigned(motion_waiting) >> 8, unsigned(motion_waiting) & 0xff);
    motion_waiting = -1;
    motion_synced = now;
}

// A kit pad's own value turned while recording: a lane of that pad (the last hit).
void Context::record_pad_motion(PadParam which, int value) {
    if (track().engine != EngineKind::Sampler) return;
    record_motion(motion_lane(motion_pad_param(which), track().last_pad % sampler_pads), value);
}

void Context::clear_lane(int lane) {
    if (lane < 0) return;
    auto& t = track();
    std::string what = "Erase " + lane_name(t, lane) + " motion";
    edit_pattern(ui.track, t.pattern, what.c_str(), [&] { // one undo (the pattern holds its motion), or part of a take
        if (pattern().clear_lane(lane_param(lane), lane_pad(lane) % sampler_pads)) sync_pattern(ui.track, t.pattern);
    });
    notice(what);
}

void Context::launch(unsigned t, unsigned p) {
    auto& tr = project.tracks[t % track_count];
    if (!chaining) { tr.chain_count = 0; chain_queued[t % track_count] = false; } // a pattern launched by hand: the chain ends
    tr.pattern = uint8_t(p % pattern_count);
    ui.lead_pattern = tr.pattern; // BPM per pattern: the one launched last leads
    send({Type::Pattern, uint8_t(t), 0, 0, int16_t(tr.pattern)});
    if (ui.recording && host.song_playing()) record_launch(t % track_count, tr.pattern);
}

unsigned Context::song_sixteenth() const { return host.song_bar() * 16 + (host.ticks() % project.bar_ticks()) / sixteenth_ticks; }

void Context::record_launch(unsigned t, unsigned p) {
    unsigned bar = host.song_bar() + 1; // like a launch, on the next bar
    if (bar >= song_bars) return;
    project.song.place({uint8_t(t), uint8_t(p), uint16_t(bar), 1, false});
    recording_from[t] = uint16_t(bar);
    recording_tracks |= uint16_t(1u << t);
    seen_song_bar = host.song_bar();
    sync_song();
}

// Each bar the clips being written grow by one, so the song always holds the
// bar after the one playing; past 16 bars a new clip carries on.
void Context::song_tick() {
    bool on = ui.recording && host.song_playing();
    if (!on) {
        if (recording_tracks) finish_launches();
        if (recording_punch >= 0) end_punch();
        seen_song_bar = ~0u;
        return;
    }
    unsigned bar = host.song_bar();
    if (bar == seen_song_bar) return;
    if (seen_song_bar != ~0u && bar < seen_song_bar) { finish_launches(); seen_song_bar = bar; return; } // the loop came round
    seen_song_bar = bar;
    if (!recording_tracks) return;
    auto& song = project.song;
    for (unsigned t = 0; t < track_count; ++t) {
        if (!(recording_tracks & (1u << t))) continue;
        int index = -1;
        for (unsigned n = 0; n < song.count; ++n) if (song.clips[n].track == t && song.clips[n].bar == recording_from[t]) index = int(n);
        if (index < 0) { recording_tracks &= uint16_t(~(1u << t)); continue; }
        Clip clip = song.clips[size_t(index)];
        if (clip.end() != bar + 1 || bar + 1 >= song_bars) continue;
        if (clip.bars >= 16) { // a new clip carries on
            song.place({clip.track, clip.pattern, uint16_t(bar + 1), 1, false});
            recording_from[t] = uint16_t(bar + 1);
            continue;
        }
        song.cut(t, bar + 1, bar + 2); // what was written after it gives way
        for (unsigned n = 0; n < song.count; ++n)
            if (song.clips[n].track == t && song.clips[n].bar == recording_from[t]) { ++song.clips[n].bars; break; }
    }
    sync_song();
}

// Seq Record off or the song stopped: each clip ends with the bar that was playing.
void Context::finish_launches() {
    auto& song = project.song;
    unsigned bar = seen_song_bar == ~0u ? 0 : seen_song_bar;
    for (unsigned t = 0; t < track_count; ++t) {
        if (!(recording_tracks & (1u << t))) continue;
        for (unsigned n = 0; n < song.count; ++n) {
            auto& c = song.clips[n];
            if (c.track != t || c.bar != recording_from[t]) continue;
            unsigned end = std::max<unsigned>(bar + 1, c.bar + 1u);
            if (c.end() > end) c.bars = uint16_t(end - c.bar);
            break;
        }
    }
    recording_tracks = 0;
    sync_song();
}

void Context::begin_punch(unsigned kind, uint8_t target, uint8_t depth) {
    if (recording_punch >= 0) end_punch();
    if (!(ui.recording && host.song_playing())) return;
    unsigned at = song_sixteenth();
    punch_take = SongPunch {uint16_t(at / 16), uint8_t(at % 16), 1, uint8_t(kind), target, depth};
    recording_punch = int(at);
}

void Context::end_punch() {
    if (recording_punch < 0) return;
    unsigned now = host.song_playing() ? song_sixteenth() : unsigned(recording_punch) + 1;
    int length = int(now) - recording_punch;
    punch_take.length = uint8_t(std::clamp(length, 1, 255));
    recording_punch = -1;
    if (project.song.add_punch(punch_take)) sync_punches();
}

void Context::sync_punches() {
    const auto& song = project.song;
    for (unsigned n = 0; n < song.punch_count; ++n) {
        Command c {Type::SongPunchSet, 0, 0, uint16_t(n)};
        c.song_punch = song.punches[n];
        send(c);
    }
    send({Type::SongPunchCount, 0, 0, 0, int16_t(song.punch_count)});
}

void Context::clear_pattern(unsigned t, unsigned p) {
    auto& pattern = patterns.at[t % track_count][p % pattern_count];
    if (pattern.events.empty()) return;
    pattern.events.clear();
    sync_pattern(t, p);
}

void Context::copy_pattern(unsigned t, unsigned from, unsigned to, int from_track) {
    unsigned source_track = from_track < 0 ? t % track_count : unsigned(from_track) % track_count;
    if (from == to && source_track == t % track_count) return;
    patterns.at[t % track_count][to % pattern_count] = patterns.at[source_track][from % pattern_count];
    sync_pattern(t, to);
}

// Undo and redo. ----------------------------------------------------------

void Context::notice(const std::string& text) {
    std::snprintf(ui.notice, sizeof(ui.notice), "%s", text.c_str());
    ui.notice_at = now;
}

void Context::restore_pattern(unsigned t, unsigned p, const Pattern& value) {
    t %= track_count; p %= pattern_count;
    if (patterns.at[t][p] == value) return;
    patterns.at[t][p] = value;
    sync_pattern(t, p);
}

void Context::restore_pad(unsigned t, unsigned kit, const PadState& value) {
    t %= track_count; kit %= sampler_pads;
    set_pad_state(project.tracks[t], kit, value);
    send_pad(t, kit);
    sync_chain(pad_chain(t, kit));
}

void Context::delete_pad(unsigned kit) {
    kit %= sampler_pads;
    auto& t = track();
    if (!t.pads[kit].used()) { notice(pad_text("Empty", kit)); return; }
    Edit edit;
    edit.what = pad_text("Delete", kit); edit.track = ui.track; edit.pad = int16_t(kit);
    edit.pad_before = pad_state(t, kit);
    edit.pad_after = PadState {};
    edit.pad_after.chain.size = pad_slots;
    restore_pad(ui.track, kit, edit.pad_after);
    notice(edit.what);
    edits.push(std::move(edit));
}

void Context::erase_pad_notes(unsigned kit) {
    kit %= sampler_pads;
    const int key = int(sampler_first_note + kit);
    std::string what = pad_text("Erase", kit) + " notes";
    bool found = false;
    auto erase = [&] {
        auto& p = pattern();
        auto before = p.events.size();
        p.events.erase(std::remove_if(p.events.begin(), p.events.end(), [&](const Event& e) { return e.key == key; }), p.events.end());
        if (p.events.size() == before) return;
        sync_pattern(ui.track, track().pattern);
        found = true;
    };
    edit_pattern(ui.track, track().pattern, what.c_str(), erase); // inside a take, part of it
    notice(found ? what : pad_text("No notes on", kit));
}

void Context::copy_pad(unsigned from, unsigned to) {
    from %= sampler_pads; to %= sampler_pads;
    if (from == to) return;
    auto& t = track();
    Edit edit;
    char a[4], b[4]; pad_label(from, a); pad_label(to, b);
    edit.what = std::string("Copy ") + a + " to " + b; edit.track = ui.track; edit.pad = int16_t(to);
    edit.pad_before = pad_state(t, to);
    edit.pad_after = pad_state(t, from);
    restore_pad(ui.track, to, edit.pad_after);
    notice(edit.what);
    edits.push(std::move(edit));
}

Edit Context::begin_edit(const char* what, unsigned t, int p, bool song) {
    close_gesture(); // kept before this edit, as it came first
    Edit edit;
    edit.what = what; edit.track = uint8_t(t % track_count);
    if (p >= 0) { edit.pattern = int16_t(unsigned(p) % pattern_count); edit.pattern_before = std::make_unique<Pattern>(patterns.at[edit.track][edit.pattern]); }
    if (song) edit.song_before = std::make_unique<Song>(project.song);
    return edit;
}

// Keeps what changed since begin_edit(); an edit that changed nothing is dropped.
void Context::commit_edit(Edit&& edit) {
    bool changed = false;
    if (edit.pattern_before) {
        const auto& now = patterns.at[edit.track][unsigned(edit.pattern)];
        bool differs = now != *edit.pattern_before;
        if (differs) edit.pattern_after = std::make_unique<Pattern>(now);
        else { edit.pattern_before.reset(); edit.pattern = -1; }
        changed |= differs;
    }
    // Byte compares: a stray difference only keeps a part that did not change.
    if (edit.song_before) {
        if (std::memcmp(edit.song_before.get(), &project.song, sizeof(Song)) != 0) { edit.song_after = std::make_unique<Song>(project.song); changed = true; }
        else edit.song_before.reset();
    }
    if (edit.motion_before) {
        auto now_motion = track_motion(edit.motion_before->track);
        if (!(now_motion == *edit.motion_before)) { edit.motion_after = std::make_unique<TrackMotion>(std::move(now_motion)); changed = true; }
        else edit.motion_before.reset();
    }
    if (changed) edits.push(std::move(edit));
}

void Context::restore_song(const Song& value) {
    project.song = value;
    sync_song();
    sync_punches();
}

TrackMotion Context::track_motion(unsigned t) const {
    TrackMotion m;
    m.track = uint8_t(t % track_count);
    for (unsigned p = 0; p < pattern_count; ++p) m.lanes[p] = patterns.at[m.track][p].motion;
    return m;
}

void Context::restore_motion(const TrackMotion& value) {
    for (unsigned p = 0; p < pattern_count; ++p) {
        auto& pattern = patterns.at[value.track % track_count][p];
        if (pattern.motion == value.lanes[p]) continue;
        pattern.motion = value.lanes[p];
        sync_pattern(value.track, p);
    }
}

void Context::apply_edit(const Edit& e, bool after) {
    if (e.pad >= 0) restore_pad(e.track, unsigned(e.pad), after ? e.pad_after : e.pad_before);
    if (after) for (const auto& [kit, value] : e.pads_after) restore_pad(e.track, kit, value);
    else for (auto p = e.pads_before.rbegin(); p != e.pads_before.rend(); ++p) restore_pad(e.track, p->first, p->second);
    const auto& pattern = after ? e.pattern_after : e.pattern_before;
    if (e.pattern >= 0 && pattern) restore_pattern(e.track, unsigned(e.pattern), *pattern);
    const auto& song = after ? e.song_after : e.song_before;
    if (song) restore_song(*song);
    const auto& motion = after ? e.motion_after : e.motion_before;
    if (motion) restore_motion(*motion);
    const auto& whole = after ? e.track_after : e.track_before;
    if (whole) restore_track(*this, e.track, *whole); // last: its pack carries the motion as restored
    if (e.column >= 0)
        for (const auto& [t, value] : after ? e.column_after : e.column_before) restore_pattern(t, unsigned(e.column), value);
    const auto& chain = after ? e.chain_after : e.chain_before;
    if (e.chain >= 0 && chain) restore_chain(unsigned(e.chain), *chain);
    if (const auto& m = after ? e.mix_after : e.mix_before) restore_mix(*m);
    if (const auto& m = after ? e.sound_after : e.sound_before) restore_sound(e.track, *m);
    if (const auto& m = after ? e.master_after : e.master_before) restore_master(*m);
    if (const auto& m = after ? e.chop_after : e.chop_before) restore_chop(*m);
    dirty = true;
    take_track = -1;
}

void Context::undo() {
    close_gesture();                        // a pot's turns just made are the step undone
    close_hold(hold.pad);                   // so is a held pad's note
    if (ui.recording) set_recording(false); // a take in progress ends, and is the edit undone
    Edit* e = edits.undo();
    if (!e) { notice("Nothing to undo"); return; }
    apply_edit(*e, false);
    notice("Undo " + e->what);
}

void Context::redo() {
    close_gesture();
    close_hold(hold.pad);
    if (ui.recording) return; // a take in progress would be lost under it
    Edit* e = edits.redo();
    if (!e) { notice("Nothing to redo"); return; }
    apply_edit(*e, true);
    notice("Redo " + e->what);
}

void Context::set_length(uint32_t ticks) {
    auto& p = pattern();
    ticks = std::clamp<uint32_t>(ticks, 1, project.max_length());
    if (p.length == ticks) return;
    p.length = ticks;
    sync_pattern(ui.track, track().pattern);
}

void Context::set_division(Division division) {
    if (pattern().division == division) return;
    pattern().division = division;
    sync_pattern(ui.track, track().pattern);
}

// Every pattern shares the time signature; one of whole bars keeps its bar count.
void Context::set_signature(unsigned index) {
    index %= time_signature_count;
    if (index == project.signature) return;
    unsigned before = project.bar_ticks();
    project.signature = uint8_t(index);
    unsigned after = project.bar_ticks();
    for (unsigned t = 0; t < track_count; ++t)
        for (unsigned p = 0; p < pattern_count; ++p) {
            auto& pattern = patterns.at[t][p];
            if (pattern.length % before) continue;
            pattern.length = std::min<uint32_t>(pattern.length / before * after, project.max_length());
            sync_pattern(t, p);
        }
    send({Type::Signature, 0, 0, uint16_t(project.beat_ticks()), int16_t(after)});
    auto sig = project.time_signature();
    notice("Time signature " + std::to_string(sig.beats) + "/" + std::to_string(sig.unit));
}

void Context::set_seq_tempo(bool on) {
    project.seq_tempo = on;
    send({Type::SeqTempo, 0, 0, ui.lead_pattern, int16_t(on)});
    notice(on ? "BPM per pattern" : "BPM global");
}

// The wheel on Patterns: the playhead by a beat, or with Shift by Q. Stopped,
// it sets where Shift + Play carries on from.
void Context::locate(int way) {
    const auto& p = pattern();
    unsigned step = ui.shift ? p.q() : project.beat_ticks();
    uint32_t from = host.playing() ? host.ticks() : ui.locate;
    int64_t at = int64_t(from / step * step) + int64_t(way) * step;
    ui.locate = uint32_t(std::max<int64_t>(at, 0)) % std::max<uint32_t>(p.length, 1);
    if (host.playing() && !host.song_playing()) { Command c {Type::Locate}; c.position = ui.locate; send(c); }
}

void Context::send_pad(unsigned t, unsigned n) {
    Command c {Type::Pad, uint8_t(t), 0, uint16_t(n % sampler_pads)};
    c.pad = project.tracks[t % track_count].pads[n % sampler_pads];
    send(c);
}

int Context::sample_slot(const std::string& path, const std::string& name) {
    int free = -1;
    for (unsigned n = 0; n < sample_slots; ++n) {
        if (project.samples[n].used() && path == project.samples[n].path) return int(n); // already in the project
        if (free < 0 && !project.samples[n].used()) free = int(n);
    }
    if (free < 0 || !library) return -1;
    auto& ref = project.samples[size_t(free)];
    std::strncpy(ref.path, path.c_str(), sizeof(ref.path) - 1);
    std::strncpy(ref.name, name.c_str(), sizeof(ref.name) - 1);
    ref.frames = 0; // known once loaded
    if (!library->load(unsigned(free), path)) slots_waiting.push_back(uint8_t(free)); // a kit of many: the rest as the queue drains
    return free;
}

// Chop's slices of the region it holds, by its type. Manual starts as one
// slice, the whole region, armed: pad 1 plays it and the pads tapped
// meanwhile cut it (chop_tap()).
void Context::rechop() {
    if (!library || ui.chop_sample >= sample_slots) { ui.slices = {}; return; }
    auto loaded = library->slot(ui.chop_sample);
    ui.slices = chop(ui.chop_mode, ui.chop_amount[size_t(ui.chop_mode) % ui.chop_amount.size()], ui.chop_from, ui.chop_to, loaded.frames,
                     loaded.hits, loaded.hit_count, project.tempo);
    snap_slices();
    ui.chop_slice = uint8_t(std::min<unsigned>(ui.chop_slice, ui.slices.count ? ui.slices.count - 1 : 0));
    ui.chop_armed = ui.chop_mode == ChopMode::Manual;
    ui.chop_take_at = 0;
}

void Context::follow_tempos() {
    auto& t = track();
    for (unsigned n = 0; n < sampler_pads; ++n) {
        auto& pad = t.pads[n];
        uint32_t frames = pad.used() ? project.samples[pad.sample % sample_slots].frames : 0;
        if (!frames) continue;
        uint32_t end = pad.end ? pad.end : frames, length = end > pad.start ? end - pad.start : 0;
        if (length < 4410) continue; // a hit, not a loop: it keeps its speed
        // The number of bars that puts it nearest the project's tempo, between 70 and 180 BPM.
        double best = 0, target = project.tempo / 10.0;
        for (double bars : {0.25, 0.5, 1.0, 2.0, 4.0, 8.0}) {
            double bpm = bars * 4 * 60 * 44100.0 / length;
            if (bpm >= 70 && bpm <= 180 && (best == 0 || std::fabs(bpm - target) < std::fabs(best - target))) best = bpm;
        }
        pad.own_tempo = uint16_t(best * 10);
        send_pad(ui.track, n);
    }
}

bool Context::host_recording() const { return host.recorder().state() != TakeRecorder::State::Idle; }

void Context::chop_to_pads() {
    auto& t = track();
    uint32_t frames = ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].frames : 0;
    if (!frames) return;
    unsigned pad = 0;
    for (unsigned n = 0; n < ui.slices.count; ++n) {
        while (pad < sampler_pads && t.pads[pad].used()) ++pad; // from the first free pad on
        if (pad >= sampler_pads) break;
        PadSound sound;
        sound.sample = ui.chop_sample; sound.start = ui.slices.start[n]; sound.end = ui.slices.end(n, frames);
        t.pads[pad] = sound;
        send_pad(ui.track, pad);
    }
}

// A track chain changed shape: motion on its effects named slots that now
// hold something else, so it goes.
void Context::clear_fx_motion(unsigned n) {
    if (n >= track_count) return;
    for (unsigned p = 0; p < pattern_count; ++p) {
        auto& motion = patterns.at[n][p].motion;
        auto before = motion.size();
        motion.erase(std::remove_if(motion.begin(), motion.end(), [](const MotionPoint& m) { // pad lanes stay
            return m.param >= motion_fx_base && m.param < motion_pad_base; }), motion.end());
        if (motion.size() != before) sync_pattern(n, p);
    }
}

void Context::set_effect(unsigned n, unsigned slot, EffectKind kind) {
    auto& chain = project.chain(n);
    if (slot >= chain.size) return;
    clear_fx_motion(n);
    auto& s = chain.slots[slot];
    if (kind == EffectKind::None) { // remove: the rest close up
        for (unsigned k = slot + 1; k < chain.size; ++k) chain.slots[k - 1] = chain.slots[k];
        chain.slots[chain.size - 1] = EffectSlot {};
        sync_chain(n);
        return;
    }
    s.kind = uint8_t(kind); s.bypass = false;
    const auto& info = effect_info(kind);
    for (unsigned q = 0; q < effect_params; ++q) s.params[q] = info.params[q].label ? info.params[q].initial : 0;
    send_fx(Type::FxKind, n, slot, 0, int16_t(kind)); // the host takes the same initial values
    send_fx(Type::FxBypass, n, slot, 0, 0);
}

void Context::set_effect_param(unsigned n, unsigned slot, unsigned param, int value) {
    auto& chain = project.chain(n);
    if (slot >= chain.size || param >= effect_params) return;
    chain.slots[slot].params[param] = int16_t(value);
    send_fx(Type::FxParam, n, slot, param, int16_t(value));
}

void Context::toggle_bypass(unsigned n, unsigned slot) {
    auto& s = project.chain(n).slots[slot % max_slots];
    if (slot >= project.chain(n).size) return;
    s.bypass = !s.bypass;
    send_fx(Type::FxBypass, n, slot, 0, int16_t(s.bypass));
}

void Context::move_effect(unsigned n, unsigned slot, int way) {
    auto& chain = project.chain(n);
    int to = int(slot) + way;
    if (to < 0 || unsigned(to) >= chain.count() || slot >= chain.count()) return;
    clear_fx_motion(n);
    std::swap(chain.slots[slot], chain.slots[size_t(to)]);
    sync_chain(n);
}

void Context::sync_chain(unsigned n) {
    const auto& chain = project.chain(n);
    for (unsigned k = 0; k < chain.size; ++k) {
        const auto& s = chain.slots[k];
        send_fx(Type::FxKind, n, k, 0, int16_t(s.kind));
        for (unsigned q = 0; q < effect_params; ++q) send_fx(Type::FxParam, n, k, q, s.params[q]);
        send_fx(Type::FxBypass, n, k, 0, int16_t(s.bypass));
    }
}

// A chain's command: a track, bus or master chain by number; a pad's chain
// as PadFx*, with the track, the pad and the slot and parameter together.
void Context::send_fx(Command::Type type, unsigned n, unsigned slot, unsigned param, int16_t value) {
    if (n < pad_chain_base) { send({type, uint8_t(n), uint8_t(param), uint16_t(slot), value}); return; }
    n -= pad_chain_base;
    Type pad = type == Type::FxKind ? Type::PadFxKind : type == Type::FxParam ? Type::PadFxParam : Type::PadFxBypass;
    send({pad, uint8_t(n / sampler_pads), uint8_t(n % sampler_pads), uint16_t(slot | param << 8), value});
}

void Context::set_return(unsigned bus, int level) {
    project.buses[bus % bus_count].level = uint8_t(std::clamp(level, 0, 100));
    send({Type::Return, 0, 0, uint16_t(bus % bus_count), int16_t(level)});
}

void Context::sync_song() {
    const auto& song = project.song;
    for (unsigned n = 0; n < song.count; ++n) {
        Command c {Type::SongClip, 0, 0, uint16_t(n)};
        c.clip = song.clips[n];
        send(c);
    }
    send({Type::SongCount, 0, 0, 0, int16_t(song.count)});
    send({Type::SongLoop, 0, 0, song.loop_start, int16_t(song.loop_end)});
}

// On the Song page Play plays the arrangement from the cursor; elsewhere the
// patterns (open question 4: Song's controls).
void Context::set_arp(unsigned track, bool on) {
    uint8_t n = uint8_t(track % track_count);
    auto& t = project.tracks[n];
    t.arp = on;
    // Notes the pads hold on that track stop; the arp starts with what is pressed next.
    for (unsigned p = 0; p < pad_keys.size(); ++p)
        if (pad_track[p] == n) for (auto& k : pad_keys[p]) if (k >= 0) { note(n, k, 0); k = -1; }
    if (on) {
        send({Type::Arp, n, 0, 1, t.arp_mode}); send({Type::Arp, n, 0, 2, t.arp_rate});
        send({Type::Arp, n, 0, 3, int16_t(t.engine == EngineKind::Sampler ? 1 : t.arp_octaves)}); // a kit repeats its pad
        send({Type::Arp, n, 0, 4, t.arp_gate});
        send({Type::Arp, n, 0, 5, int16_t(!ui.settings.full_level())}); // pressure sets the repeats' velocity, unless Full Level
    }
    send({Type::Arp, n, 0, 0, int16_t(on)});
}

void Context::set_click(unsigned mode) {
    ui.settings.click = uint8_t(mode % 3);
    send_click();
    if (store) store->save_settings(ui.settings.text());
}

// The audio thread only hears on or off: Rec is on while Seq Record is.
void Context::send_click() {
    const auto& set = ui.settings;
    bool on = set.click == 1 || (set.click == 2 && (ui.recording || loop_click)); // a loop recording counts (docs/LOOPER.md)
    send({Type::Click, 0, 0, set.click_level, int16_t(on)});
}

void Context::capture() {
    auto& t = track();
    if (!t.used()) return;
    int into = -1; // the next empty pattern after the one playing, round to the first
    for (unsigned k = 1; k <= pattern_count && into < 0; ++k)
        if (patterns.at[ui.track][(t.pattern + k) % pattern_count].empty()) into = int((t.pattern + k) % pattern_count);
    if (into < 0) { notice("No empty pattern to capture into"); return; }
    double tick_ms = 60000.0 / (tempo_now() / 10.0) / (4.0 * sixteenth_ticks);
    double start = double(now) - double(project.bar_ticks()) * ui.settings.capture_bars * tick_ms;
    bool played = false;
    for (unsigned n = 0; n < std::min<unsigned>(history_at, history.size()) && !played; ++n)
        played = history[n].track == ui.track && history[n].at >= start && history[n].at <= now;
    if (!played) { notice("Nothing played to capture"); return; }
    edit_pattern(ui.track, unsigned(into), "Capture", [&] { capture_notes(unsigned(into)); });
    ui.captured_pattern = uint8_t(into);
    ui.captured_at = now ? now : 1;
}

void Context::capture_notes(unsigned into) {
    auto& t = track();
    const auto& set = ui.settings;
    uint32_t span = project.bar_ticks() * set.capture_bars; // whole bars of the signature
    double tick_ms = 60000.0 / (tempo_now() / 10.0) / (4.0 * sixteenth_ticks), start = double(now) - span * tick_ms;
    // As played (to the tick), or onto sixteenths or eighths.
    uint32_t grid = set.capture_quantise == 0 ? 1 : set.capture_quantise == 1 ? sixteenth_ticks : 2 * sixteenth_ticks;
    auto& p = patterns.at[ui.track][into % pattern_count];
    p.events.clear();
    p.division = Division::Sixteenth;
    p.length = span;
    for (unsigned n = 0; n < std::min<unsigned>(history_at, history.size()); ++n) {
        const auto& h = history[n];
        if (h.track != ui.track || h.at < start || h.at > now) continue;
        uint32_t at = uint32_t(std::lround((h.at - start) / tick_ms / grid)) * grid;
        if (at >= span) at = span - grid; // a note right at the end belongs to the last place
        double held = h.length ? h.length / tick_ms : double(sixteenth_ticks); // still held: a sixteenth
        uint32_t length = uint32_t(std::max(1L, std::lround(held / grid))) * grid;
        Event e;
        e.tick = at; e.key = h.key; e.velocity = h.velocity;
        e.length = uint16_t(std::min<uint32_t>({length, span, 65535u}));
        p.add(e);
    }
    sync_pattern(ui.track, into);
    t.pattern = uint8_t(into); // and it plays from here
    ui.lead_pattern = t.pattern;
    send({Type::Pattern, ui.track, 0, 0, int16_t(t.pattern)});
}

// Play: from the start (on Song, the arrangement from the cursor). Seq Record
// armed, the take begins, after a bar's count when Count-In is on.
void Context::play(bool external) {
    if (host.playing()) return;
    played_song = ui.page == PageId::Song;
    ui.span_count = 0; // the punches heard (pad-fx C) counted from this Play
    if (ui.rec_armed) { ui.rec_armed = false; set_recording(true); }
    // A loop armed while stopped counts in as a recording does (docs/LOOPER.md).
    bool count = (ui.recording || host.looper().armed()) && ui.settings.count_in && !external;
    if (played_song) send({Type::Play, uint8_t(count), 0, ui.song_cursor, 1});
    else send({Type::Play, uint8_t(count)});
    if (external) send({Type::ExtSync, 0, 0, 3}); // the ticks wait for the clock's pulses
}

// Play held while playing: Fill, until it is let go (trig conditions FILL and NOT FILL, model/trig.h).
void Context::fill(bool on) {
    if (on == ui.fill) return;
    ui.fill = on;
    send({Type::Fill, 0, 0, 0, int16_t(on)});
    if (on) notice("Fill");
}

// Shift + Play: on from where it stopped (or where the wheel moved it on Patterns).
void Context::play_continue(bool external) {
    if (host.playing()) return;
    ui.span_count = 0; // the punches heard (pad-fx C) counted from this Play
    if (ui.rec_armed) { ui.rec_armed = false; set_recording(true); }
    bool count = (ui.recording || host.looper().armed()) && ui.settings.count_in && !external;
    if (played_song) send({Type::Play, uint8_t(count), 0, uint16_t(host.song_bar()), 1}); // from the bar it stopped in
    else { Command c {Type::Play, uint8_t(count)}; c.position = ui.locate; send(c); }
    if (external) send({Type::ExtSync, 0, 0, 3});
}

// Shift + Loop: the last pad plays backwards, or forwards again.
void Context::toggle_reverse() {
    auto& t = track();
    if (t.engine != EngineKind::Sampler || !pad().used()) { notice("Reverse: no sample"); return; }
    pad().reverse = !pad().reverse;
    send_pad(ui.track, t.last_pad);
    dirty = true;
    notice(pad().reverse ? "Reverse on" : "Reverse off");
}

void Context::double_length() {
    if (pattern().length * 2 > project.max_length()) { notice("Double Seq: " + std::to_string(max_bars) + " bars at most"); return; }
    edit_pattern(ui.track, track().pattern, "Double Seq", [&] { double_notes(); });
    notice("Double Seq");
}

void Context::double_notes() {
    auto& p = pattern();
    bool copy = ui.settings.double_copies;
    if (!double_pattern(p, project.max_length(), copy)) return; // its motion too (Settings' Empty: the new half bare)
    sync_pattern(ui.track, track().pattern);
}

void Context::half_length() {
    auto& p = pattern();
    uint32_t half = std::max<uint32_t>(p.length / 2, 1);
    if (half == p.length) return;
    edit_pattern(ui.track, track().pattern, "Half Seq", [&] {
        half_pattern(p); // its motion past the half goes with the notes
        sync_pattern(ui.track, track().pattern);
    });
    notice("Half Seq");
}

void Context::half_speed() {
    edit_pattern(ui.track, track().pattern, "Half Speed", [&] { scale_notes(true); });
    notice("Half Speed");
}

void Context::double_speed() {
    edit_pattern(ui.track, track().pattern, "Double Speed", [&] { scale_notes(false); });
    notice("Double Speed");
}

// Every note (and its motion) twice as far from the start and twice as long,
// or half; the pattern keeps its length, and what falls past its end goes.
void Context::scale_notes(bool slower) {
    auto& p = pattern();
    scale_pattern(p, slower); // its motion with the notes
    sync_pattern(ui.track, track().pattern);
}

// Time Correct's Do It: the chosen notes onto the nearest step of Q, every
// second step of a straight Q late by the swing, then moved by the shift.
void Context::time_correct() {
    const auto& t = track();
    edit_pattern(ui.track, t.pattern, "Time Correct", [&] {
        time_correct_all(pattern(), ui.tc_all ? nullptr : &ui.tc_keys, ui.tc_swing, ui.tc_shift);
        sync_pattern(ui.track, t.pattern);
    });
    notice("Time Correct");
}

// Seq Record on: the take begins, with what it may change as it is now.
// Off: the clips being written end, and the take goes into the history.
void Context::set_recording(bool on) {
    if (on == ui.recording) return;
    ui.recording = on;
    send({Type::SongRecord, 0, 0, 0, int16_t(on)});
    if (ui.settings.click == 2) send_click(); // the Rec metronome follows Seq Record
    if (on) {
        ui.rec_armed = false;
        take_track = ui.track; take_pattern = track().pattern;
        take_edit = std::make_unique<Edit>(begin_edit("Take", ui.track, track().pattern, true));
        return;
    }
    for (auto& h : held_notes) h.key = -1; // notes held on keep the length they were written with
    song_tick(); // finishes the clips and the punch being written
    if (take_edit) commit_edit(std::move(*take_edit));
    take_edit.reset();
}

// Seq Record, as on the MPC: stopped it arms (Play starts the take); playing
// it records at once; recording, it stops recording and playback goes on.
void Context::seq_record() {
    if (ui.recording) { set_recording(false); return; }
    if (host.playing()) { set_recording(true); return; }
    ui.rec_armed = !ui.rec_armed;
}

// Stop ends playback and recording together; Shift + Play carries on from here.
void Context::stop() {
    if (!host.playing()) send({Type::AllNotesOff, 255});
    else if (!host.song_playing()) ui.locate = uint32_t(std::max(host.position_of(ui.track), 0));
    ui.rec_armed = false;
    if (ui.recording) set_recording(false);
    send({Type::Stop});
}

// The tempo in play: the global one, or with BPM per pattern the lead pattern's own.
void Context::set_tempo(int tenths) {
    tenths = std::clamp(tenths, 300, 3000);
    if (project.seq_tempo) {
        project.pattern_tempo[ui.lead_pattern % pattern_count] = uint16_t(tenths);
        send({Type::PatternTempo, 0, 0, ui.lead_pattern, int16_t(tenths)});
        return;
    }
    project.tempo = uint16_t(tenths);
    send({Type::Tempo, 0, 0, 0, int16_t(project.tempo)});
}

void Context::tap_tempo() {
    if (tap_count && now - taps[(tap_count - 1) % taps.size()] > 2000) tap_count = 0; // a pause starts over
    taps[tap_count % taps.size()] = now;
    ++tap_count;
    if (tap_count < 2) return;
    unsigned n = std::min<unsigned>(tap_count, unsigned(taps.size()));
    uint32_t first = taps[(tap_count - n) % taps.size()], last = taps[(tap_count - 1) % taps.size()];
    if (last > first) set_tempo(int(600000.0 * (n - 1) / double(last - first) + 0.5));
}

} // namespace melodee
