// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Roll (pages/roll_page.cpp), read from the pattern's events. The cursor is
// a step of Q (UiState::step_cursor, shared with Steps and Step Edit) and a
// pitch (UiState::roll_key); the event starting in that step on that key is
// in focus. The view scrolls with the cursor a step at a time, never by the
// page. The functions here are pure (tests/melodee.cpp); the ones that write
// the pattern take the Context.
namespace melodee {

class Context;

constexpr unsigned roll_columns = 16, roll_rows = 12;
constexpr unsigned roll_margin = 2; // steps kept in view beside the cursor

// 16 Levels on a kit track: one pad plays on all sixteen, each at a pitch of its own (Seq Record keeps them as that
// pad's key with the semitones in Event::pitch). Roll then has a row for each pitch, as a synth's roll, and shows
// only that pad's notes. `lane`: the pad's key, -1 for a roll of keys (a synth) or of pads (a kit).
inline int roll_lane(const Track& t) {
    return t.engine == EngineKind::Sampler && t.levels ? int(sampler_first_note + t.levels_pad % sampler_pads) : -1;
}
// The row a note sits on: its key, or with a lane its pitch over C4 (60, an instrument's root); -1 off the roll.
inline int roll_row(const Event& e, int lane) {
    if (lane < 0) return e.key;
    return int(e.key) == lane ? std::clamp(60 + int(e.pitch), 0, 127) : -1;
}

// The cursor's step, inside the pattern.
unsigned roll_cursor(const Pattern& pattern, const UiState& ui);
// The first step in view, inside the pattern.
unsigned roll_first(const Pattern& pattern, const UiState& ui);
// The note in focus: its index in pattern.events, or -1 when none starts in the cursor's step on roll_key.
int roll_focus(const Pattern& pattern, const UiState& ui, int lane = -1);
// The view onto the cursor: its step roll_margin steps from either edge, its key a row from the top or bottom.
void roll_follow(const Pattern& pattern, UiState& ui);
// Step Edit's selection (step_event) onto the note in focus, so its card, fader and erase functions act on it.
void roll_select(const Pattern& pattern, UiState& ui, int lane = -1);
// Roll opening on a pattern whose notes all lie outside the rows in view (a bass line in octave 2 under a cursor left
// on C4): the cursor's pitch goes to the note nearest its step in time, so the page opens on the notes. Left as it
// was when one is in view, or the pattern is empty.
void roll_open(const Pattern& pattern, UiState& ui, int lane = -1);
// The wheel, `way` detents: each to the next note of the chord in the cursor's step (up going on, down going back),
// past the last to the next step of Q, onto its lowest note going on and its highest going back; an empty step keeps
// the pitch. Stops at the pattern's ends.
void roll_step(const Pattern& pattern, UiState& ui, int way, int lane = -1);
// The selection (UiState::roll_marks). Marks count while they are of this track's pattern `pattern`.
inline bool roll_selection(const UiState& ui, unsigned pattern) {
    return ui.roll_marked && ui.roll_mark_track == ui.track && ui.roll_mark_pattern == pattern;
}
// Shift let go after marking: the marks act (the wheel moves them, and so on).
inline bool roll_selected(const UiState& ui, unsigned pattern) { return roll_selection(ui, pattern) && !ui.roll_marking; }
inline void roll_unmark(UiState& ui) { ui.roll_marked = 0; ui.roll_marking = false; }
// Event `n` is marked.
bool roll_is_marked(const Pattern& pattern, const UiState& ui, size_t n);
// The marked notes still in the pattern: their indexes in pattern.events, in order.
std::vector<size_t> roll_marked_events(const Pattern& pattern, const UiState& ui);
// Shift + wheel: `way` detents of the wheel's walk, the note in focus marked before the first and at each one, for
// track `track`'s pattern `pattern` (marks of another pattern are dropped first).
void roll_mark_walk(const Pattern& pattern, UiState& ui, int way, unsigned pattern_index, int lane = -1);
// The marked notes `way` steps of Q later (earlier), hopping over notes of the same row in their way; false when one
// would leave the pattern. The cursor goes with them.
bool roll_marks_move(Pattern& pattern, UiState& ui, int way);
// The marked notes `way` rows up (down), hopping over notes in their way; false when one would leave the rows
// (0..max_key, or with a lane -48..48 semitones). The cursor's pitch goes with them.
bool roll_marks_pitch(Pattern& pattern, UiState& ui, int way, int lane, int max_key, int hop = 1); // hop 12: by octaves
// The marked notes out of the pattern, in order; and the marks onto a list of notes (their new place).
std::vector<Event> roll_take_marked(Pattern& pattern, const UiState& ui);
void roll_remark(UiState& ui, const std::vector<Event>& events);
// The marked notes out of the pattern; how many went. The marks are cleared.
unsigned roll_marks_erase(Pattern& pattern, UiState& ui);
// Mute on the marked notes: all muted when any played, else all played again; true when they are muted now.
bool roll_marks_mute(Pattern& pattern, const UiState& ui);
// Note Repeat on the marked notes: their ratchet the next of 1, 2, 3, 4, 6, 8 after the first's; the hits set.
unsigned roll_marks_ratchet(Pattern& pattern, const UiState& ui);

// Minus / Plus: to the next note up (or down) starting in the cursor's step; past the last, a semitone.
void roll_pitch(const Pattern& pattern, UiState& ui, int way, int lane = -1);
// Where a tick sits as the MPC counts it: bar.beat.step of Q within the beat ("2.2.3"), 1-based.
std::string roll_position(uint32_t tick, unsigned bar_ticks, unsigned beat_ticks, unsigned q);

// A pad: `key` at the cursor, NEW NOTE long, or the note there focused if it is already written. With 16 Levels on a
// kit (roll_lane), the levels pad's key at `pitch` semitones, with the level's cutoff (-1 the pad's own).
void roll_write(Context& c, int key, uint8_t velocity, int pitch = 0, int cutoff = -1);
// Erase tapped alone, Delete on the sheet: the marked notes go, else the note in focus (one undo); false when none.
bool roll_erase(Context& c);
// The sheet: accent, slide or tie on the note in focus, on or off.
void roll_flag(Context& c, uint8_t flag);
// The selection's jobs with the Context: the wheel (moves; Shift: pitch, or marks while marking), Shift let go (marking
// ends; a tap clears the marks), Mute and Note Repeat pressed (false when there is no selection: their own job).
bool roll_selection_wheel(Context& c, int way);
void roll_shift_up(Context& c);
bool roll_selection_mute(Context& c);
bool roll_selection_repeat(Context& c);

} // namespace melodee
