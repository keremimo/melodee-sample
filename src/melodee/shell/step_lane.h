// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include "melodee/model/project.h"
#include "melodee/model/scales.h"
#include <algorithm>

// Steps (pages/steps_page.cpp) on a kit shows and toggles one lane, the last
// pad's: a tap sets or clears that pad's note in the step and leaves the
// other pads' notes alone (Roll keeps the whole kit). A synth's Steps works
// on the whole step. The pads light the same (shell/pad_lights.cpp).
namespace melodee {

// The key Steps works on: the last pad's on a kit or on Drums pads (its drum; pad 1 of the bank before any), -1 for
// the whole step.
inline int steps_lane(const Track& t) {
    if (t.levels) return -1;
    if (t.engine == EngineKind::Sampler) return int(t.step_note());
    if (drum_pads(t)) return drum_pad_of(t.last_note) >= 0 ? int(t.last_note) : drum_key(t, 0);
    return -1;
}

// The loudest note starting in step `s` on `lane` (on any key: -1), 0 none; an accent counts as 127.
inline unsigned step_velocity(const Pattern& p, unsigned s, int lane) {
    uint32_t start = uint32_t(s) * p.q(), end = std::min<uint32_t>(start + p.q(), p.length);
    unsigned loudest = 0;
    for (size_t n = p.lower(start); n < p.events.size() && p.events[n].tick < end; ++n) {
        const auto& e = p.events[n];
        if (lane >= 0 && e.key != lane) continue;
        loudest = std::max<unsigned>(loudest, e.flags & step_accent ? 127u : e.velocity);
    }
    return loudest;
}

// Takes `lane`'s notes out of step `s`; false when it held none.
inline bool clear_lane(Pattern& p, unsigned s, int lane) {
    uint32_t start = uint32_t(s) * p.q(), end = std::min<uint32_t>(start + p.q(), p.length);
    bool any = false;
    for (size_t n = p.lower(start); n < p.events.size() && p.events[n].tick < end;) {
        const auto& e = p.events[n];
        if (e.key == lane && p.remove(e.tick, e.key)) any = true; // the next one moves up into `n`
        else ++n;
    }
    return any;
}

// `lane`'s notes in step `s` onto key `to` (a kit: another pad's lane), as they were; false when it held none.
inline bool move_lane(Pattern& p, unsigned s, int lane, uint8_t to) {
    uint32_t start = uint32_t(s) * p.q(), end = std::min<uint32_t>(start + p.q(), p.length);
    std::array<Event, notes_per_step> moved {};
    unsigned count = 0;
    for (size_t n = p.lower(start); n < p.events.size() && p.events[n].tick < end && count < moved.size();) {
        Event e = p.events[n];
        if (e.key == lane && p.remove(e.tick, e.key)) moved[count++] = e;
        else ++n;
    }
    for (unsigned n = 0; n < count; ++n) { moved[n].key = to; p.add(moved[n]); }
    return count > 0;
}

// The first note of `lane` (any key: -1) starting in step `s`: its index in the events, or -1. Steps' cards
// show it (on a kit the last pad's note, not the step's first).
inline int lane_event(const Pattern& p, unsigned s, int lane) {
    uint32_t start = uint32_t(s) * p.q(), end = std::min<uint32_t>(start + p.q(), p.length);
    for (size_t n = p.lower(start); n < p.events.size() && p.events[n].tick < end; ++n)
        if (lane < 0 || p.events[n].key == lane) return int(n);
    return -1;
}

// Each note of `lane` (any key: -1) starting in step `s` through `edit`; false when it held none.
template <class Edit> bool edit_lane(Pattern& p, unsigned s, int lane, Edit&& edit) {
    uint32_t start = uint32_t(s) * p.q(), end = std::min<uint32_t>(start + p.q(), p.length);
    bool any = false;
    for (size_t n = p.lower(start); n < p.events.size() && p.events[n].tick < end; ++n)
        if (lane < 0 || p.events[n].key == lane) { edit(p.events[n]); any = true; }
    return any;
}

// Whether a Motion lock (roadmap P1) starts in ticks [from, to): on a kit pad's own values only that pad's (`pad`, 0..127;
// -1 any), the track's other lanes always.
inline bool lock_in(const Pattern& p, uint32_t from, uint32_t to, int pad) {
    for (size_t n = p.motion_lower(from); n < p.motion.size() && p.motion[n].tick < to; ++n) {
        const auto& m = p.motion[n];
        if (m.lock() && (pad < 0 || m.param < motion_pad_base || m.param == motion_wheel || m.pad == pad)) return true;
    }
    return false;
}

// A note of `lane` at the start of step `s`, a step long; false when the pattern is full.
inline bool add_lane_note(Pattern& p, unsigned s, uint8_t lane, uint8_t velocity) {
    Event e;
    e.tick = uint32_t(s) * p.q();
    e.length = uint16_t(std::min<uint32_t>(p.q(), 0xffff));
    e.key = lane;
    e.velocity = velocity;
    return p.add(e);
}

} // namespace melodee
