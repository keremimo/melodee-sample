// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pad_params.h"
#include "melodee/model/pattern.h"
#include "melodee/model/project.h"
#include "melodee/model/scales.h"
#include <algorithm>
#include <array>

// Step Edit's pads on the steps (pages/step_edit_page.cpp) on a kit show and toggle one lane, the last
// pad's: a tap sets or clears that pad's note in the step and leaves the
// other pads' notes alone (Roll keeps the whole kit). A synth's Steps works
// on the whole step. The pads light the same (shell/pad_lights.cpp).
namespace melodee {

// Step Edit's B1 card sets (UiState::step_cards), in the order B1 steps through those a track has: NOTE, LOCK and
// TRIG on every track, SOUND (a step's own sample, slice and way) on a kit or keygroup, EUCLID on a kit or Drums lane.
enum StepCards : uint8_t { cards_note = 0, cards_lock = 1, cards_trig = 2, cards_euclid = 3, cards_sound = 4, cards_count = 5 };
inline const char* step_cards_name(unsigned set) {
    static const char* const names[] {"NOTE", "LOCK", "TRIG", "EUCLID", "SOUND"};
    return names[set % cards_count];
}
inline bool sound_steps(const Track& t) { return t.engine == EngineKind::Sampler || t.engine == EngineKind::Keygroup; }
inline int steps_lane(const Track& t);
// The sets a track has, in order; how many.
inline unsigned step_card_sets(const Track& t, std::array<uint8_t, cards_count>& out) {
    unsigned n = 0;
    out[n++] = cards_note; out[n++] = cards_lock; out[n++] = cards_trig;
    if (sound_steps(t)) out[n++] = cards_sound;
    if (steps_lane(t) >= 0) out[n++] = cards_euclid;
    return n;
}
// The set in hand on a track (one it does not have: NOTE), its place among them, and the next (B1).
inline unsigned step_set(const Track& t, unsigned stored) {
    std::array<uint8_t, cards_count> sets {};
    unsigned n = step_card_sets(t, sets);
    for (unsigned k = 0; k < n; ++k) if (sets[k] == stored) return stored;
    return cards_note;
}
inline unsigned step_set_place(const Track& t, unsigned stored) {
    std::array<uint8_t, cards_count> sets {};
    unsigned n = step_card_sets(t, sets), set = step_set(t, stored);
    for (unsigned k = 0; k < n; ++k) if (sets[k] == set) return k;
    return 0;
}
inline unsigned next_step_set(const Track& t, unsigned stored) {
    std::array<uint8_t, cards_count> sets {};
    unsigned n = step_card_sets(t, sets);
    return sets[(step_set_place(t, stored) + 1) % n];
}

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
        if (m.lock() && (pad < 0 || !is_pad_lane(m.param) || m.pad == pad)) return true;
    }
    return false;
}

// Erase + pot on a lock card: the lock on lane (`param`, `pad`) that starts at `tick` out, the notes and the lane's
// recorded moves kept; false when there was none.
inline bool remove_lock(Pattern& p, uint32_t tick, unsigned param, unsigned pad) {
    for (size_t n = p.motion_lower(tick); n < p.motion.size() && p.motion[n].tick == tick; ++n)
        if (p.motion[n].on(param, pad) && p.motion[n].lock()) {
            p.motion.erase(p.motion.begin() + std::ptrdiff_t(n));
            return true;
        }
    return false;
}

// A note of `lane` at the start of step `s`, a step long; false when the pattern is full.
inline bool add_lane_note(Pattern& p, unsigned s, uint8_t lane, uint8_t velocity) {
    Event e;
    e.tick = uint32_t(s) * p.q();
    e.length = note_ticks(p.q());
    e.key = lane;
    e.velocity = velocity;
    return p.add(e);
}

} // namespace melodee
