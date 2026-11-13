// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once

// The printed buttons on the note editors (Roll, Steps, Step Edit), as Song gives them to its clips. Each acts on
// Roll's marked notes, else the note in hand (Roll's in focus, Steps' step on its lane, Step Edit's event), one undo:
//   Chop          split each note in two at the grid line nearest its middle
//   Shift + Chop  legato: each note held until the next starts
//   Shift + Loop  reverse (Roll's marks, else the notes in view)
//   Shift + Note Repeat  arp to notes: each chord turned into what the track's arp plays over it
//   Loop          the track's pattern plays only the bars in view (Roll's marks' bars) while the editor is open
// Each returns false off the editors, where the button keeps its own job. model/note_edits.h does the notes.
namespace melodee {

class Context;

bool note_editor_open(Context& c);
bool note_chop(Context& c);
bool note_legato(Context& c);
bool note_reverse(Context& c);
bool note_arp(Context& c);
bool edit_loop_tap(Context& c);
void edit_loop_check(Context& c); // each frame: the loop ends once the editor, track or pattern changes
void edit_loop_off(Context& c);

// Roll's marked notes (shell/roll.h): Push while they are copied pastes them at the cursor, Shift + K2 their velocity
// ramp, Erase + wheel pulls them toward the grid, − / + an octave.
bool roll_paste(Context& c);
void roll_marks_ramp(Context& c, int percent);
bool roll_marks_quantize(Context& c, int way);
bool roll_marks_octave(Context& c, int way);

} // namespace melodee
