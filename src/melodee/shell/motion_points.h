// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/ui_state.h"
#include <algorithm>
#include <cstdint>

// Motion's points set by hand (pages/motion_page.cpp), in its draw mode
// (Shift + B3, UiState::motion_draw): a cursor on the pattern
// (UiState::motion_cursor, the wheel: a step of Q, stopping on each point of
// the lane drawn on the way), and the lane drawn's point under it. The lane
// drawn is the parameter turned last on the track, on any page (MOD's
// learned lane), else the lane of the knob in hand. Push adds a point there
// at the value the lane has there, one that goes in a straight line to the
// lane's next point (a ramp, model/pattern.h; a pad's MUTE lane holds), and
// a lane none of Motion's three shows comes first (UiState::motion_pin);
// Push on a point makes it go in a line or hold; Erase (tapped, or + Push)
// takes it out; Shift + wheel moves it a step of Q (past the lane's other
// points); with Seq Record off, a control turning the lane (its knob, the
// fader) sets its value, through Context::apply.
namespace melodee {

class Context;

// The knob (0-2) whose lane is in hand: the one turning, else the one turned last.
int motion_hand(const UiState& ui, uint32_t now);
// The lane drawn on: the parameter turned last on the track (any page), else the lane of the knob in hand.
inline int motion_draw_lane(const Project& project, const UiState& ui, const Pattern& pattern, uint32_t now) {
    unsigned t = ui.track % track_count;
    if (ui.learn_lane >= 0 && ui.learn_track == t && motion_param(project.tracks[t], ui.learn_lane, &project).info) return ui.learn_lane;
    return motion_lanes(project, ui, pattern)[size_t(motion_hand(ui, now))];
}
// The cursor's tick, inside the pattern or on its end (a point there is where the line before it ends).
inline uint32_t motion_cursor(const UiState& ui, const Pattern& pattern) {
    return std::min<uint32_t>(ui.motion_cursor, std::max<uint32_t>(pattern.length, 1));
}
// The point (not a lock) on `lane` under the cursor, if the pattern has one there that it plays or draws to: a
// recorded point on the pattern's end stays hidden (model/pattern.h MotionPoint::within), so none.
inline const MotionPoint* motion_point_at_cursor(const UiState& ui, const Pattern& pattern, int lane) {
    if (lane < 0) return nullptr;
    const auto* m = pattern.point_at(motion_cursor(ui, pattern), lane_param(lane), lane_pad(lane) % sampler_pads);
    return m && m->within(pattern.length) ? m : nullptr;
}
// Drawing, the point (not a lock) under the cursor on the lane drawn, if there is one: what Push, Erase and
// Shift + wheel edit.
const MotionPoint* motion_point_here(Context& c);
// Drawing with Seq Record off, the point (not a lock) under the cursor on `lane`, if there is one: the one rule for
// a card's value on Motion, the value its card shows (shell/bindings.cpp) and what turning it sets (motion_point_for).
inline const MotionPoint* motion_point_on(const UiState& ui, const Pattern& pattern, int lane) {
    if (!ui.motion_draw || ui.recording) return nullptr;
    return motion_point_at_cursor(ui, pattern, lane);
}
// On Motion, motion_point_on() for the lane a card turns, when it is one of the three Motion shows: what any control
// turning that card sets (Context::apply: a knob, the fader, Shift + wheel's fine edit) and the value it shows
// (Context::shown). motion_point_take() sets it and is true then, as one undo per control's turns.
const MotionPoint* motion_point_for(Context& c, const Binding& b);
bool motion_point_take(Context& c, const Binding& b, int value);
// The wheel on Motion: the cursor a step of Q at a time.
void motion_cursor_wheel(Context& c, int delta);
// Shift + wheel on Motion: the point under the cursor moved a step of Q at a time; false when there is none.
bool motion_point_move(Context& c, int delta);
// Push on Motion, drawing (Erase held: the point out).
void motion_point_push(Context& c);
// Erase on Motion, drawing: the point under the cursor out.
void motion_point_erase(Context& c);
// Shift + B3 on Motion: draw mode on or off.
void motion_draw_toggle(Context& c);


} // namespace melodee
