// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// Note Repeat, as on the MPC (MELODEE_PARITY.md, phase 1c): it stays on the
// page. A tap latches it, a hold lasts while held; its arp's settings are
// Stage's ARP page (B3). While it is on, B1-B3 pick its rate, 1/8, 1/16, 1/32, and with Shift
// held their triplets; the repeats swing with the project. It is the track's
// arp (a kit repeats the pad held). MIDI thread.
namespace melodee {

class Context;

constexpr uint32_t repeat_hold_ms = 400;   // longer than this, Note Repeat was held, not tapped

void repeat_button(Context& c, bool pressed);
// Whether B1-B3 pick the rate here: repeat on, and not on Song or the Looper, whose own keys stay (the Looper's
// pads are the loop grid, which repeats nothing).
// On Stage only while Note Repeat is held: latched, B1-B3 stay Stage's (its ARP page has the rate on K2).
inline bool repeat_keys(const Project& project, const UiState& ui) {
    if (ui.page == PageId::Stage) return project.tracks[ui.track % track_count].arp && ui.repeat_held;
    return project.tracks[ui.track % track_count].arp && ui.page != PageId::Song && ui.page != PageId::Looper;
}
// The arp rate (engine/arp.h) B1-B3 pick: 1/8, 1/16, 1/32; with Shift, 1/8T, 1/16T, 1/32T. The lit key again
// steps on (roadmap P1): B1 to 1/4, B3 to 1/64 (and their triplets), and back; B2 stays.
inline unsigned repeat_rate(int key, bool shift) { return 2 + 2 * (unsigned(key) % 3) + (shift ? 1 : 0); }
inline unsigned repeat_other(int key, bool shift) {
    unsigned k = unsigned(key) % 3;
    return k == 0 ? 0u + (shift ? 1 : 0) : k == 2 ? 8u + (shift ? 1 : 0) : repeat_rate(key, shift);
}
// The rate a press of B1-B3 gives while `current` plays, and what that key shows.
inline unsigned repeat_next(unsigned current, int key, bool shift) {
    unsigned a = repeat_rate(key, shift), b = repeat_other(key, shift);
    return current == a && b != a ? b : a;
}
inline unsigned repeat_shown(unsigned current, int key, bool shift) {
    return current == repeat_other(key, shift) ? current : repeat_rate(key, shift);
}
// B1-B3 while repeat_keys(): sets the rate; false when the page's own keys should answer.
bool repeat_soft(Context& c, int key);

} // namespace melodee
