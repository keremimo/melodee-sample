// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include <algorithm>
#include <vector>

// Copy (Shift + Erase) inside one pattern: a key's notes onto another key
// (a synth's note, a drum, a kit pad's; on Roll, Step Edit and a synth's play
// pages), or a step onto another step (Steps). What was on the target goes
// first, as an MPC copy replaces. Pure: the shell wraps them in one undo.
namespace melodee {

// Every event of key `from` again on key `to`, the same times and values; `to`'s own events go first.
// False when `from` has none or the pattern fills up (what fitted stays).
inline bool copy_key_events(Pattern& p, int from, int to) {
    if (from == to || from < 0 || to < 0) return false;
    std::vector<Event> copies;
    for (const auto& e : p.events) if (e.key == from) { Event c = e; c.key = uint8_t(to); copies.push_back(c); }
    if (copies.empty()) return false;
    p.events.erase(std::remove_if(p.events.begin(), p.events.end(), [&](const Event& e) { return e.key == to; }), p.events.end());
    bool all = true;
    for (const auto& c : copies) all &= p.add(c);
    return all;
}

// Whether step `s` holds a note of `lane` (any key: -1).
inline bool step_has_notes(const Pattern& p, unsigned s, int lane) {
    uint32_t start = uint32_t(s) * p.q(), end = std::min<uint32_t>(start + p.q(), p.length);
    for (size_t n = p.lower(start); n < p.events.size() && p.events[n].tick < end; ++n)
        if (lane < 0 || p.events[n].key == lane) return true;
    return false;
}

// The notes of `lane` (any key: -1) starting in step `from` onto step `to`, each as far into it; `to`'s own notes
// of `lane` go first. False when `from` holds none or the pattern fills up.
inline bool copy_step_notes(Pattern& p, unsigned from, unsigned to, int lane) {
    if (from == to || !step_has_notes(p, from, lane)) return false;
    uint32_t q = p.q(), a = uint32_t(from) * q, b = uint32_t(to) * q;
    if (b >= p.length) return false;
    uint32_t a_end = std::min<uint32_t>(a + q, p.length), b_end = std::min<uint32_t>(b + q, p.length);
    std::vector<Event> copies;
    for (size_t n = p.lower(a); n < p.events.size() && p.events[n].tick < a_end; ++n)
        if (lane < 0 || p.events[n].key == lane) { Event c = p.events[n]; c.tick = b + (c.tick - a); if (c.tick < b_end) copies.push_back(c); }
    p.events.erase(std::remove_if(p.events.begin(), p.events.end(), [&](const Event& e) {
        return e.tick >= b && e.tick < b_end && (lane < 0 || e.key == lane); }), p.events.end());
    bool all = true;
    for (const auto& c : copies) all &= p.add(c);
    return all;
}

} // namespace melodee
