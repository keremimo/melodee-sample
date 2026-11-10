// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// Mute, as printed (MELODEE_PARITY.md, phase 1c): a tap latches mute mode,
// a hold lasts while Mute is held, Shift + Mute unmutes everything. In mute
// mode the pads mute instead of playing: kit pads on a page where the pads
// play the kit, tracks everywhere else; with Shift a pad solos. MIDI thread.
namespace melodee {

class Context;

constexpr uint32_t mute_hold_ms = 400; // longer than this, Mute was held, not tapped

void mute_button(Context& c, bool pressed);
// Whether mute mode works on kit pads here: the pads play the selected
// track's kit (`kit_pads`), and the page is not one about tracks (Stage, Mix).
inline bool mute_on_kit(const UiState& ui, bool kit_pads) { return kit_pads && ui.page != PageId::Stage && ui.page != PageId::Mix; }
// A pad pressed in mute mode: mutes (or unmutes) a kit pad or a track; with Shift, solos.
void mute_pad(Context& c, unsigned pad, bool kit_pads);
void set_pad_mute(Context& c, unsigned track, unsigned kit, bool on);
// Shift + Mute: every track, every solo, every kit pad of every track.
void unmute_all(Context& c);
// A kit pad soloed: the only used pad of the kit not muted.
inline bool pad_soloed(const Track& t, unsigned kit) {
    kit %= sampler_pads;
    if (!t.pads[kit].used() || t.pad_muted(kit)) return false;
    for (unsigned n = 0; n < sampler_pads; ++n)
        if (n != kit && t.pads[n].used() && !t.pad_muted(n)) return false;
    return true;
}

} // namespace melodee
