// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// Note Repeat, as on the MPC (MELODEE_PARITY.md, phase 1c): it stays on the
// page. A tap latches it, a hold lasts while held, a double tap opens the Arp
// page. While it is on, B1-B3 pick its rate, 1/8, 1/16, 1/32, and with Shift
// held their triplets; the repeats swing with the project. It is the track's
// arp (a kit repeats the pad held). MIDI thread.
namespace melodee {

class Context;

constexpr uint32_t repeat_hold_ms = 400;   // longer than this, Note Repeat was held, not tapped
constexpr uint32_t repeat_double_ms = 450; // two presses this close: a double tap

void repeat_button(Context& c, bool pressed);
// Whether B1-B3 pick the rate here: repeat on, and not on Arp or Chord, whose own keys stay.
// On Stage only while Note Repeat is held: latched, B1-B3 stay Stage's (its ARP page has the rate on K2).
inline bool repeat_keys(const Project& project, const UiState& ui) {
    if (ui.page == PageId::Stage) return project.tracks[ui.track % track_count].arp && ui.repeat_held;
    return project.tracks[ui.track % track_count].arp && ui.page != PageId::Arp && ui.page != PageId::Chord;
}
// The arp rate (engine/arp.h) B1-B3 pick: 1/8, 1/16, 1/32; with Shift, 1/8T, 1/16T, 1/32T.
inline unsigned repeat_rate(int key, bool shift) { return 2 + 2 * (unsigned(key) % 3) + (shift ? 1 : 0); }
// B1-B3 while repeat_keys(): sets the rate; false when the page's own keys should answer.
bool repeat_soft(Context& c, int key);

} // namespace melodee
