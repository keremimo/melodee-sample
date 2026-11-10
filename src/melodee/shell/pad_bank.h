// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// Pad Bank (MELODEE_PARITY.md, phase 1c). A tap steps the selected kit to
// its next bank, A-H round (Shift: the previous one; on Punch the other bank
// of punches). Held: a pad picks that track, Minus / Plus or the wheel jump
// banks, and the screen shows the sixteen tracks; Shift + Erase then copies a
// whole track (shell/track_copy.cpp). Tapped twice, as Note Repeat's double
// tap opens Arp: the Tracks screen latches (pages/tracks_page.cpp), and the
// first tap's bank step on a kit is undone; a tap there goes back to Stage.
// MIDI thread.
namespace melodee {

class Context;
constexpr uint32_t pad_bank_double_ms = 450; // two taps this close: the Tracks screen

void pad_bank_button(Context& c, bool pressed);
// Minus / Plus or the wheel while Pad Bank is held: the bank jumps. False when it is not held.
bool pad_bank_move(Context& c, int way);
// The selected kit's bank, `way` banks on (round A-H).
void step_bank(Context& c, int way);
// Whether the pads are the sixteen tracks now: Pad Bank held, or a track copy under way.
inline bool tracks_on_pads(const UiState& ui) { return ui.pad_bank || ui.copying == UiState::Copying::Tracks; }
// A pad while tracks_on_pads(): picks that track, or the copy's track and
// then where it goes. False when the pads are not the tracks.
bool track_pad(Context& c, unsigned pad);

} // namespace melodee
