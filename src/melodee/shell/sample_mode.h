// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/scales.h"
#include "melodee/shell/context.h"

// The MPC's Sample Mode buttons on a kit (parity phase 4): Loop, Shift +
// Chop (Note On), 16 Levels and Shift + 16 Levels (its type), and Sample
// held while a pad picks a kit pad without playing it. The shell hands
// these buttons here; the Context jobs they call live in sample_mode.cpp.
//
// Sample is the SOUND hub (docs/HUBS_PLAN.md): a tap opens Stage, the
// selected track's sound, from any page, and on Stage goes home (B1's first
// page); it opens no other page. Held: a pad picks a kit pad (silent, as on
// the MPC), B1-B3 open Stage's sections, and after sample_sheet_ms the screen
// shows the track's pads (Pads on a kit, Keys otherwise) but on Steps and
// Roll, which stay in view with the held sound named in the header.
namespace melodee {

constexpr uint32_t sample_sheet_ms = 300;
// Whether the screen shows Sample held's pads now (not on Steps and Roll, nor under the Name or Exit sheet).
inline bool sample_sheet_shown(const UiState& ui, uint32_t now) {
    return ui.sample_held && now - ui.sample_down >= sample_sheet_ms && ui.page != PageId::Steps && ui.page != PageId::Roll &&
           !ui.name.open() && !ui.exit_open;
}
// Whether Steps or Roll name the held sound instead (the editor stays in view).
inline bool sample_held_named(const UiState& ui, uint32_t now) {
    return ui.sample_held && now - ui.sample_down >= sample_sheet_ms && (ui.page == PageId::Steps || ui.page == PageId::Roll);
}

// The page whose cards K1-K3 are while Sample is held (Pads on a kit, Keys otherwise: Keys' root, scale and
// layout); PageId::Count when it is not held, on Steps and Roll (their own cards stay) or on an empty track.
inline PageId sample_held_page(const Project& project, const UiState& ui) {
    const auto& t = project.tracks[ui.track % track_count];
    if (!ui.sample_held || ui.page == PageId::Steps || ui.page == PageId::Roll || !t.used()) return PageId::Count;
    return t.engine == EngineKind::Sampler ? PageId::Pads : PageId::Keys;
}

// Sample: Shift opens Input Config at once; alone it waits for its release,
// so a pad pressed meanwhile only picks a kit pad (and no page opens).
void sample_button(Context& c, bool pressed);
// B1-B3 while Sample is held: Stage on that section. False when Sample is not held.
bool sample_key(Context& c, unsigned key);
// A pad while Sample is held: on a kit it becomes the last pad, silently.
// False when Sample is not held there.
bool sample_pad(Context& c, unsigned pad);
void loop_button(Context& c);   // Loop: the last pad loops or not; Shift reverse
void chop_button(Context& c);   // Chop opens Chop; Shift: Note On
void levels_button(Context& c); // 16 Levels on and off (kit or synth); Shift: its type

// 16 Levels held (there is no 16 Levels page): a tap turns it on or off in place, a hold puts its settings on
// K1-K3 of any page (the type, then its two ends or the key's root and scale) and − / + move its octave or
// Zones' page. Not of type Loop (that tap opens the Looper), nor on an empty track or the Looper.
constexpr uint32_t levels_hold_ms = 300;
inline bool levels_held_cards(const Project& project, const UiState& ui) {
    const auto& t = project.tracks[ui.track % track_count];
    return ui.levels_held && !ui.shift && t.used() && t.levels_kind != unsigned(LevelsKind::Loop) && ui.page != PageId::Looper &&
           !ui.name.open() && !ui.exit_open;
}
// − / + while 16 Levels is held: Scale and a kit's Tune an octave, Zones a page of sixteen keys.
void levels_nudge(Context& c, int way);

} // namespace melodee
