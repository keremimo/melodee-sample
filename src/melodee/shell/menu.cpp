// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The MENU's jobs on the MIDI thread (shell/menu.h has its rules): opening it, its keys, the wheel and the pads,
// the sixteen places of GO and the cards of HERE. Every job calls what its printed key or shortcut calls.
#include "melodee/shell/menu.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/card_access.h"
#include "melodee/shell/context.h"
#include "melodee/shell/lesson_jobs.h"
#include "melodee/shell/levels_keys.h"
#include "melodee/shell/mod_jobs.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/sample_mode.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/track_copy.h"

namespace melodee {
namespace {

unsigned here_count(const Context& c) {
    std::array<MenuItem, menu_here_max> items {};
    return menu_here(c.project, c.ui, items);
}

// GO: a place opens as its own key opens it.
void go(Context& c, unsigned tile) {
    auto& ui = c.ui;
    const auto& track = c.track();
    if (!menu_tile_shown(ui, tile)) return; // a place Basic leaves out: its pad does nothing, the MENU stays
    if (ui.lesson) { if (tile % 16 == 15) lesson_leave(c, false); return; } // a lesson: LEAVE only
    if (tile % 16 == 15) { ui.menu = menu_learn_view; ui.menu_row = 0; return; } // LEARN: the lessons
    ui.menu = menu_closed;
    switch (tile % 16) {
    case 0: if (ui.page != PageId::Stage) c.open(PageId::Stage); break;
    case 1: { // Sample Select: a kit's or a keygroup's files, else the track's sounds (a plug-in track: the picker)
        bool files = track.sampled();
        c.open(files ? PageId::Samples : PageId::Sounds);
        if (files && c.library) c.library->rescan();
        break;
    }
    case 2: // Sample Record: a kit's Record page, an audio track's INPUT, any other track's PRINT sheet
        if (track.is_audio()) { stage_go(ui, 1, 0); if (ui.page != PageId::Stage) c.open(PageId::Stage); }
        else if (track.engine != EngineKind::Sampler) c.open_print_sheet();
        else if (ui.page != PageId::Record) c.open(PageId::Record);
        break;
    case 3: if (ui.page != PageId::Chop) chop_button(c); break; // Chop on the last pad's sample (it says why not)
    case 4: if (ui.page != seq_home) c.open(seq_home); break; // Sequences, as Seq
    case 5: c.open(PageId::Roll); break;
    case 6: // Step Edit, as Shift + Seq
        if (ui.page == PageId::StepEdit) break;
        if (ui.page == PageId::Roll) roll_select(c.pattern(), ui, roll_lane(track)); // the note in focus is the event chosen
        c.open(PageId::StepEdit);
        break;
    case 7: c.open(PageId::Motion); break;
    case 8: c.open(PageId::Mixer); break;
    case 10: ui.fx_loop = -1; ui.fx_sub = -1; c.open(PageId::Rack); break; // the selected track's chain
    case 11: c.open(PageId::Looper); break;
    case 12: c.open(PageId::Song); break;
    case 13: // Project, as Shift + Pad 16
        if (ui.page == PageId::Project) break;
        ui.project_list = false; ui.sheet_open = false; ui.move_open = false; ui.project_pick = 0;
        c.open(PageId::Project);
        if (c.store) c.store->list(c.project_folder);
        ui.project_rows = 64; // until the list arrives
        break;
    case 14: // Settings, as Shift + Pad 8
        if (ui.page == PageId::SettingsPage) break;
        ui.settings_in = false; ui.settings_look = false;
        c.open(PageId::SettingsPage);
        break;
    default: break; // LEARN, above
    }
}

// HERE: a card does what its shortcut does.
void run(Context& c, MenuJob job) {
    using J = MenuJob;
    auto& ui = c.ui;
    ui.menu = menu_closed;
    const unsigned column = c.track().pattern % pattern_count; // the selected track's pattern
    const unsigned scene = c.project.scene % pattern_count;    // the sequence in hand
    switch (job) {
    case J::SequenceCapture: ui.scene_capture = true; break; // the next pad picks the sequence
    case J::SequenceName: c.name_scene(scene); break;
    case J::SequenceFollowAll: c.follow_to_column(); break;
    case J::SequenceLaunch: c.launch_scene(scene); break; // every track back on what the sequence keeps
    case J::SequenceClear: c.clear_scene(scene); break;
    case J::PatternDouble: c.double_length(); break;
    case J::PatternHalf: c.half_length(); break;
    case J::PatternClear: ask(c, UiState::Ask::ClearPattern, int(column)); break; // asked first, as Seq + Erase + pad
    case J::TrackName: c.name_track(ui.track); break;
    case J::TrackMove: start_track_pick(ui, true); break;  // then the track, then where
    case J::TrackCopy: start_track_pick(ui, false); break;
    case J::TrackClear: { // asked first, as Pad Bank + Erase + pad
        unsigned t = ui.track % track_count;
        if (ui.recording) c.notice("Clear track: not in a take");
        else if (!track_clearable(c.project.tracks[t], c.patterns.at[t])) c.notice("Track " + std::to_string(t + 1) + " is empty");
        else ask(c, UiState::Ask::ClearTrack, int(t));
        break;
    }
    case J::Looper: c.open(PageId::Looper); break;
    case J::Freeze: c.open_print_sheet(); break;
    case J::Stems: c.open_stems_sheet(); break;
    case J::ModAdd: mod_add(c); break;
    case J::Chords: levels_chords_on_now(c); break; // as 16 Levels held + B2
    case J::ChordFile: // as 16 Levels held + B2 FILE, with Chords on
        levels_chords_on_now(c);
        if (levels_chords_on(c.track())) levels_key(c, 1);
        break;
    default: break;
    }
}

} // namespace

void menu_open(Context& c) {
    auto& ui = c.ui;
    if (ui.menu || ui.ask != UiState::Ask::None || ui.name.open() || ui.exit_open || ui.loading || card_owns_controls(ui) || ui.levels_sheet) return;
    ui.stop_copying(); // a copy or a move waiting for its pads gives way
    ui.scene_capture = false;
    ui.history = false; // the undo list gives way
    ui.menu = here_count(c) ? menu_here_view : menu_go_view;
    ui.menu_row = 0;
    ui.menu_tile = uint8_t(menu_tile_of(ui.page));
    if (!menu_tile_shown(ui, ui.menu_tile)) ui.menu_tile = 0;
    if (ui.lesson) { ui.menu = menu_go_view; ui.menu_tile = 15; } // a lesson: GO, on LEAVE
}

bool menu_button(Context& c, SurfaceButton b) {
    using B = SurfaceButton;
    auto& ui = c.ui;
    if (!ui.menu) return false;
    if (ui.menu == menu_learn_view) { // LEARN: B1 back to GO, B3 or Push starts the lesson in hand
        if (b == B::TopLeft) ui.menu = menu_go_view;
        else if (b == B::TopRight || b == B::WheelPush) lesson_start(c, ui.menu_row % lesson_count);
        else if (b != B::TopMiddle && b != B::Play && b != B::Stop && b != B::Shift) { ui.menu = menu_closed; return false; }
        return b != B::Play && b != B::Stop && b != B::Shift;
    }
    if (ui.lesson) { // a lesson: BACK, and Push on LEAVE
        if (b == B::TopLeft) ui.menu = menu_closed;
        else if (b == B::WheelPush) go(c, ui.menu_tile);
        return true;
    }
    switch (b) {
    case B::TopLeft: ui.menu = menu_closed; return true;                               // BACK
    case B::TopMiddle: if (here_count(c)) ui.menu = menu_here_view; return true;       // HERE, when the page lists something
    case B::TopRight: ui.menu = menu_go_view; return true;                             // GO
    case B::WheelPush:
        if (ui.menu == menu_go_view) { c.usage.menu_go(c.now, ui.menu_tile); go(c, ui.menu_tile); return true; }
        {
            std::array<MenuItem, menu_here_max> items {};
            unsigned n = menu_here(c.project, ui, items);
            if (n) { c.usage.menu_here(c.now, items[ui.menu_row % n].name); run(c, items[ui.menu_row % n].job); }
            else ui.menu = menu_closed;
        }
        return true;
    case B::Play: case B::Stop: case B::Shift: return false; // the transport and Shift, with the MENU left open
    default: ui.menu = menu_closed; return false;             // any other button: its own job, the MENU closed
    }
}

void menu_wheel(Context& c, int delta) {
    auto& ui = c.ui;
    if (ui.menu == menu_learn_view) { ui.menu_row = uint8_t(std::clamp(int(ui.menu_row % lesson_count) + delta, 0, int(lesson_count) - 1)); return; }
    if (ui.menu == menu_go_view) { ui.menu_tile = uint8_t(menu_tile_turn(ui, ui.menu_tile, delta)); return; }
    unsigned n = here_count(c);
    if (n) ui.menu_row = uint8_t(std::clamp(int(ui.menu_row % n) + delta, 0, int(n) - 1));
}

void menu_pad(Context& c, unsigned pad) {
    if (c.ui.menu == menu_go_view) { c.usage.menu_go(c.now, pad); go(c, pad % 16); } // HERE: the pads wait
}

} // namespace melodee
