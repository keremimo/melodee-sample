// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/context.h"
#include "melodee/shell/snapshot.h"
#include "melodee/ui/paint.h"

// A page: what it draws from a snapshot (screen thread) and what its pads,
// wheel, Push, soft keys and Minus / Plus do (MIDI thread). Pots go through
// the page's Bindings (shell/bindings.h). Each page lives in its own file.
namespace melodee {

struct PageSpec {
    void (*draw)(ui::Paint&, const Snapshot&);
    void (*pad)(Context&, unsigned pad, uint8_t velocity, bool down);
    void (*wheel)(Context&, int delta);
    void (*push)(Context&);
    void (*soft)(Context&, int key);  // B1-B3: 0-2
    void (*nudge)(Context&, int way); // Minus -1, Plus +1
};
const PageSpec& page_spec(PageId page);
// What the pads do on the page in view (Seq held, Pad Bank held and the like come first: shell/shell.cpp).
using PadJob = void (*)(Context&, unsigned pad, uint8_t velocity, bool down);
PadJob pad_job(const UiState& ui);

// Overlays drawn over any page. draw_overlays() draws whichever are open,
// after the page (the app and the previews both call it).
void draw_overlays(ui::Paint& p, const Snapshot& s);
void draw_track_overlay(ui::Paint& p, const Snapshot& s); // a track move or copy: the sixteen tracks
void draw_seq_sheet(ui::Paint& p, const Snapshot& s);     // Seq held a moment: the track's patterns (pages/seq_sheet.cpp)
void draw_repeat_keys(ui::Paint& p, const Snapshot& s);   // Note Repeat on: its rates on B1-B3
void draw_shift_layer(ui::Paint& p, const Snapshot& s);  // Shift held: the Shift + K cards in the knob strip
void draw_exit_sheet(ui::Paint& p, const Snapshot& s);
void draw_loading_sheet(ui::Paint& p, const Snapshot& s); // a project loading: built, then its samples coming in
void draw_ask_sheet(ui::Paint& p, const Snapshot& s);  // a question before an erase (shell/ask.cpp)
void draw_notice(ui::Paint& p, const Snapshot& s);     // copying a pad, or what an undo did
void draw_fader_header(ui::Paint& p, const Snapshot& s); // for a moment after the fader moves: what it drives, when no K card shows it
void draw_name_sheet(ui::Paint& p, const Snapshot& s); // while ui.name is open
void draw_sampler_map(ui::Paint& p, const Snapshot& s); // the sampler pages' map (Push), parity phase 10
void draw_place_bar(ui::Paint& p, const Snapshot& s);   // where the music is, last of all (shell/place_bar.h)

} // namespace melodee
