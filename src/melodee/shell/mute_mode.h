// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pad_params.h"
#include "melodee/model/pattern.h"
#include "melodee/shell/ui_state.h"
#include <algorithm>

// Mute, as printed (MELODEE_PARITY.md, phase 1c): a tap latches mute mode,
// a hold lasts while Mute is held, Shift + Mute unmutes everything. In mute
// mode the pads mute instead of playing: kit pads on a page where the pads
// play the kit, tracks everywhere else; with Shift a pad solos. MIDI thread.
namespace melodee {

class Context;

constexpr uint32_t mute_hold_ms = 400; // longer than this, Mute was held, not tapped

void mute_button(Context& c, bool pressed);
// Mute tapped on the mixer's FX block: the lit effect switched off or on (its bypass). False elsewhere, or on the plus.
bool mute_effect(Context& c);
// Whether mute mode works on kit pads here: the pads play the selected
// track's kit (`kit_pads`), and the page is not one about tracks (Stage, Mix).
inline bool mute_on_kit(const UiState& ui, bool kit_pads) { return kit_pads && ui.page != PageId::Mixer; }
// Whether mute mode works on the mixer's second row: its submixes and buses (shell/mixer_channels.h).
inline bool mute_on_others(const UiState& ui) { return ui.page == PageId::Mixer && ui.mixer_bank == 1; }
// A pad pressed in mute mode: mutes (or unmutes) a kit pad, a track, or on the mixer's second row a submix or bus;
// with Shift, solos (a track, a submix).
void mute_pad(Context& c, unsigned pad, bool kit_pads);
void set_pad_mute(Context& c, unsigned track, unsigned kit, bool on);
// Shift + Mute: every track, every solo, every kit pad of every track, every submix and bus.
void unmute_all(Context& c);
// Issue 32: kit pad `kit` muted as heard now. While `pattern` plays at `position` (written ticks; -1 stopped), where
// its mute lane last put it (before the lane's first point, as set); else as set (Mute + pad).
inline bool pad_muted_now(const Track& t, const Pattern& pattern, int position, unsigned kit) {
    kit %= sampler_pads;
    bool own = t.pad_muted(kit);
    if (position < 0 || pattern.motion.empty()) return own;
    return pattern.lane_value(uint32_t(position) % std::max<uint32_t>(pattern.length, 1), motion_pad_mute, kit, own) != 0;
}
bool pad_muted_now(const Context& c, unsigned track, unsigned kit);
// A kit pad soloed: the only used pad of the kit not muted.
inline bool pad_soloed(const Track& t, unsigned kit) {
    kit %= sampler_pads;
    if (!t.pads[kit].used() || t.pad_muted(kit)) return false;
    for (unsigned n = 0; n < sampler_pads; ++n)
        if (n != kit && t.pads[n].used() && !t.pad_muted(n)) return false;
    return true;
}

} // namespace melodee
