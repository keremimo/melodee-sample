// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include "melodee/ui/shades.h"
#include "surface/controls.h"
#include <array>

// Track colour (issue 69). A track shows its place's colour (track n the nth of the sixteen, ui/shades.h) until one
// is picked for it (Track::colour, model/project.h). The mixer's MENU (encoder held) TRACK COLOUR opens the sheet
// over the page, for the selected track:
//   pads       the sixteen colours as the pads sit (pad 1 Coral ... pad 16 Grey); a pad picks one
//   wheel      the next or previous colour
//   B1 BACK    the colour it had, and the sheet closes
//   B2 DEFAULT its place's colour again (it then follows the track's place, as before a colour was picked)
//   B3 DONE    keeps it (one undo step); Push the same
// The same sheet sets the sequence in hand's colour (Scene::colour) from Sequences' MENU (COLOUR) or Patterns'
// (SEQUENCE COLOUR): B2 NONE in place of DEFAULT (the sequence white again); DONE one undo step for the sequence.
// A colour picked shows at once wherever the track's colour does (the screen, pads, fader light, a plug-in's
// companion screen, the Live Set export). MIDI thread, but show_track_colours() any thread that owns the project.
namespace melodee {

class Context;

// The sixteen tracks' colours into ui::track_slot_table(), as the project has them now.
inline void show_track_colours(const Project& project) {
    for (unsigned t = 0; t < track_count; ++t) ui::set_track_slot(t, track_colour_slot(project.tracks[t], t));
}

// The Live Set export's colours (shell/live_export.h): each track's, picked or its place's, as Live's nearest.
inline std::array<uint8_t, track_count> live_track_colours(const Project& project) {
    std::array<uint8_t, track_count> out {};
    for (unsigned t = 0; t < track_count; ++t) out[t] = ui::track_live_colours[track_colour_slot(project.tracks[t], t)];
    return out;
}

// Which of the sixteen comes `way` steps after `slot` (round).
inline unsigned colour_slot_step(unsigned slot, int way) { return unsigned((int(slot % 16) + (way % 16) + 16) % 16); }

// A sequence's colour (Scene::colour: 0 none, 1-16) `way` steps on: from none, Coral up or Grey down; then round the sixteen.
inline uint8_t sequence_colour_step(uint8_t colour, int way) {
    if (!way) return colour;
    return uint8_t(1 + (colour ? colour_slot_step(colour - 1u, way) : colour_slot_step(way > 0 ? 15 : 0, way)));
}

// The sheet's colour now, one of the sixteen; -1 a sequence with none.
inline int colour_sheet_slot(const Project& project, const UiState& ui) {
    if (ui.colour_seq) {
        const uint8_t colour = project.scenes[ui.colour_scene % pattern_count].colour;
        return colour ? int((colour - 1u) % 16u) : -1;
    }
    const unsigned t = ui.colour_track % track_count;
    return int(track_colour_slot(project.tracks[t], t));
}

void track_colour_open(Context& c);                          // the MENU's TRACK COLOUR
void sequence_colour_open(Context& c);                       // the MENU's COLOUR on Sequences / SEQUENCE COLOUR on Patterns
bool track_colour_button(Context& c, SurfaceButton button);  // while it is open: true when the button was its
void track_colour_wheel(Context& c, int delta);
void track_colour_pad(Context& c, unsigned pad);

} // namespace melodee
