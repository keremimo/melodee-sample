// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/erase_copy.h"
#include "melodee/shell/motion_points.h"
#include "melodee/shell/delete_jobs.h"
#include "melodee/engine/engine_host.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/context.h"
#include "melodee/shell/mod_jobs.h"
#include "melodee/shell/note_copy.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/record_place.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/step_lane.h"
#include "melodee/shell/track_copy.h"
#include "melodee/model/scene_links.h"
#include <algorithm>
#include <string>

namespace melodee {

namespace {
std::string pattern_name(unsigned n) { return "P" + std::to_string(n % pattern_count + 1); }
std::string step_name(unsigned s) { return "Step " + std::to_string(s + 1); }
bool key_has_events(const Pattern& p, int key) {
    return std::any_of(p.events.begin(), p.events.end(), [&](const Event& e) { return e.key == key; });
}

// Steps: the step under the cursor (on a kit, the last pad's lane).
bool erase_step(Context& c) {
    auto& ui = c.ui;
    if (ui.sheet_open) return false;
    auto& p = c.pattern();
    unsigned st = edit_caret(ui, p); // a spare step (shell/spare_bar.h) is empty
    int lane = steps_lane(c.track());
    if (in_spare(ui, p) || !step_has_notes(p, st, lane)) { c.notice(step_name(st) + " is empty"); return true; }
    std::string what = "Erase " + step_name(st);
    c.edit_pattern(ui.track, c.track().pattern, what.c_str(), [&] {
        if (lane < 0) c.set_step(ui.track, st, Step {});
        else if (clear_lane(c.pattern(), st, lane)) c.sync_pattern(ui.track, c.track().pattern);
    });
    c.notice(what);
    return true;
}

// Chop: the slice in hand joins its neighbour, as Erase + its pad (Context::chop_erase); a new chop leaves its cut out.
bool erase_slice(Context& c) {
    if (!c.ui.slices.count) return false;
    if (c.ui.slices.count == 1) return true; // one slice: nothing to join
    const unsigned slice = c.ui.chop_slice;
    c.chop_change(slice ? slice - 1 : 0, [&c, slice] { c.chop_erase(slice); }); // slice hits from there on: asked first (issue 24)
    return true;
}
} // namespace

void ask_clear_track(Context& c) {
    const unsigned t = c.ui.track % track_count;
    if (c.ui.recording) c.notice("Clear track: not in a take");
    else if (!track_clearable(c.project.tracks[t], c.patterns.at[t])) c.notice("Track " + std::to_string(t + 1) + " is empty");
    else ask(c, UiState::Ask::ClearTrack, int(t));
}

bool erase_tap(Context& c) {
    auto& ui = c.ui;
    if (ui.ask != UiState::Ask::None || ui.name.open() || ui.exit_open || ui.seq_held || ui.pad_bank || ui.mute_mode()) return false;
    if (c.plugin_screen_key(ScreenKey::Erase)) return true; // a plug-in's own page that takes Erase (melodee_screen.h)
    unsigned t = ui.track % track_count;
    switch (ui.page) {
    case PageId::Roll: return !ui.sheet_open && roll_erase(c);
    case PageId::StepEdit:
        if (ui.step_pads) return erase_step(c); // the pads on the steps: the caret's step, as Steps had it
        if (selected_event(c.pattern(), ui) < 0) return false;
        erase_event(c);
        return true;
    case PageId::Patterns: {
        if (ui.patterns_cards == 1) { // SEQUENCE: the sequence in hand (one undo)
            c.clear_scene(c.project.scene % pattern_count);
            return true;
        }
        unsigned n = c.track().pattern % pattern_count;
        if (c.patterns.at[t][n].empty()) c.notice(pattern_name(n) + " is empty");
        else ask(c, UiState::Ask::ClearPattern, int(n));
        return true;
    }
    case PageId::Scenes: c.clear_scene(c.project.scene % pattern_count); return true; // the sequence in hand (one undo)
    case PageId::Mixer: return mixer_fx_lit(ui) && chain_erase(c); // its FX block: the lit effect out
    case PageId::Looper: {
        unsigned loop = ui.loop % loop_tracks;
        if (c.host.looper().view(loop).state == Looper::State::Empty) c.notice("L" + std::to_string(loop + 1) + " is empty");
        else ask(c, UiState::Ask::ClearLoop, int(loop));
        return true;
    }
    case PageId::Rack: case PageId::Bus: case PageId::Master: case PageId::PadFx: return chain_erase(c);
    case PageId::Chop: return erase_slice(c);
    case PageId::LayersPage: c.layer_clear(); return true;
    case PageId::ModPage: mod_remove(c); return true;
    case PageId::Motion: return ui.motion_draw && (motion_point_erase(c), true); // drawing: the point under the cursor
    case PageId::Project: return project_erase(c);
    case PageId::Samples: // the trim sheet up: both its ends back; else the file goes from the card
        if (c.ui.browse.up()) { c.browse_erase(-1); return true; }
        return samples_erase(c);
    case PageId::Plugins: return plugins_erase(c);
    default: return false;
    }
}

void copy_press(Context& c, bool kit_pads, bool key_pads) {
    auto& ui = c.ui;
    auto arm = [&](UiState::Copying kind) { ui.stop_copying(); ui.copying = kind; ui.copy_from = -1; };
    if (ui.page == PageId::Roll && roll_selected(ui, c.track().pattern)) { // Roll's marked notes: Push pastes them at the cursor
        arm(UiState::Copying::Notes);
        ui.sheet_open = false;
        c.notice("Copy: Push pastes at the cursor");
        return;
    }
    switch (copy_here(c.project, ui, kit_pads, key_pads)) {
    case CopyHere::Tracks: start_track_pick(ui, false); return;
    case CopyHere::Patterns: arm(UiState::Copying::Patterns); return;
    case CopyHere::Pads: arm(UiState::Copying::Pads); return;
    case CopyHere::Keys: arm(UiState::Copying::Keys); ui.sheet_open = false; return;
    case CopyHere::Steps: arm(UiState::Copying::Steps); ui.sheet_open = false; return;
    case CopyHere::Scenes: arm(UiState::Copying::Scenes); return;
    case CopyHere::Clip: song_copy(c); return;
    case CopyHere::Project: if (!project_copy(c)) c.notice("Copy: pick a project"); return;
    case CopyHere::Effect: chain_copy(c); return;
    case CopyHere::Slice: c.extract_slice(); return;
    case CopyHere::None: break;
    }
    c.notice("Nothing to copy here");
}

void key_copy_pad(Context& c, unsigned pad) {
    auto& ui = c.ui;
    const auto& t = c.track();
    int key = pad_key(c.project, t, pad);
    if (key < 0) return;
    if (ui.copy_from < 0) { // first the key to copy
        if (!key_has_events(c.pattern(), key)) { c.notice("No notes on " + key_label(t, key)); return; }
        ui.copy_from = int16_t(key);
        return;
    }
    int from = ui.copy_from;
    if (from == key) return; // then where its notes go
    std::string what = "Copy " + key_label(t, from) + " to " + key_label(t, key);
    bool all = true;
    c.edit_pattern(ui.track, t.pattern, what.c_str(), [&] {
        all = copy_key_events(c.pattern(), from, key);
        c.sync_pattern(ui.track, c.track().pattern);
    });
    c.notice(all ? what : std::string("The pattern is full"));
    ui.stop_copying();
}

void step_copy_pad(Context& c, unsigned pad) {
    auto& ui = c.ui;
    auto& p = c.pattern();
    unsigned st = unsigned(ui.steps_bar) * bar_steps + pad % 16;
    if (st >= p.steps()) return;
    int lane = steps_lane(c.track());
    if (ui.copy_from < 0) { // first the step to copy
        if (!step_has_notes(p, st, lane)) { c.notice(step_name(st) + " is empty"); return; }
        ui.copy_from = int16_t(st);
        ui.step_cursor = int16_t(st);
        return;
    }
    unsigned from = unsigned(ui.copy_from);
    if (from == st) return; // then where
    std::string what = "Copy " + step_name(from) + " to " + std::to_string(st + 1);
    bool all = true;
    c.edit_pattern(ui.track, c.track().pattern, what.c_str(), [&] {
        all = copy_step_notes(c.pattern(), from, st, lane);
        c.sync_pattern(ui.track, c.track().pattern);
    });
    ui.step_cursor = int16_t(st);
    c.notice(all ? what : std::string("The pattern is full"));
    ui.stop_copying();
}

void scene_copy_pad(Context& c, unsigned pad) {
    auto& ui = c.ui;
    const unsigned n = ui.pad_pattern(pad); // in the bank in view: Pad Bank between the two takes it to another
    if (ui.copy_from < 0) { // first the sequence to copy
        if (c.project.scenes[n].plain() && !scene_kept(c.project, n)) { c.notice("SEQ " + std::to_string(n + 1) + " is empty"); return; }
        ui.copy_from = int16_t(n);
        return;
    }
    if (unsigned(ui.copy_from) == n) return; // then where
    c.copy_scene(unsigned(ui.copy_from), n);
    ui.stop_copying();
}

bool erase_selected_pad(Context& c) {
    const auto& t = c.track();
    if (t.engine != EngineKind::Sampler || !t.used()) { c.notice("Select a kit pad"); return false; }
    unsigned kit = (t.levels ? t.levels_pad : t.last_pad) % sampler_pads;
    int key = int(sampler_first_note + kit);
    if (!key_has_events(c.pattern(), key)) { c.notice("No events in this pad"); return false; }
    ask(c, UiState::Ask::EraseKeyEvents, key);
    return true;
}

// Erase + kit pad while playing. Its notes go from where the press was heard on, as the playhead passes them, a
// little ahead of it (live_erase_tick) so a note the frame would reach is not heard. Let go of either, it stops.
void Context::live_erase_press(unsigned pad, unsigned kit) {
    pad %= 16;
    const unsigned t = ui.track % track_count, p = track().pattern % pattern_count;
    if (live_erase.pads && (live_erase.track != t || live_erase.pattern != p)) live_erase_end();
    if (!live_erase.pads) {
        live_erase = {};
        live_erase.track = uint8_t(t); live_erase.pattern = uint8_t(p);
        if (!in_take(t, p)) live_erase.edit = std::make_unique<Edit>(begin_edit("Erase notes", t, int(p), false));
        int position = 0;
        uint32_t ticks = 0;
        if (!host.heard_place(t, hit_ns, position, ticks)) position = host.position_of(t);
        live_erase.from = position < 0 ? -1 : int(wrap_tick(position, patterns.at[t][p].length));
    }
    live_erase.pads = uint16_t(live_erase.pads | 1u << pad);
    live_erase.keys[pad] = int16_t(sampler_first_note + kit % sampler_pads);
    notice(pad_text("Erase", kit % sampler_pads) + " notes");
    live_erase_tick();
}

void Context::live_erase_release(unsigned pad) {
    if (!(live_erase.pads & 1u << (pad % 16))) return;
    live_erase.pads = uint16_t(live_erase.pads & ~(1u << (pad % 16)));
    if (!live_erase.pads) live_erase_end();
}

void Context::live_erase_end() {
    auto edit = std::move(live_erase.edit);
    live_erase.pads = 0;
    if (edit) commit_edit(std::move(*edit));
    else if (live_erase.found) dirty = true; // part of the take
}

void Context::live_erase_tick() {
    auto& le = live_erase;
    if (!le.pads) return;
    if (!ui.erase || !host.playing() || ui.track % track_count != le.track || track().pattern % pattern_count != le.pattern) {
        live_erase_end();
        return;
    }
    auto& p = patterns.at[le.track][le.pattern];
    const int now = host.position_of(le.track);
    if (now < 0 || p.length == 0) return;
    // A little past the playhead: what the audio thread reaches before the next frame looks again (40 ms).
    const Speed sp = speed_of(track().speed);
    const uint64_t ahead = std::max<uint64_t>(2, (uint64_t(tempo_now()) * ticks_per_quarter * sp.num * 4 + 60000u * sp.den - 1) / (60000u * sp.den));
    const uint32_t to = wrap_tick(int(uint64_t(now) + ahead), p.length);
    const uint32_t from = le.from < 0 ? uint32_t(now) % p.length : uint32_t(le.from);
    le.from = int(to);
    if (from == to) return;
    auto passed = [&](uint32_t tick) { return from < to ? tick >= from && tick < to : tick >= from || tick < to; };
    auto held = [&](int key) {
        for (unsigned n = 0; n < 16; ++n)
            if (le.pads & 1u << n && le.keys[n] == key) return true;
        return false;
    };
    auto before = p.events.size();
    p.events.erase(std::remove_if(p.events.begin(), p.events.end(), [&](const Event& e) { return held(e.key) && passed(e.tick); }), p.events.end());
    if (p.events.size() == before) return;
    le.found = true;
    sync_pattern(le.track, le.pattern);
}

void key_erase_pad(Context& c, unsigned pad) {
    const auto& t = c.track();
    int key = pad_key(c.project, t, pad);
    if (key < 0) return;
    if (!key_has_events(c.pattern(), key)) { c.notice("No notes on " + key_label(t, key)); return; }
    ask(c, UiState::Ask::EraseKeyEvents, key);
}

} // namespace melodee
