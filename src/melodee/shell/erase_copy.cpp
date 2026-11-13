// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/erase_copy.h"
#include "melodee/engine/engine_host.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/context.h"
#include "melodee/shell/mod_jobs.h"
#include "melodee/shell/note_copy.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/step_lane.h"
#include "melodee/shell/track_copy.h"
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
    unsigned st = unsigned(std::max<int>(ui.step_cursor, 0)) % p.steps();
    int lane = steps_lane(c.track());
    if (!step_has_notes(p, st, lane)) { c.notice(step_name(st) + " is empty"); return true; }
    std::string what = "Erase " + step_name(st);
    c.edit_pattern(ui.track, c.track().pattern, what.c_str(), [&] {
        if (lane < 0) c.set_step(ui.track, st, Step {});
        else if (clear_lane(c.pattern(), st, lane)) c.sync_pattern(ui.track, c.track().pattern);
    });
    c.notice(what);
    return true;
}

// Chop: the slice in hand joins the one before (the chop is Manual from here), as Erase + its pad.
bool erase_slice(Context& c) {
    auto& ui = c.ui;
    if (!ui.slices.count) return false;
    if (ui.chop_slice == 0) { c.notice("The first slice stays"); return true; }
    ui.slices.remove(ui.chop_slice);
    ui.chop_slice = uint8_t(ui.chop_slice - 1);
    ui.chop_mode = ChopMode::Manual; ui.chop_armed = false;
    return true;
}
} // namespace

bool erase_tap(Context& c) {
    auto& ui = c.ui;
    if (ui.ask != UiState::Ask::None || ui.name.open() || ui.exit_open || ui.seq_held || ui.pad_bank || ui.mute_mode()) return false;
    unsigned t = ui.track % track_count;
    switch (ui.page) {
    case PageId::Roll: return !ui.sheet_open && roll_erase(c);
    case PageId::StepEdit:
        if (ui.step_pads) return erase_step(c); // the pads on the steps: the caret's step, as Steps had it
        if (selected_event(c.pattern(), ui) < 0) return false;
        erase_event(c);
        return true;
    case PageId::Patterns: {
        if (ui.patterns_cards == 1) { // SCENE: the cursor's scene's name and tempo
            c.clear_scene(ui.pattern_bank % pattern_banks * pattern_pads + ui.scene % pattern_pads);
            return true;
        }
        unsigned n = c.track().pattern % pattern_count;
        if (c.patterns.at[t][n].empty()) c.notice(pattern_name(n) + " is empty");
        else ask(c, UiState::Ask::ClearPattern, int(n));
        return true;
    }
    case PageId::Tracks:
        if (ui.recording) c.notice("Clear track: not in a take");
        else if (!track_clearable(c.project.tracks[t], c.patterns.at[t])) c.notice("Track " + std::to_string(t + 1) + " is empty");
        else ask(c, UiState::Ask::ClearTrack, int(t));
        return true;
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
    case PageId::Project: return project_erase(c);
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

void key_erase_pad(Context& c, unsigned pad) {
    const auto& t = c.track();
    int key = pad_key(c.project, t, pad);
    if (key < 0) return;
    if (!key_has_events(c.pattern(), key)) { c.notice("No notes on " + key_label(t, key)); return; }
    ask(c, UiState::Ask::EraseKeyEvents, key);
}

} // namespace melodee
