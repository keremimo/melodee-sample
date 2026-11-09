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
    if (ui.page == PageId::Sounds && page != PageId::Sounds) audition(-1);
    if (page != ui.page) { ui.previous = ui.page; ui.page = page; }
    if (page == PageId::Sounds) {
        // The cursor starts on the loaded sound.
        ui.sounds_filter = 0;
        ui.sounds_cursor = track().preset;
    }
}

void Context::back() {
    if (ui.sheet_open) { ui.sheet_open = false; return; } // Shift + Push closes a sheet first
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
    unsigned at = host.position() % p.length;
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
    case Kind::None: break;
    }
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
    e.step = uint16_t(host.position() % pattern().length);
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

void Context::play() { send({Type::Play}); }
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
