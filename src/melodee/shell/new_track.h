// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"
#include "surface/controls.h"
#include <algorithm>
#include <cstdint>

// An empty track's Stage: what it can become, as a carousel (pages/stage_page.cpp).
// The wheel moves along it, Push makes the card in the middle. Picked with a pad
// (Tracks, Pad Bank held, Song's lanes) or Push on Tracks, the track goes straight
// there and, once made, back to the page it was picked on (B1 BACK goes back
// empty): a kit or a MIDI track at once, a plug-in once one is picked from the
// card, a kit from Samples once its first sample loads. B1 CANCEL at the top of
// Samples leaves that kit empty again.
namespace melodee {

class Context;

enum class NewChoice : uint8_t { Plugin, Kit, Samples, Record, MidiOut, MidiUsb, MidiAllUsb, Count };
constexpr unsigned new_choice_count = unsigned(NewChoice::Count);

// Where the carousel is drawn at `now`: the card picked, or on the way there (an ease-out over carousel_ms
// from new_pick_from, set as the wheel moves, at the ends a bump back, and a slide in as the carousel opens).
constexpr uint32_t carousel_ms = 220;
inline float new_pick_shown(const UiState& ui, uint32_t now) {
    float to = float(ui.new_pick % new_choice_count);
    uint32_t gone = now - ui.new_pick_at;
    if (!ui.new_pick_at || gone >= carousel_ms) return to;
    float t = 1 - float(gone) / float(carousel_ms);
    return to + (ui.new_pick_from - to) * t * t * t; // 1 - (1 - x)^3 of the way
}
// The wheel `delta` detents along the carousel (at `now`).
inline void new_pick_turn(UiState& ui, int delta, uint32_t now) {
    float shown = new_pick_shown(ui, now);
    int to = std::clamp<int>(int(ui.new_pick % new_choice_count) + delta, 0, int(new_choice_count) - 1);
    ui.new_pick_from = to == int(ui.new_pick) ? float(to) + (delta < 0 ? -0.18f : 0.18f) : shown; // at an end: a bump
    ui.new_pick = uint8_t(to);
    ui.new_pick_at = now ? now : 1;
}

// An empty track in hand on its carousel (or the plug-in picker it opened): the track is made, or another
// picked (Pad Bank + pad, or B1 CANCEL), before anything else. The other buttons wait; Play, Stop and Tap
// Tempo still work, and on the carousel Pad Bank (+ pad) too.
bool new_track_locked(const Context& c);
// A button pressed or let go while locked: true when the lock took it (a taken press's release goes too).
bool new_track_lock(Context& c, SurfaceButton button, bool pressed);
// The used track nearest the selected one (the lower of two as near), -1 when every track is empty.
int nearest_used_track(const Context& c);
// B1 CANCEL on the carousel: the nearest used track, back on the page the empty one was picked on.
void cancel_new_track(Context& c);

// The selected track empty: its Stage, the page in view kept to come back to.
void offer_new_track(Context& c);
// The card in the middle pressed: the track made, one undo step.
void make_new_track(Context& c, NewChoice choice);
// A sample loaded: a kit made for Samples goes back to the page it was picked on.
void new_track_landed(Context& c);
// B1 CANCEL at the top of Samples: back, and a kit just made there for its first sample empty again.
void samples_cancel(Context& c);

} // namespace melodee
