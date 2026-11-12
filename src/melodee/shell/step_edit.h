// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/model/scales.h"
#include "melodee/shell/ui_state.h"
#include <algorithm>
#include <cstddef>
#include <string>

// Step Edit (Shift + Seq, the MPC's): one step of the selected track's
// pattern at a time, in steps of its Q, and the events starting in it. The
// selection is the step (UiState::step_cursor, shared with Steps and Roll)
// and which of its events (UiState::step_event). What the page's controls
// do to the selected event lives here, so the fader (shell/fader.cpp) and
// the pots reach it without going through the page.
namespace melodee {

class Context;

// The selected event's index in pattern.events, or -1 when the step holds none.
inline int selected_event(const Pattern& pattern, const UiState& ui) {
    auto in = step_events(pattern, unsigned(std::max<int>(ui.step_cursor, 0)) % pattern.steps());
    if (!in.count) return -1;
    return int(in.first + std::min<unsigned>(ui.step_event, in.count - 1));
}
// The key a pad stands for on a track: a kit pad of the bank in view, a 909
// drum, or the note a synth's pad plays; -1 for none. Inline: the pad
// lights (shell/pad_lights.cpp) use it in the host tests too.
inline int pad_key(const Project& project, const Track& t, unsigned pad) {
    if (t.engine == EngineKind::Sampler) return int(sampler_first_note + t.bank % sampler_banks * 16 + pad % 16);
    if (t.engine == EngineKind::Kit909) return pad < 11 ? int(36 + pad) : -1;
    return t.used() ? pad_note(project, t, pad).key : -1;
}
// What a key is called on a track: its kit pad (A03) or its note (D2).
std::string key_label(const Track& track, int key);
// The event of `key` in the step under the cursor becomes the selection; false when there is none.
bool select_key(Context& c, int key);

// The wheel: to the next or previous step of Q (the first event selected).
void step_move(Context& c, int way);
// Minus / Plus: through the step's events.
void event_move(Context& c, int way);
// The fader: the selected event earlier or later by `ticks`, inside the
// pattern; the cursor follows it into another step.
void nudge_event(Context& c, int ticks);
// The cards: velocity (K3) and length in steps of Q (Shift + K1).
void set_event_velocity(Context& c, int velocity);
void set_event_length(Context& c, int steps);
void set_event_chance(Context& c, int chance); // Roll's K3, percent
// Ratchets (roadmap P1): Roll's Shift + K1 / K2 and Step Edit's TRIG row: hits 1..8, the velocity ramp
// (-100..100 %), a kit pad's pitch ramp (semitones).
void set_event_ratchet(Context& c, int hits);
void set_event_ramp(Context& c, int ramp);
void set_event_pitch_ramp(Context& c, int semitones);
void set_event_condition(Context& c, int condition); // a trig condition (model/trig.h)
// B3: the selected event goes (one undo, or part of a take).
void erase_event(Context& c);
// Step entry (round 2, step-edit2 C; pages/step_edit_page.cpp). A pad down writes its key at the caret
// (UiState::step_cursor) with the entry's length, velocity and chance, heard as it goes in (a chord: each pad held,
// one undo for the step); when the last pad held lets go the caret moves on a step of Q. REST moves it on without
// writing; ERASE steps back and takes that step out; the wheel moves it. All wrap round the pattern.
void entry_pad(Context& c, unsigned pad, uint8_t velocity, bool down);
void entry_rest(Context& c);
void entry_back(Context& c);
void entry_move(Context& c, int way);
// Roll's Shift + K1 NUDGE: the note in focus `offset` ticks off its nearest grid line of Q (bindings.h nudge_card()),
// the cursor following it so it stays in focus.
void set_event_nudge(Context& c, int offset);
// Erase + pad, once B3 says so: every event of `key` in the pattern (one undo, or part of a take).
void erase_key_events(Context& c, int key);

} // namespace melodee
