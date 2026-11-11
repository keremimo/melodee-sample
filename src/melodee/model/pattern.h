// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// A pattern as a list of note events on the sequencer's clock (96 ticks a
// quarter), up to 128 bars long. Q is the grid the pages step through and
// quantising snaps to; the notes themselves sit on any tick. The Steps and
// Roll pages see the pattern through step(): the notes that start inside one
// step of the Q grid, with where in the step each one falls.
namespace melodee {

// Q: the grid a pattern is edited and quantised on, as on the MPC.
enum class Division : uint8_t {
    Quarter, QuarterTriplet, Eighth, EighthTriplet, Sixteenth, SixteenthTriplet, ThirtySecond, ThirtySecondTriplet,
    SixtyFourth, Count
};
inline unsigned division_ticks(Division d) {
    static const unsigned ticks[] {96, 64, 48, 32, 24, 16, 12, 8, 6};
    return ticks[unsigned(d) < unsigned(Division::Count) ? unsigned(d) : 4];
}
inline const char* division_name(Division d) {
    static const char* names[] {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64"};
    return names[unsigned(d) < unsigned(Division::Count) ? unsigned(d) : 4];
}
inline bool division_triplet(Division d) { return unsigned(d) % 2 == 1 && d != Division::SixtyFourth; }
// Files from before parity phase 2 numbered five divisions: 1/8, 1/8T, 1/16, 1/16T, 1/32.
inline Division legacy_division(unsigned value) {
    static const Division map[] {Division::Eighth, Division::EighthTriplet, Division::Sixteenth, Division::SixteenthTriplet, Division::ThirtySecond};
    return value < 5 ? map[value] : Division::Sixteenth;
}

enum StepFlag : uint8_t { step_accent = 1, step_slide = 2, step_tie = 4 };

// One note of the pattern: where it starts, how long it is held (ticks).
struct Event {
    uint32_t tick = 0;
    uint16_t length = sixteenth_ticks;
    uint8_t key = 0, velocity = 100, chance = 100, flags = 0;
    int8_t pitch = 0; // a sampler pad's: semitones from its root (16 Levels, an instrument's keys)
    bool operator==(const Event& o) const {
        return tick == o.tick && length == o.length && key == o.key && velocity == o.velocity && chance == o.chance && flags == o.flags && pitch == o.pitch;
    }
    bool operator!=(const Event& o) const { return !(*this == o); }
};

// The pages' view of one step: up to four notes starting in it.
struct Note {
    uint8_t key = 0, velocity = 0; // velocity 0: no note
    uint8_t offset = 0;            // ticks after the step's start
};
struct Step {
    std::array<Note, notes_per_step> notes {};
    uint8_t length = 1;    // in steps of the pattern's Q
    uint8_t chance = 100;  // percent
    uint8_t flags = 0;
    bool empty() const { for (auto n : notes) if (n.velocity) return false; return true; }
    const Note* first() const { for (auto& n : notes) if (n.velocity) return &n; return nullptr; }
};

struct Pattern {
    uint32_t length = 16 * sixteenth_ticks; // ticks
    Division division = Division::Sixteenth; // Q
    std::vector<Event> events;               // by tick, then key

    unsigned q() const { return division_ticks(division); }
    // Steps of Q the pattern holds (a last part step counts).
    unsigned steps() const { return std::max(1u, unsigned((length + q() - 1) / q())); }
    bool empty() const { return events.empty() || events.front().tick >= length; }
    bool operator==(const Pattern& o) const { return length == o.length && division == o.division && events == o.events; }
    bool operator!=(const Pattern& o) const { return !(*this == o); }
    // The first event at or after `tick`.
    size_t lower(uint32_t tick) const {
        return size_t(std::lower_bound(events.begin(), events.end(), tick, [](const Event& e, uint32_t t) { return e.tick < t; }) - events.begin());
    }
    // Adds a note in its place; one already on that tick with that key is replaced. False when full.
    bool add(const Event& e);
    // Removes the note with that key on that tick; false when there is none.
    bool remove(uint32_t tick, unsigned key);
    // The notes starting in step `s` of Q (the first four), and setting them:
    // notes the step showed and no longer holds go, the rest keep their
    // place in the step and, while the step's length is unchanged, their own
    // length. Notes past the fourth in a step are left as they are.
    Step step(unsigned s) const;
    void set_step(unsigned s, const Step& value);
    // Keeps the events in order after they were moved in place.
    void sort();
    // Drops every note from `tick` on.
    void cut(uint32_t tick);
};

// The events starting in step `step` of a pattern's Q: events[first, first + count) (Step Edit lists them).
struct StepEvents { size_t first = 0; unsigned count = 0; };
StepEvents step_events(const Pattern& pattern, unsigned step);

// Edits of a whole pattern (Shift + Pad 2, 3, 6, 7 and Time Correct). Each
// keeps the pattern's notes in order; two notes landing on the same tick
// with the same key become one.
bool double_pattern(Pattern& p, uint32_t max_length); // Double Seq: twice as long, the notes twice; false past max_length
void half_pattern(Pattern& p);                        // Half Seq: the first half
void scale_pattern(Pattern& p, bool slower);          // Half / Double Speed: every note twice as far apart, or half; the length stays
// The tick a recorded note lands on with Rec Quantize: the nearest step of
// Q; past the last step, the first (next time round).
uint32_t quantise_tick(const Pattern& p, uint32_t tick);
// Time Correct: notes `chosen` (a bit a key, 0..191) onto the nearest step
// of Q, every second step of a straight Q late by swing (50..75 %), then
// moved by shift ticks (wrapping inside the pattern).
template <class Chosen> void time_correct(Pattern& p, Chosen&& chosen, unsigned swing, int shift);
void time_correct_all(Pattern& p, const std::array<uint64_t, 3>* keys, unsigned swing, int shift); // keys nullptr: every note

} // namespace melodee

namespace melodee {
template <class Chosen> void time_correct(Pattern& p, Chosen&& chosen, unsigned swing, int shift) {
    unsigned q = p.q();
    double late = division_triplet(p.division) ? 0 : q * (double(swing) - 50) / 50.0;
    int64_t length = std::max<uint32_t>(p.length, 1);
    std::vector<Event> moved;
    moved.reserve(p.events.size());
    for (auto e : p.events) {
        if (e.tick < p.length && chosen(e.key)) {
            uint32_t step = (e.tick + q / 2) / q;
            int64_t at = int64_t(step) * q + (step % 2 ? int64_t(late + 0.5) : 0) + shift;
            e.tick = uint32_t(((at % length) + length) % length);
        }
        moved.push_back(e);
    }
    p.events.clear();
    for (const auto& e : moved) p.add(e);
}

} // namespace melodee
