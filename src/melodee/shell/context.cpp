// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/context.h"
#include "melodee/engine/engine_host.h"
#include "melodee/model/scales.h"
#include <algorithm>

namespace melodee {
using Type = Command::Type;

void Context::send(const Command& command) { host.send(command); }

void Context::open(PageId page) {
    ui.sheet_open = false;
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
    if (ui.sheet_open || ui.map_open || ui.picker_open) { ui.sheet_open = ui.map_open = ui.picker_open = false; return; } // a sheet, map or picker first
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

void Context::play_pad(unsigned pad, uint8_t velocity, bool down) {
    pad %= 16;
    if (!down) {
        if (pad_keys[pad] >= 0) note(ui.track, pad_keys[pad], 0);
        pad_keys[pad] = -1;
        return;
    }
    auto& t = track();
    if (!t.used()) return;
    auto chosen = pad_note(project, t, pad);
    int v = t.layout == PadLayout::Levels ? level_velocity(pad) : velocity;
    if (ui.page == PageId::Sounds) v = ui.audition_velocity;
    if (pad_keys[pad] >= 0) note(ui.track, pad_keys[pad], 0);
    pad_keys[pad] = int16_t(chosen.key);
    if (t.layout != PadLayout::Levels) t.last_note = uint8_t(std::clamp(chosen.key, 0, 127));
    note(ui.track, chosen.key, v);
    record(chosen.key, v);
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
    melodee::load_sound(tr, kind, sound);
    if (engine_changed) send({Type::Engine, uint8_t(t), 0, uint16_t(kind), 0});
    send({Type::Sound, uint8_t(t), 0, uint16_t(tr.preset), 0});
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
    send({Type::Sound, ui.track, 0, uint16_t(sound), 0});
    ui.auditioning = int16_t(sound);
}

void Context::apply(const Binding& b, int value) {
    using Kind = Binding::Kind;
    value = std::clamp(value, b.min, b.max);
    auto& t = track();
    switch (b.kind) {
    case Kind::Param: set_param(ui.track, b.id, value); record_motion(b.id, value); break;
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
    case Kind::FxParam: set_effect_param(unsigned(b.id) / 64, (unsigned(b.id) / 8) % 8, unsigned(b.id) % 8, value); break;
    case Kind::SendBus: set_send_bus(ui.track, unsigned(value)); break;
    case Kind::BusReturn: set_return(unsigned(b.id), value); break;
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

void Context::pressure(unsigned pad, uint8_t value) {
    if (pad < 16 && pad_keys[pad] >= 0) send({Type::Pressure, ui.track, 0, uint16_t(pad_keys[pad]), int16_t(value)});
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

void Context::set_effect(unsigned n, unsigned slot, EffectKind kind) {
    auto& chain = project.chain(n);
    if (slot >= chain.size) return;
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
    send({Type::FxKind, uint8_t(n), 0, uint16_t(slot), int16_t(kind)}); // the host takes the same initial values
    send({Type::FxBypass, uint8_t(n), 0, uint16_t(slot), 0});
}

void Context::set_effect_param(unsigned n, unsigned slot, unsigned param, int value) {
    auto& chain = project.chain(n);
    if (slot >= chain.size || param >= effect_params) return;
    chain.slots[slot].params[param] = int16_t(value);
    send({Type::FxParam, uint8_t(n), uint8_t(param), uint16_t(slot), int16_t(value)});
}

void Context::toggle_bypass(unsigned n, unsigned slot) {
    auto& s = project.chain(n).slots[slot % max_slots];
    s.bypass = !s.bypass;
    send({Type::FxBypass, uint8_t(n), 0, uint16_t(slot), int16_t(s.bypass)});
}

void Context::move_effect(unsigned n, unsigned slot, int way) {
    auto& chain = project.chain(n);
    int to = int(slot) + way;
    if (to < 0 || unsigned(to) >= chain.count() || slot >= chain.count()) return;
    std::swap(chain.slots[slot], chain.slots[size_t(to)]);
    sync_chain(n);
}

void Context::sync_chain(unsigned n) {
    const auto& chain = project.chain(n);
    for (unsigned k = 0; k < max_slots; ++k) {
        const auto& s = chain.slots[k];
        send({Type::FxKind, uint8_t(n), 0, uint16_t(k), int16_t(s.kind)});
        for (unsigned q = 0; q < effect_params; ++q) send({Type::FxParam, uint8_t(n), uint8_t(q), uint16_t(k), s.params[q]});
        send({Type::FxBypass, uint8_t(n), 0, uint16_t(k), int16_t(s.bypass)});
    }
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
void Context::play() {
    if (ui.page == PageId::Song) send({Type::Play, 0, 0, ui.song_cursor, 1});
    else send({Type::Play});
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
