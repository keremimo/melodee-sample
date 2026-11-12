// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>

// Trig conditions (roadmap P1): which passes of its pattern a note plays on,
// beside its chance. A pass counts from 0 where the pattern began (launched,
// Play, or its clip in the song); a kit lane of its own length counts its own.
// One byte a note (Event::condition): 0 always; 1..35 n:m, the n-th of every m
// passes (1:2, 2:2, 1:3 .. 8:8); then FILL and NOT FILL (Play held while
// playing), FIRST and NOT FIRST (the first pass), PRE and NOT PRE (the last
// note weighed before it on its lane played, or not), NEI and NOT NEI (the
// same of the lane before: a kit's pad before, else the track before).
namespace melodee {

constexpr unsigned trig_always = 0;
constexpr unsigned trig_passes = 35; // 1:2 .. 8:8
constexpr unsigned trig_fill = 36, trig_not_fill = 37, trig_first = 38, trig_not_first = 39, trig_pre = 40, trig_not_pre = 41,
                   trig_nei = 42, trig_not_nei = 43;
constexpr unsigned trig_count = 44;

// n:m as its byte, and back (false for any other condition).
constexpr unsigned trig_of(unsigned n, unsigned m) {
    unsigned at = 1;
    for (unsigned every = 2; every < m && every <= 8; ++every) at += every;
    return m >= 2 && m <= 8 && n >= 1 && n <= m ? at + n - 1 : trig_always;
}
constexpr bool trig_pass(unsigned c, unsigned& n, unsigned& m) {
    if (c < 1 || c > trig_passes) return false;
    unsigned at = c - 1;
    for (unsigned every = 2; every <= 8; ++every) {
        if (at < every) { n = at + 1; m = every; return true; }
        at -= every;
    }
    return false;
}

// Whether a note with condition `c` plays on pass `pass` (from 0), with Fill held or not, the note before on its
// lane having played (pre) and the lane before's (nei).
constexpr bool trig_met(unsigned c, uint32_t pass, bool fill, bool pre, bool nei) {
    unsigned n = 0, m = 0;
    if (trig_pass(c, n, m)) return pass % m == n - 1;
    switch (c) {
    case trig_fill: return fill;
    case trig_not_fill: return !fill;
    case trig_first: return pass == 0;
    case trig_not_first: return pass > 0;
    case trig_pre: return pre;
    case trig_not_pre: return !pre;
    case trig_nei: return nei;
    case trig_not_nei: return !nei;
    default: return true;
    }
}

// Whether how a note went is what PRE and NEI hear next: one with a condition of its own (not PRE or NEI, which
// follow another's) or a chance below 100.
constexpr bool trig_weighed(unsigned c, unsigned chance) {
    return chance < 100 || (c != trig_always && c != trig_pre && c != trig_not_pre && c != trig_nei && c != trig_not_nei && c < trig_count);
}

// As the cards and the pages name it: "1:2", "FILL", "NOT FILL" .. ("ALWAYS" for 0).
inline const char* trig_name(unsigned c) {
    static const char* const passes[] {"1:2", "2:2", "1:3", "2:3", "3:3", "1:4", "2:4", "3:4", "4:4", "1:5", "2:5", "3:5", "4:5", "5:5",
                                       "1:6", "2:6", "3:6", "4:6", "5:6", "6:6", "1:7", "2:7", "3:7", "4:7", "5:7", "6:7", "7:7",
                                       "1:8", "2:8", "3:8", "4:8", "5:8", "6:8", "7:8", "8:8"};
    static const char* const rest[] {"FILL", "NOT FILL", "FIRST", "NOT FIRST", "PRE", "NOT PRE", "NEI", "NOT NEI"};
    if (c >= 1 && c <= trig_passes) return passes[c - 1];
    if (c > trig_passes && c < trig_count) return rest[c - trig_passes - 1];
    return "ALWAYS";
}

} // namespace melodee
