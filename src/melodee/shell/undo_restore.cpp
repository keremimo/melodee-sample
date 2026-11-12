// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The Context side of undo for pots and page presses (parity phase 10,
// shell/undo_scope.h): taking a scope's state, keeping what changed, and
// restoring it with the commands the audio thread needs.
#include "melodee/shell/undo_scope.h"
#include "melodee/shell/context.h"
#include <cstring>

namespace melodee {
using Type = Command::Type;

namespace {
// Copies of the same model data compare byte for byte; the states built field by field have their own ==.
template <class T> bool same(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }
bool same(const MixState& a, const MixState& b) { return a == b; }
bool same(const SoundState& a, const SoundState& b) { return a == b; }
bool same(const MasterState& a, const MasterState& b) { return a == b; }
bool same(const ChopState& a, const ChopState& b) { return a == b; }
bool same(const Pattern& a, const Pattern& b) { return a == b; } // its events are on the heap
bool same(const TrackMotion& a, const TrackMotion& b) { return a == b; } // and its motion
} // namespace

// What the scope holds now, as the edit's before.
std::unique_ptr<Edit> Context::scope_edit(const UndoScope& s, const std::string& what) {
    using S = UndoScope::Kind;
    if (s.none()) return nullptr;
    auto e = std::make_unique<Edit>();
    e->what = what; e->track = s.track;
    switch (s.kind) {
    case S::Pad: e->pad = int16_t(s.index % sampler_pads); e->pad_before = pad_state(project.tracks[s.track], s.index); break;
    case S::Mix: e->mix_before = std::make_unique<MixState>(mix_of(project)); break;
    case S::Chain:
        e->chain = int16_t(s.index); e->chain_before = std::make_unique<Chain>(project.chain(s.index));
        if (!ui.recording && s.index < track_count) e->motion_before = std::make_unique<TrackMotion>(track_motion(s.index)); // moving or removing an effect clears its lanes
        break;
    case S::Master: e->master_before = std::make_unique<MasterState>(master_of(project)); break;
    case S::Sound: e->sound_before = std::make_unique<SoundState>(sound_of(project.tracks[s.track])); break;
    case S::Song: e->song_before = std::make_unique<Song>(project.song); break;
    case S::Chop: e->chop_before = std::make_unique<ChopState>(chop_of(ui)); break;
    case S::Pattern:
        e->pattern = int16_t(s.index);
        e->pattern_before = std::make_unique<Pattern>(patterns.at[s.track % track_count][s.index % pattern_count]);
        break;
    case S::None: break;
    }
    return e;
}

// The edit's after, part by part; false when nothing changed (then nothing is kept).
bool Context::finish_scope(Edit& e) {
    bool changed = false;
    if (e.pad >= 0) {
        e.pad_after = pad_state(project.tracks[e.track], unsigned(e.pad));
        changed |= !same(e.pad_after.sound, e.pad_before.sound) || !same(e.pad_after.chain, e.pad_before.chain);
    }
    auto part = [&](auto& before, auto& after, const auto& now) {
        if (!before) return;
        if (same(*before, now)) { before.reset(); return; }
        after = std::make_unique<std::decay_t<decltype(now)>>(now);
        changed = true;
    };
    if (e.pattern >= 0) part(e.pattern_before, e.pattern_after, patterns.at[e.track % track_count][unsigned(e.pattern) % pattern_count]);
    if (e.chain >= 0) part(e.chain_before, e.chain_after, project.chain(unsigned(e.chain)));
    if (e.motion_before) part(e.motion_before, e.motion_after, track_motion(e.motion_before->track));
    part(e.mix_before, e.mix_after, mix_of(project));
    part(e.master_before, e.master_after, master_of(project));
    part(e.sound_before, e.sound_after, sound_of(project.tracks[e.track]));
    part(e.song_before, e.song_after, project.song);
    part(e.chop_before, e.chop_after, chop_of(ui));
    return changed;
}

// A pot is about to change a value: its gesture goes on, or the open one is kept and a new one begins.
void Context::touch_gesture(const Binding& b) {
    auto scope = binding_scope(project, ui, b);
    touch_scope(scope, b.key(ui.track) ^ (scope.kind == UndoScope::Kind::Pad ? uint32_t(scope.index) << 16 : 0));
}

void Context::touch_scope(const UndoScope& scope, uint32_t key) {
    if (ui.recording) { close_gesture(); return; } // a take keeps its own moves
    if (held(scope)) { close_gesture(); return; }  // the held pad's step takes it in
    if (gesture.edit && gesture.key == key && gesture.scope == scope && now - gesture.last < gesture_quiet_ms) { gesture.last = now; return; }
    close_gesture();
    gesture.edit = scope_edit(scope, scope_name(project, ui, scope));
    gesture.key = key; gesture.scope = scope; gesture.last = now;
}

void Context::open_hold(unsigned pad) {
    if (hold.edit || ui.recording || ui.name.open() || ui.exit_open || ui.ask != UiState::Ask::None) return;
    auto scope = press_scope(project, ui, SurfaceButton::Count, true);
    if (scope.kind != UndoScope::Kind::Pattern) return;
    close_gesture();
    hold.edit = scope_edit(scope, scope_name(project, ui, scope));
    hold.scope = scope; hold.pad = pad; hold.pushes = edits.pushes;
}

void Context::close_hold(unsigned pad) {
    if (!hold.edit || hold.pad != pad) return;
    auto edit = std::move(hold.edit);
    // Something kept its own edit meanwhile (Erase + pad, a Shift + pad job) or a take began: that one stands.
    if (edits.pushes != hold.pushes || ui.recording) return;
    if (finish_scope(*edit)) { edits.push(std::move(*edit)); dirty = true; }
}

void Context::close_gesture() {
    if (!gesture.edit) return;
    auto edit = std::move(gesture.edit);
    if (finish_scope(*edit)) { edits.push(std::move(*edit)); dirty = true; }
}

void Context::restore_mix(const MixState& m) {
    for (unsigned t = 0; t < track_count; ++t) {
        auto& x = project.tracks[t].mix;
        if (x.level != m.level[t]) { x.level = m.level[t]; send({Type::Level, uint8_t(t), 0, 0, int16_t(x.level)}); }
        if (x.pan != m.pan[t]) { x.pan = m.pan[t]; send({Type::Pan, uint8_t(t), 0, 0, int16_t(x.pan)}); }
        for (unsigned b = 0; b < bus_count; ++b)
            if (x.sends[b] != m.sends[t][b]) { x.sends[b] = m.sends[t][b]; send({Type::Send, uint8_t(t), 0, uint16_t(b), int16_t(x.sends[b])}); }
    }
    for (unsigned b = 0; b < bus_count; ++b) if (project.buses[b].level != m.returns[b]) set_return(b, m.returns[b]);
}

void Context::restore_sound(unsigned t, const SoundState& s) {
    auto& tr = project.tracks[t % track_count];
    for (unsigned p = 0; p < max_params; ++p) if (tr.params[p] != s.params[p]) set_param(t, int(p), s.params[p]);
    for (unsigned r = 0; r < mod_routes; ++r) {
        if (same(tr.mods[r], s.mods[r])) continue;
        tr.mods[r] = s.mods[r];
        Command c {Type::Mod, uint8_t(t % track_count), 0, uint16_t(r)};
        c.route = s.mods[r];
        send(c);
    }
}

void Context::restore_master(const MasterState& m) {
    auto& k = project.knob_fx;
    if (k.kind != m.knob_kind) { k.kind = m.knob_kind; send({Type::KnobFx, 0, 0, 0, int16_t(k.kind)}); }
    k.choice = m.knob_choice;
    k.a = m.knob_a; k.b = m.knob_b; k.morph = m.knob_morph; // Knob FX C: the settings with the values heard between them
    for (unsigned q = 0; q < k.params.size(); ++q) {
        k.params[q] = m.knob_params[q];
        send({Type::KnobFx, 0, uint8_t(q), 1, k.params[q]});
    }
    if (k.bypass != m.knob_bypass) { k.bypass = m.knob_bypass; send_knob_state(); }
    const auto& c = m.compressor;
    set_compressor(0, c.on); set_compressor(1, c.color); set_compressor(2, c.attack); set_compressor(3, c.release);
    set_compressor(4, c.amount); set_compressor(5, c.boost); set_compressor(6, c.sidechain); set_compressor(7, c.key);
    set_compressor(8, c.key_pad == MasterComp::whole_kit ? -1 : c.key_pad);
    for (unsigned n = 0; n < 16; ++n)
        for (unsigned q = 0; q < 3; ++q)
            if (project.pad_fx_params[n][q] != m.pad_fx[n][q]) set_pad_fx_param(n, q, m.pad_fx[n][q]);
    set_flex(1, m.flex.mode); set_flex(2, m.flex.mix); set_flex(3, m.flex.quantize);
    if (project.punch_depth != m.punch_depth) {
        project.punch_depth = m.punch_depth;
        if (ui.engaged_count) send({Type::PunchDepth, 254, 0, 0, int16_t(m.punch_depth)});
    }
    project.punch_snap = m.punch_snap; project.punch_tail = m.punch_tail;
}

void Context::restore_chain(unsigned n, const Chain& value) {
    project.chain(n) = value;
    sync_chain(n);
}

void Context::restore_chop(const ChopState& c) {
    ui.slices = c.slices; ui.chop_mode = ChopMode(c.mode); ui.chop_slice = c.slice; ui.chop_amount = c.amount;
    ui.chop_sample = c.sample; ui.chop_from = c.from; ui.chop_to = c.to;
    ui.chop_armed = false;
}

PageEdit::PageEdit(Context& context, SurfaceButton button, bool pad) : c(context) {
    c.close_gesture(); // a pot's turns end with any press
    if (c.ui.recording || c.ui.name.open() || c.ui.exit_open || c.ui.ask != UiState::Ask::None) return;
    auto scope = press_scope(c.project, c.ui, button, pad);
    if (c.held(scope)) return; // part of the held pad's step
    edit = c.scope_edit(scope, scope_name(c.project, c.ui, scope));
    pushes = c.edits.pushes;
}

void PageEdit::finish() {
    if (!edit || c.edits.pushes != pushes || c.ui.recording) return; // the press kept its own edit (or began a take)
    if (c.finish_scope(*edit)) { c.edits.push(std::move(*edit)); c.dirty = true; }
}

} // namespace melodee
