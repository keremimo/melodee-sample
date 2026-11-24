// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/motion_points.h"
#include "melodee/model/limits.h"
#include "melodee/shell/context.h"
#include "melodee/shell/mod_jobs.h"
#include "melodee/shell/undo_scope.h"
#include <algorithm>
#include <cstdlib>

namespace melodee {
namespace {

constexpr uint32_t move_gesture = 0x40710000u;  // Shift + wheel's moves, one undo while they go on
constexpr uint32_t value_gesture = 0x40720000u; // a control's turns of a point's value (^ its card's key)

int hand_lane(Context& c) { return motion_draw_lane(c.project, c.ui, c.pattern(), c.now); }

// The lane's own points (locks aside) strictly between `from` and `to`, the nearest to `from`; -1 for none.
int64_t point_between(const Pattern& p, int lane, uint32_t from, uint32_t to) {
    if (lane < 0 || from == to) return -1;
    unsigned lp = lane_param(lane), lk = lane_pad(lane) % sampler_pads;
    if (to > from) {
        for (size_t n = p.motion_lower(from + 1); n < p.motion.size() && p.motion[n].tick < to; ++n)
            if (p.motion[n].on(lp, lk) && !p.motion[n].lock()) return p.motion[n].tick;
    } else {
        for (size_t n = p.motion_lower(from); n-- > 0 && p.motion[n].tick > to;)
            if (p.motion[n].on(lp, lk) && !p.motion[n].lock()) return p.motion[n].tick;
    }
    return -1;
}

void cursor_to(Context& c, uint32_t tick) {
    if (c.ui.motion_cursor == tick) return;
    c.ui.motion_cursor = tick;
    for (auto& pot : c.ui.pots) { pot.engaged = false; pot.bound = 0; } // a new value under the knobs: picked up again
}

// More lanes in a line at once than a track plays (model/limits.h motion_ramps_at_once): the edit is taken back.
bool too_many_ramps(Context& c, const Pattern& p) {
    if (p.ramps_at_once() <= motion_ramps_at_once) return false;
    c.notice("Too many lines at once");
    return true;
}

bool lane_editable(Context& c, int lane) {
    if (lane < 0) { c.notice("No lane here"); return false; }
    if (!motion_param(c.track(), lane, &c.project).info) { c.notice("No lane here"); return false; }
    return !c.frozen_edit(c.ui.track); // freeze plan F2b: asks to unfreeze first
}

} // namespace

int motion_hand(const UiState& ui, uint32_t now) {
    if (ui.turning >= 0 && now - ui.turned_at < 1500) return ui.turning % 3;
    return ui.last_pot >= 0 ? ui.last_pot % 3 : 0;
}

const MotionPoint* motion_point_here(Context& c) {
    if (!c.ui.motion_draw) return nullptr;
    const auto& p = c.pattern();
    int lane = hand_lane(c);
    if (lane < 0) return nullptr;
    return p.point_at(motion_cursor(c.ui, p), lane_param(lane), lane_pad(lane) % sampler_pads);
}

void motion_cursor_wheel(Context& c, int delta) {
    const auto& p = c.pattern();
    uint32_t length = std::max<uint32_t>(p.length, 1), q = std::max(p.q(), 1u);
    int lane = hand_lane(c);
    uint32_t at = motion_cursor(c.ui, p);
    for (int n = 0; n < std::abs(delta); ++n) {
        uint32_t next = delta > 0 ? (at / q + 1) * q : at == 0 ? 0 : (at - 1) / q * q;
        next = std::min(next, length); // the end is a place too
        int64_t point = point_between(p, lane, at, next); // a point on the way: there first
        at = point >= 0 ? uint32_t(point) : next;
    }
    cursor_to(c, at);
}

bool motion_point_move(Context& c, int delta) {
    const auto* here = motion_point_here(c);
    if (!here) return false;
    int lane = hand_lane(c);
    if (!lane_editable(c, lane)) return true;
    auto& p = c.pattern();
    uint32_t length = std::max<uint32_t>(p.length, 1), q = std::max(p.q(), 1u);
    unsigned lp = lane_param(lane), lk = lane_pad(lane) % sampler_pads;
    uint32_t at = here->tick;
    c.touch_scope(pattern_scope(c.project, c.ui), move_gesture);
    bool moved = false;
    for (int n = 0; n < std::abs(delta); ++n) {
        // A step of Q that way, past any other point of the lane (it keeps its place); at the ends it stays.
        for (uint32_t to = at;;) {
            if (delta > 0) { if (to >= length) break; to = std::min((to / q + 1) * q, length); }
            else { if (to == 0) break; to = (to - 1) / q * q; }
            if (!p.move_point(at, to, lp, lk)) continue;
            if (too_many_ramps(c, p)) { p.move_point(to, at, lp, lk); n = std::abs(delta); break; } // back, and no further
            at = to; moved = true;
            break;
        }
    }
    cursor_to(c, at);
    if (moved) c.sync_pattern(c.ui.track, c.track().pattern);
    return true;
}

void motion_point_erase(Context& c) {
    int lane = hand_lane(c);
    if (!motion_point_here(c)) { c.notice("No point here"); return; }
    if (!lane_editable(c, lane)) return;
    uint32_t at = motion_cursor(c.ui, c.pattern());
    MotionPoint was = *motion_point_here(c);
    bool erased = false;
    c.edit_pattern(c.ui.track, c.track().pattern, "Erase motion point", [&] {
        if (!c.pattern().erase_point(at, lane_param(lane), lane_pad(lane) % sampler_pads)) return;
        if (too_many_ramps(c, c.pattern())) { c.pattern().write_motion(was); return; } // the lines either side joined
        erased = true;
        c.sync_pattern(c.ui.track, c.track().pattern);
    });
    if (erased) c.notice("Point erased");
}

void motion_draw_toggle(Context& c) {
    c.ui.motion_draw = !c.ui.motion_draw;
    if (!c.ui.motion_draw) c.ui.motion_pin = -1; // the lanes in their own order again
    for (auto& pot : c.ui.pots) { pot.engaged = false; pot.bound = 0; } // the cards may show the points' values now
    c.notice(c.ui.motion_draw ? "Draw on" : "Draw off");
}

void motion_point_push(Context& c) {
    if (!c.ui.motion_draw) return;
    if (c.ui.erase) { c.ui.erase_used = true; motion_point_erase(c); return; } // Erase + Push: as Erase tapped
    int lane = hand_lane(c);
    if (!lane_editable(c, lane)) return;
    unsigned lp = lane_param(lane), lk = lane_pad(lane) % sampler_pads;
    uint32_t at = motion_cursor(c.ui, c.pattern());
    if (const auto* here = motion_point_here(c)) { // Push on a point: a line to the next, or hold
        bool ramp = !here->ramp && lp != motion_pad_mute;
        if (ramp == here->ramp) { c.notice("MUTE points hold"); return; }
        bool kept = false;
        c.edit_pattern(c.ui.track, c.track().pattern, ramp ? "Motion point: line" : "Motion point: hold", [&] {
            MotionPoint was = *here, m = was;
            m.ramp = ramp;
            if (!c.pattern().write_motion(m)) return;
            if (too_many_ramps(c, c.pattern())) { c.pattern().write_motion(was); return; }
            kept = true;
            c.sync_pattern(c.ui.track, c.track().pattern);
        });
        if (kept) c.notice(ramp ? "Line to the next point" : "Point holds");
        return;
    }
    if (c.pattern().lock_at(at, lp, lk)) { c.notice("A lock is here"); return; } // a lock stays a lock (a new point would replace it)
    MotionPoint m; // a new point at the value the lane has there
    m.tick = at; m.param = uint8_t(lp); m.pad = uint8_t(lk);
    m.value = int16_t(c.pattern().lane_value(at, lp, lk, motion_param(c.track(), lane, &c.project).value));
    m.ramp = lp != motion_pad_mute; // a mute is on or off: it holds
    bool kept = false, full = false;
    c.edit_pattern(c.ui.track, c.track().pattern, "Add motion point", [&] {
        if (!c.pattern().write_motion(m)) { full = true; return; }
        if (too_many_ramps(c, c.pattern())) { c.pattern().erase_point(m.tick, lp, lk); return; }
        kept = true;
        c.sync_pattern(c.ui.track, c.track().pattern);
    });
    if (full) c.notice("Motion full");
    if (!kept) return;
    auto lanes = motion_lanes(c.project, c.ui, c.pattern());
    auto shown = std::find(lanes.begin(), lanes.end(), lane);
    if (shown == lanes.end()) { // a lane Motion did not show (a parameter turned on another page): first now
        c.ui.motion_pin = int16_t(lane); c.ui.motion_pin_track = uint8_t(c.ui.track % track_count);
        lanes = motion_lanes(c.project, c.ui, c.pattern());
        shown = std::find(lanes.begin(), lanes.end(), lane);
    }
    if (shown != lanes.end()) { c.ui.last_pot = int8_t(shown - lanes.begin()); c.ui.turning = -1; } // its knob in hand
    c.notice("Point added: " + lane_name(c.track(), lane, &c.project));
}

const MotionPoint* motion_point_for(Context& c, const Binding& b) {
    if (!c.ui.motion_draw || c.ui.recording || c.ui.page != PageId::Motion || !b.bound()) return nullptr; // most of the time
    int lane = turned_lane(c.project, c.ui.track, b);
    if (lane < 0) return nullptr;
    auto shown = motion_lanes(c.project, c.ui, c.pattern()); // a lane Motion does not show is set as on any page
    if (std::find(shown.begin(), shown.end(), lane) == shown.end()) return nullptr;
    return motion_point_on(c.ui, c.pattern(), lane);
}

bool motion_point_take(Context& c, const Binding& b, int value) {
    const auto* here = motion_point_for(c, b);
    if (!here) return false;
    learn_lane(c, b); // the lane drawn on from now: the one turned
    if (here->value == value || c.frozen_edit(c.ui.track)) return true;
    c.touch_scope(pattern_scope(c.project, c.ui), value_gesture ^ b.key(c.ui.track)); // one undo per control's turns
    MotionPoint m = *here;
    m.value = int16_t(value);
    if (c.pattern().write_motion(m)) c.sync_motion(c.ui.track, c.track().pattern);
    return true;
}

} // namespace melodee
