// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include "melodee/shell/ui_state.h"
#include <cstdint>
#include <string>

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

// The cursor's step, inside the pattern.
unsigned roll_cursor(const Pattern& pattern, const UiState& ui);
// The first step in view, inside the pattern.
unsigned roll_first(const Pattern& pattern, const UiState& ui);
// The note in focus: its index in pattern.events, or -1 when none starts in the cursor's step on roll_key.
int roll_focus(const Pattern& pattern, const UiState& ui);
// The view onto the cursor: its step roll_margin steps from either edge, its key a row from the top or bottom.
void roll_follow(const Pattern& pattern, UiState& ui);
// Step Edit's selection (step_event) onto the note in focus, so its card, fader and erase functions act on it.
void roll_select(const Pattern& pattern, UiState& ui);
// The wheel: the cursor a step of Q on; where notes start, the pitch goes to the nearest of them.
void roll_step(const Pattern& pattern, UiState& ui, int way);
// Shift + wheel: to the next (or previous) step where a note starts, on the nearest of its notes; false when none.
bool roll_jump(const Pattern& pattern, UiState& ui, int way);
// Minus / Plus: to the next note up (or down) starting in the cursor's step; past the last, a semitone.
void roll_pitch(const Pattern& pattern, UiState& ui, int way);
// Where a tick sits as the MPC counts it: bar.beat.step of Q within the beat ("2.2.3"), 1-based.
std::string roll_position(uint32_t tick, unsigned bar_ticks, unsigned beat_ticks, unsigned q);

// A pad: `key` at the cursor, NEW NOTE long, or the note there focused if it is already written.
void roll_write(Context& c, int key, uint8_t velocity);
// Erase tapped alone, Delete on the sheet: the note in focus goes (one undo); false when there is none.
bool roll_erase(Context& c);
// The sheet: accent, slide or tie on the note in focus, on or off.
void roll_flag(Context& c, uint8_t flag);

} // namespace melodee
