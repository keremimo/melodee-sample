// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// Pad Bank (MELODEE_PARITY.md, phase 1c). A tap steps the selected kit to
// its next bank, A-H round (Shift: the previous one; on Punch the other bank
// of punches). Held: a pad picks that track, Minus / Plus or the wheel jump
// banks, and after a second the screen shows the sixteen tracks (the pads are
// the tracks at once); Shift + Erase then copies a
// whole track, Erase + pad clears one (shell/track_copy.cpp). One job a tap, acted on release: no
// double tap (the Tracks page is Sample's second stop). MIDI thread.
namespace melodee {

class Context;
constexpr uint32_t pad_bank_overlay_ms = 1000; // held this long: the tracks over the page (a tap shows nothing)

void pad_bank_button(Context& c, bool pressed);
// Minus / Plus or the wheel while Pad Bank is held: the bank jumps. False when it is not held.
bool pad_bank_move(Context& c, int way);
// The selected kit's bank, `way` banks on (round A-H).
void step_bank(Context& c, int way);
// Whether the pads are the sixteen tracks now: Pad Bank held, or a track copy under way.
inline bool tracks_on_pads(const UiState& ui) { return ui.pad_bank || ui.copying == UiState::Copying::Tracks; }
// Whether the screen shows the tracks over the page: a track copy, or Pad Bank held a second.
inline bool track_overlay_shown(const UiState& ui, uint32_t now) {
    return ui.copying == UiState::Copying::Tracks || (ui.pad_bank && now - ui.pad_bank_down >= pad_bank_overlay_ms);
}
// A pad while tracks_on_pads(): picks that track, or the copy's track and
// then where it goes; with Erase held, asks to clear it. False when the pads are not the tracks.
bool track_pad(Context& c, unsigned pad);

} // namespace melodee
