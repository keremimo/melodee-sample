// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/context.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/sampler.h"
#include <cmath>
#include "melodee/engine/take_recorder.h"
#include "melodee/model/chords.h"
#include "melodee/model/scales.h"
#include "melodee/model/project_file.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/name_suggest.h"
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace melodee {
using Type = Command::Type;

void Context::send(const Command& command) {
    // Anything but playing changes the project.
    switch (command.type) {
    case Type::NoteOn: case Type::NoteOff: case Type::AllNotesOff: case Type::Wheel: case Type::Pressure:
    case Type::Audition: case Type::PadNote: case Type::Play: case Type::Stop: case Type::Reload:
    case Type::RecordArm: case Type::RecordNow: case Type::RecordStop: break;
    default: dirty = true;
    }
    host.send(command);
}

void Context::save_project_to(const std::string& path) {
    if (!store || path.empty()) return;
    auto name = std::filesystem::path(path).stem().string();
    std::snprintf(project.name, sizeof(project.name), "%s", name.c_str());
    store->save(path, melodee::save_project(project, patterns));
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

void Context::switch_to(const Project& next, const PatternBank& bank, const std::string& path) {
    send({Type::AllNotesOff, 255});
    project = next;
    patterns = bank;
    auto keep = ui.pots;
    ui = UiState {};
    ui.pots = keep;
    for (auto& pot : ui.pots) pot.bound = 0; // pick the new values up again
    auto* pack = new ProjectPack {project, patterns};
    Command c {Type::Reload};
    c.pack = pack;
    if (!host.send(c)) delete pack;
    project_path = path;
    dirty = false;
    samples_to_load = 0; // every sample slot again, a few each tick
}

void Context::open(PageId page) {
    if (page != ui.page) punch_release(); // a pad held over a page change would never let go
    ui.sheet_open = false;
    ui.move_open = false;
    ui.map_open = false;
    ui.picker_open = false;
    if (ui.page == PageId::Sounds && page != PageId::Sounds) audition(-1);
    if (page != ui.page) { ui.previous = ui.page; ui.page = page; }
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
    open(ui.previous == ui.page ? PageId::Stage : ui.previous); }

void Context::select_track(unsigned t) {
    if (ui.page == PageId::Sounds) audition(-1);
    ui.track = uint8_t(t % track_count);
    ui.held_step = -1;
    if (ui.page == PageId::Sounds) ui.sounds_cursor = track().preset;
}

void Context::note(unsigned t, int key, int velocity) {
    if (key < 0 || key > 127) return;
    Command c {velocity ? Type::NoteOn : Type::NoteOff, uint8_t(t), 0, uint16_t(key), int16_t(velocity)};
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
    if (down) {
        record(k, velocity);
        history[history_at % history.size()] = {ui.track, uint8_t(k), uint8_t(velocity), now, 0};
        ++history_at;
    } else {
        for (unsigned n = 0; n < std::min<unsigned>(history_at, history.size()); ++n) {
            auto& h = history[(history_at - 1 - n) % history.size()];
            if (h.track == ui.track && h.key == k && !h.length) { h.length = std::max<uint32_t>(1, now - h.at); break; }
        }
    }
}

void Context::play_pad(unsigned pad, uint8_t velocity, bool down) {
    pad %= 16;
    auto& t = track();
    // Let go of whatever this pad started.
    for (auto& k : pad_keys[pad]) if (k >= 0) { key(k, 0, false); k = -1; }
    if (!down) return;
    if (t.arp && t.arp_latch && !held_pads()) { // a fresh latch: the old keys go
        send({Type::Arp, ui.track, 0, 0, 0}); send({Type::Arp, ui.track, 0, 0, 1});
    }
    if (t.engine == EngineKind::Sampler) { // the pads are the kit's, in the bank in view
        if (t.levels) { // 16 Levels: one kit pad, soft to loud or up in pitch
            int semitones = t.levels_kind == 1 ? level_scale_step(project, pad) : t.levels_kind == 2 ? int(pad) : 0;
            int v = t.levels_kind == 0 ? level_velocity(pad) : velocity;
            ui.levels_hit = int8_t(pad); ui.levels_value = int16_t(t.levels_kind == 0 ? v : semitones);
            pad_keys[pad][0] = int16_t(sampler_first_note + t.levels_pad);
            send({Type::PadNote, ui.track, uint8_t(semitones + 64), t.levels_pad, int16_t(v)});
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
        if (pad >= 11) return;
        pad_keys[pad][0] = int16_t(36 + pad);
        t.last_note = uint8_t(36 + pad);
        key(pad_keys[pad][0], velocity, true);
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

// Seq Record while playing: the note lands on the step playing now.
void Context::record(int key, int velocity) {
    if (!ui.recording || !host.playing()) return;
    auto& p = pattern();
    unsigned at = unsigned(std::max(host.step_of(ui.track), 0)) % p.length;
    Step step = p.steps[at];
    for (auto& n : step.notes) if (n.velocity && n.key == key) return;
    for (auto& n : step.notes) if (!n.velocity) { n = {uint8_t(key), uint8_t(velocity)}; break; }
    set_step(ui.track, at, step);
    // Where in the step it fell, for Quantise take: past half way is late.
    unsigned division = division_ticks(p.division);
    bool late = host.ticks() % division >= division / 2;
    if (take_count < take_notes.size()) take_notes[take_count++] = {uint16_t(at), uint8_t(key), late};
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
    auto& t = track();
    switch (b.kind) {
    case Kind::Param:
        set_param(ui.track, b.id, value); record_motion(b.id, value);
        if (t.engine == EngineKind::Sampler && b.id == sampler::Follow && value) follow_tempos();
        break;
    case Kind::Level: t.mix.level = uint8_t(value); send({Type::Level, ui.track, 0, 0, int16_t(value)}); break;
    case Kind::Pan: t.mix.pan = int8_t(value); send({Type::Pan, ui.track, 0, 0, int16_t(value)}); break;
    case Kind::Send: t.mix.send = uint8_t(value); send({Type::Send, ui.track, 0, 0, int16_t(value)}); break;
    case Kind::Root: project.root = uint8_t(value); break;
    case Kind::Scale: project.scale = uint8_t(value); break;
    case Kind::Layout: t.layout = PadLayout(value); break;
    case Kind::Engine: {
        std::array<EngineKind, size_t(EngineKind::Count)> engines {};
        unsigned count = available_engines(engines);
        if (unsigned(value) < count && engines[size_t(value)] != t.engine) load_sound(ui.track, engines[size_t(value)], 0);
        break;
    }
    case Kind::Jump: ui.sounds_cursor = uint16_t(value); break;
    case Kind::Audition: ui.audition_velocity = uint8_t(value); break;
    case Kind::StepLength:
    case Kind::StepVelocity:
    case Kind::StepChance: {
        unsigned at = unsigned(std::max<int>(ui.step_cursor, 0)) % max_steps;
        Step step = pattern().steps[at];
        if (step.empty()) break;
        if (b.kind == Kind::StepLength) step.length = uint8_t(value);
        else if (b.kind == Kind::StepChance) step.chance = uint8_t(value);
        else for (auto& n : step.notes) if (n.velocity) n.velocity = uint8_t(value);
        set_step(ui.track, at, step);
        ui.held_edited = true;
        break;
    }
    case Kind::ModSource:
    case Kind::ModDest:
    case Kind::ModAmount: {
        auto route = t.mods[size_t(b.id) % mod_routes];
        if (b.kind == Kind::ModSource) route.source = uint8_t(value);
        else if (b.kind == Kind::ModDest) route.destination = uint8_t(value);
        else route.amount = int8_t(value);
        set_route(unsigned(b.id), route);
        break;
    }
    case Kind::PatternBars: set_length(unsigned(value) * bar_steps); break;
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
    case Kind::SendBus: set_send_bus(ui.track, unsigned(value)); break;
    case Kind::BusReturn: set_return(unsigned(b.id), value); break;
    case Kind::PadPitch: pad().pitch = int8_t(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadLevel: pad().level = uint8_t(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadPan: pad().pan = int8_t(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadStart: pad().start = uint32_t(uint64_t(value) * 44100 / 1000); send_pad(ui.track, t.last_pad); break;
    case Kind::PadEnd: {
        uint32_t frames = project.samples[pad().sample % sample_slots].frames, at = uint32_t(uint64_t(value) * 44100 / 1000);
        pad().end = at >= frames ? 0 : at; // the sample's end is kept as 0, so it follows a longer reload
        send_pad(ui.track, t.last_pad);
        break;
    }
    case Kind::TrimZoom: ui.trim_zoom = uint8_t(value); break;
    case Kind::ChopMode: ui.chop_mode = ChopMode(value); rechop(); break;
    case Kind::ChopAmount: ui.chop_amount[size_t(ui.chop_mode)] = value; rechop(); break;
    case Kind::ChopStart: ui.slices.start[size_t(b.id) % max_slices] = uint32_t(uint64_t(value) * 44100 / 1000); break;
    case Kind::FileJump: ui.files_cursor = uint16_t(value); break;
    case Kind::ProjectJump: ui.project_cursor = uint16_t(value); break;
    case Kind::ProjectSort: ui.project_sort = uint8_t(value); break;
    case Kind::PadFine: pad().fine = int8_t(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadPlayMode: pad().play = PadPlay(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadGroup: pad().group = uint8_t(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadReverse: pad().reverse = value != 0; send_pad(ui.track, t.last_pad); break;
    case Kind::PadAttack: pad().attack = uint8_t(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadDecay: pad().decay = uint8_t(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadRelease: pad().release = uint8_t(value); send_pad(ui.track, t.last_pad); break;
    case Kind::PadSustain: pad().sustain = uint8_t(value); send_pad(ui.track, t.last_pad); break;
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
        if (ui.punch_held >= 0 || ui.punch_locked >= 0) send({Type::PunchDepth, 254, 0, 0, int16_t(value)});
        break;
    case Kind::PunchSnap: project.punch_snap = uint8_t(value); break;
    case Kind::PunchTail: project.punch_tail = uint8_t(value); break;
    case Kind::PadPunchKind: pad().punch = uint8_t(value); break;
    case Kind::PadPunchRate: pad().punch_rate = uint8_t(value); break;
    case Kind::PadPunchDepth: pad().punch_depth = uint8_t(value); break;
    case Kind::ClickLevel:
        project.click_level = uint8_t(value);
        send({Type::Click, 0, 0, project.click_level, int16_t(project.click)});
        break;
    case Kind::ArpMode: t.arp_mode = uint8_t(value); send({Type::Arp, ui.track, 0, 1, int16_t(value)}); break;
    case Kind::ArpRate: t.arp_rate = uint8_t(value); send({Type::Arp, ui.track, 0, 2, int16_t(value)}); break;
    case Kind::ArpOctaves: t.arp_octaves = uint8_t(value); send({Type::Arp, ui.track, 0, 3, int16_t(value)}); break;
    case Kind::ChordType: t.chord_type = uint8_t(value); break;
    case Kind::ChordVoicing: t.chord_voicing = uint8_t(value); break;
    case Kind::ChordInvert: t.chord_invert = uint8_t(value); break;
    case Kind::CaptureBars: ui.capture_bars = uint8_t(value); break;
    case Kind::CaptureQuantise: ui.capture_quantise = uint8_t(value); break;
    case Kind::CaptureInto: ui.capture_into = uint8_t(value); break;
    case Kind::RecThreshold: ui.rec_threshold = int8_t(value); break;
    case Kind::RecBars: ui.rec_bars = uint8_t(value); break;
    case Kind::RecGain: ui.rec_gain = int8_t(value); send({Type::RecordGain, 0, 0, 0, int16_t(value)}); break;
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

void Context::punch_pad(unsigned pad, bool down) {
    unsigned kind = project.punch_bank % 2 * 16 + pad % 16;
    if (kind >= punch_kinds) return;
    auto on = [&] {
        send({Type::PunchOn, uint8_t(project.punch_target ? ui.track : 255), uint8_t(project.punch_snap | project.punch_tail << 4),
              uint16_t(kind), int16_t(project.punch_depth)});
    };
    uint8_t target = uint8_t(project.punch_target ? ui.track : 255);
    if (down && ui.shift) { // lock on, or off again
        ui.shift_used = true;
        if (ui.punch_locked == int(kind)) { send({Type::PunchOff, 254}); ui.punch_locked = -1; end_punch(); }
        else { on(); ui.punch_locked = int8_t(kind); ui.punch_held = -1; begin_punch(kind, target, project.punch_depth); }
        return;
    }
    if (down) { on(); ui.punch_held = int8_t(kind); ui.punch_locked = -1; begin_punch(kind, target, project.punch_depth); return; } // the last pressed wins
    if (ui.punch_held == int(kind)) { send({Type::PunchOff, 254}); ui.punch_held = -1; end_punch(); }
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

void Context::punch_release() {
    if (ui.punch_held >= 0) { send({Type::PunchOff, 254}); ui.punch_held = -1; end_punch(); }
    if (ui.pad_punch_held >= 0) { send({Type::PunchOff, ui.track, 0, uint16_t((ui.pad_punch_held + 1) << 8)}); ui.pad_punch_held = -1; }
}

void Context::pressure(unsigned pad, uint8_t value) {
    if (pad >= 16) return;
    if (ui.page == PageId::PadPunch && ui.pad_punch_held >= 0 && ui.pad_punch_held % 16 == int(pad)) { // pressure is depth
        ui.pad_punch_pressure = value;
        send({Type::PunchDepth, ui.track, 0, uint16_t((ui.pad_punch_held + 1) << 8), int16_t(value * 100 / 127)});
        return;
    }
    for (auto k : pad_keys[pad]) if (k >= 0) send({Type::Pressure, ui.track, 0, uint16_t(k), int16_t(value)}); // every note of a chord
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
    patterns.at[t % track_count][tr.pattern].steps[at % max_steps] = value;
    Command c {Type::SetStep, uint8_t(t), tr.pattern, uint16_t(at % max_steps), 0};
    c.step = value;
    send(c);
}

void Context::record_motion(int param, int value) {
    if (!ui.recording || !host.playing() || param < 0) return;
    auto& t = track();
    MotionEvent e;
    e.track = ui.track; e.pattern = t.pattern; e.param = uint8_t(param); e.value = int16_t(value);
    e.step = uint16_t(unsigned(std::max(host.step_of(ui.track), 0)) % pattern().length);
    if (!project.motion.write(e)) return; // full: the move still sounds, it is just not kept
    Command c {Type::MotionWrite};
    c.motion = e;
    send(c);
}

void Context::clear_lane(int param) {
    if (param < 0) return;
    auto& t = track();
    project.motion.clear(ui.track, t.pattern, uint8_t(param));
    Command c {Type::MotionClear};
    c.motion.track = ui.track; c.motion.pattern = t.pattern; c.motion.param = uint8_t(param);
    send(c);
}

void Context::launch(unsigned t, unsigned p) {
    auto& tr = project.tracks[t % track_count];
    tr.pattern = uint8_t(p % pattern_count);
    send({Type::Pattern, uint8_t(t), 0, 0, int16_t(tr.pattern)});
    if (ui.recording && host.song_playing()) record_launch(t % track_count, tr.pattern);
}

unsigned Context::song_sixteenth() const { return host.song_bar() * 16 + (host.ticks() % ticks_per_bar) / (ticks_per_bar / 16); }

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
    for (unsigned s = 0; s < max_steps; ++s) {
        if (pattern.steps[s].empty()) continue;
        pattern.steps[s] = Step {};
        Command c {Type::SetStep, uint8_t(t), uint8_t(p), uint16_t(s)};
        send(c);
    }
}

void Context::copy_pattern(unsigned t, unsigned from, unsigned to) {
    if (from == to) return;
    const auto source = patterns.at[t % track_count][from % pattern_count];
    auto& target = patterns.at[t % track_count][to % pattern_count];
    for (unsigned s = 0; s < max_steps; ++s) {
        if (target.steps[s].empty() && source.steps[s].empty()) continue;
        target.steps[s] = source.steps[s];
        Command c {Type::SetStep, uint8_t(t), uint8_t(to), uint16_t(s)};
        c.step = source.steps[s];
        send(c);
    }
    target.length = source.length; target.division = source.division;
    send({Type::Length, uint8_t(t), uint8_t(to), 0, int16_t(target.length)});
    send({Type::Division, uint8_t(t), uint8_t(to), 0, int16_t(target.division)});
}

void Context::set_length(unsigned steps) {
    auto& p = pattern();
    p.length = uint16_t(std::clamp<unsigned>(steps, 1, max_steps));
    send({Type::Length, ui.track, track().pattern, 0, int16_t(p.length)});
}

void Context::set_division(Division division) {
    pattern().division = division;
    send({Type::Division, ui.track, track().pattern, 0, int16_t(division)});
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
    library->load(unsigned(free), path);
    return free;
}

void Context::rechop() {
    if (!library || ui.chop_sample >= sample_slots) { ui.slices = {}; return; }
    auto loaded = library->slot(ui.chop_sample);
    ui.slices = chop(ui.chop_mode, ui.chop_amount[size_t(ui.chop_mode)], loaded.frames, loaded.hits, loaded.hit_count, project.tempo);
    ui.chop_slice = uint8_t(std::min<unsigned>(ui.chop_slice, ui.slices.count ? ui.slices.count - 1 : 0));
}

// Arms a take for the first free sample slot and the first empty pad; a
// second press starts it at once, a third stops it.
void Context::record_press() {
    auto state = host.recorder().state();
    if (state == TakeRecorder::State::Armed) { send({Type::RecordNow}); return; }
    if (state == TakeRecorder::State::Recording) { send({Type::RecordStop}); return; }
    if (state != TakeRecorder::State::Idle || ui.rec_slot >= 0) return;
    int slot = -1, empty = -1;
    for (unsigned n = 0; n < sample_slots && slot < 0; ++n) if (!project.samples[n].used()) slot = int(n);
    for (unsigned n = 0; n < sampler_pads && empty < 0; ++n) if (!track().pads[n].used()) empty = int(n);
    if (slot < 0 || empty < 0) return;
    auto& ref = project.samples[size_t(slot)];
    std::strncpy(ref.path, "(recording)", sizeof(ref.path) - 1); // held for the take
    std::strncpy(ref.name, "TAKE", sizeof(ref.name) - 1);
    ui.rec_slot = int16_t(slot); ui.rec_pad = int16_t(empty);
    send({Type::RecordArm, ui.rec_source, uint8_t(ui.rec_bars), uint16_t(slot), int16_t(ui.rec_threshold)});
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
    auto& motion = project.motion;
    for (unsigned e = 0; e < motion.count;) {
        const auto ev = motion.events[e];
        if (ev.track != n || ev.param < motion_fx_base) { ++e; continue; }
        motion.clear(ev.track, ev.pattern, ev.param);
        Command c {Type::MotionClear};
        c.motion = ev;
        send(c);
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

void Context::set_send_bus(unsigned t, unsigned bus) {
    project.tracks[t % track_count].mix.bus = uint8_t(bus % bus_count);
    send({Type::SendBus, uint8_t(t), 0, 0, int16_t(bus % bus_count)});
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
void Context::set_arp(bool on) {
    auto& t = track();
    t.arp = on;
    // Notes held now stop; the arp starts with what is pressed next.
    for (auto& pad : pad_keys) for (auto& k : pad) if (k >= 0) { note(ui.track, k, 0); k = -1; }
    if (on) {
        send({Type::Arp, ui.track, 0, 1, t.arp_mode}); send({Type::Arp, ui.track, 0, 2, t.arp_rate});
        send({Type::Arp, ui.track, 0, 3, int16_t(t.engine == EngineKind::Sampler ? 1 : t.arp_octaves)}); // a kit repeats its pad
        send({Type::Arp, ui.track, 0, 4, t.arp_gate});
    }
    send({Type::Arp, ui.track, 0, 0, int16_t(on)});
}

void Context::set_click(bool on) {
    project.click = on;
    send({Type::Click, 0, 0, project.click_level, int16_t(on)});
}

void Context::capture() {
    auto& t = track();
    double step_ms = 60000.0 / (project.tempo / 10.0) / 4;
    double window = step_ms * bar_steps * ui.capture_bars, start = double(now) - window;
    auto& p = patterns.at[ui.track][ui.capture_into % pattern_count];
    clear_pattern(ui.track, ui.capture_into);
    p.length = uint16_t(bar_steps * ui.capture_bars);
    send({Type::Length, ui.track, ui.capture_into, 0, int16_t(p.length)});
    unsigned grid = ui.capture_quantise == 2 ? 2 : 1; // 1/16, or 1/8 (every other step)
    for (unsigned n = 0; n < std::min<unsigned>(history_at, history.size()); ++n) {
        const auto& h = history[n];
        if (h.track != ui.track || h.at < start || h.at > now) continue;
        unsigned at = unsigned(std::lround((h.at - start) / step_ms / grid)) * grid;
        if (at >= p.length) at = p.length - grid; // a note right at the end belongs to the last step
        Step s = p.steps[at];
        for (auto& note : s.notes) if (!note.velocity) { note = {h.key, h.velocity}; break; }
        s.length = uint8_t(std::clamp<long>(std::lround((h.length ? h.length : step_ms) / step_ms), 1, 16));
        set_step(ui.track, at, s);
    }
    t.pattern = ui.capture_into; // and it plays from here
    send({Type::Pattern, ui.track, 0, 0, int16_t(t.pattern)});
}

void Context::play() {
    played_song = ui.page == PageId::Song;
    if (played_song) send({Type::Play, 0, 0, ui.song_cursor, 1});
    else send({Type::Play});
}

void Context::play_continue() {
    if (host.playing()) return;
    if (played_song) send({Type::Play, 0, 0, uint16_t(host.song_bar()), 1}); // from the bar it stopped in
    else send({Type::Play});
}

void Context::double_length() {
    auto& p = pattern();
    unsigned length = p.length;
    if (length * 2 > max_steps) return;
    for (unsigned s = 0; s < length; ++s)
        if (!p.steps[s].empty()) set_step(ui.track, s + length, p.steps[s]);
    // Its motion too, as far as there is room.
    auto& motion = project.motion;
    unsigned count = motion.count;
    for (unsigned n = 0; n < count; ++n) {
        MotionEvent e = motion.events[n];
        if (e.track != ui.track || e.pattern != track().pattern || e.step >= length) continue;
        e.step = uint16_t(e.step + length);
        if (!motion.write(e)) break;
        Command c {Type::MotionWrite};
        c.motion = e;
        send(c);
    }
    set_length(length * 2);
}

void Context::begin_take() {
    take_count = 0;
    take_track = ui.track; take_pattern = track().pattern;
    if (!before_take) before_take = std::make_unique<Pattern>();
    if (!song_before_take) song_before_take = std::make_unique<Song>();
    *before_take = pattern();
    *song_before_take = project.song;
}

void Context::undo_take() {
    if (take_track < 0 || !before_take) return;
    if (ui.recording) { ui.recording = false; send({Type::SongRecord, 0, 0, 0, 0}); song_tick(); }
    unsigned t = unsigned(take_track), p = unsigned(take_pattern);
    auto& now = patterns.at[t][p];
    for (unsigned s = 0; s < max_steps; ++s) {
        if (now.steps[s].empty() && before_take->steps[s].empty()) continue;
        now.steps[s] = before_take->steps[s];
        Command c {Type::SetStep, uint8_t(t), uint8_t(p), uint16_t(s)};
        c.step = now.steps[s];
        send(c);
    }
    now.length = before_take->length;
    send({Type::Length, uint8_t(t), uint8_t(p), 0, int16_t(now.length)});
    project.song = *song_before_take;
    sync_song();
    sync_punches();
    take_count = 0; take_track = -1;
}

void Context::quantise_take() {
    if (take_track < 0 || unsigned(take_track) != ui.track || int(track().pattern) != take_pattern) return;
    auto& p = pattern();
    for (unsigned n = 0; n < take_count; ++n) {
        auto& note = take_notes[n];
        if (!note.late) continue;
        unsigned from = note.step % p.length, to = (from + 1) % p.length;
        Step a = p.steps[from], b = p.steps[to];
        Note moving {};
        for (auto& x : a.notes) if (x.velocity && x.key == note.key) { moving = x; x = Note {}; break; }
        if (!moving.velocity) continue;
        bool there = false;
        for (auto& x : b.notes) there |= x.velocity && x.key == note.key;
        if (!there) for (auto& x : b.notes) if (!x.velocity) { x = moving; there = true; break; }
        if (!there) continue; // no room: it stays
        set_step(ui.track, from, a); set_step(ui.track, to, b);
        note.step = uint16_t(to); note.late = false;
    }
}

void Context::transpose(int way) {
    auto& t = track();
    t.transpose = int8_t(std::clamp(int(t.transpose) + way, -12, 12));
}
void Context::stop() {
    if (!host.playing()) send({Type::AllNotesOff, 255});
    send({Type::Stop});
}

void Context::set_tempo(int tenths) {
    project.tempo = uint16_t(std::clamp(tenths, 300, 3000));
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
