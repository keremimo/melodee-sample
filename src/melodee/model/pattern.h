// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/trig.h"
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
// Rec Quantize's grid (Settings::rec_grid): 0 follows the pattern's `q`, else 1 + a Division.
inline Division grid_division(unsigned grid, Division q) {
    return grid >= 1 && grid <= unsigned(Division::Count) ? Division(grid - 1) : q;
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

// A ratchet (roadmap P1): a note played as so many hits spread evenly over its length.
constexpr unsigned max_ratchet = 8;
constexpr int max_pitch_ramp = 24; // semitones a kit pad's last hit sits from its first

// One note of the pattern: where it starts, how long it is held (ticks).
struct Event {
    uint32_t tick = 0;
    uint16_t length = sixteenth_ticks;
    uint8_t key = 0, velocity = 100, chance = 100, flags = 0;
    int8_t pitch = 0; // a sampler pad's: semitones from its root (16 Levels, an instrument's keys)
    // Ratchet: hits inside its length, 1..8; the last hit's velocity in percent off the note's (-100..100), the
    // hits between going evenly; on a kit pad the last hit's semitones over the first (-24..24) too.
    uint8_t ratchet = 1;
    int8_t ramp = 0, pitch_ramp = 0;
    uint8_t condition = trig_always; // which passes it plays on, beside its chance (model/trig.h)
    uint8_t cutoff = 0; // roadmap P3, a kit pad's: 16 Levels Filter's cutoff + 1 it was played at (0: the pad's own)
    bool operator==(const Event& o) const {
        return tick == o.tick && length == o.length && key == o.key && velocity == o.velocity && chance == o.chance && flags == o.flags &&
               pitch == o.pitch && ratchet == o.ratchet && ramp == o.ramp && pitch_ramp == o.pitch_ramp && condition == o.condition &&
               cutoff == o.cutoff;
    }
    bool operator!=(const Event& o) const { return !(*this == o); }
    unsigned hits() const { return std::clamp<unsigned>(ratchet, 1, max_ratchet); }
    // Hit `n` of the ratchet (0 the first): how hard, and its semitones over `pitch` (a kit pad's ramp).
    int hit_velocity(int played, unsigned n) const {
        unsigned last = hits() - 1;
        if (!last || !ramp) return played;
        double scale = 1.0 + ramp / 100.0 * double(std::min(n, last)) / double(last);
        return std::clamp(int(played * scale + 0.5), 1, 127);
    }
    int hit_pitch(unsigned n) const {
        unsigned last = hits() - 1;
        if (!last || !pitch_ramp) return 0;
        double at = pitch_ramp * double(std::min(n, last)) / double(last);
        return int(at < 0 ? at - 0.5 : at + 0.5);
    }
    // Copies what plays it besides where and which (chance, flags, the ratchet, the condition), for a note rewritten in place.
    void take_play(const Event& o) {
        chance = o.chance; flags = o.flags; ratchet = o.ratchet; ramp = o.ramp; pitch_ramp = o.pitch_ramp; condition = o.condition;
    }
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
    uint8_t ratchet = 1;   // the first note's (Event::ratchet); set_step keeps each note's own
    int8_t ramp = 0;
    uint8_t condition = 0; // the first note's (Event::condition), kept the same way
    bool empty() const { for (auto n : notes) if (n.velocity) return false; return true; }
    const Note* first() const { for (auto& n : notes) if (n.velocity) return &n; return nullptr; }
};

// A point of Motion (roadmap P1: kept by its pattern, at tick resolution): from `tick` on, a lane holds `value`. A
// lane is an engine parameter, an effect's on the track's chain or the mod wheel (`param`, model/limits.h's
// motion_* numbers), or a kit pad's own value on `pad`. A lock (`hold` above 0: a pot turned with a step or note
// held) holds for that many ticks only, then the lane goes back to what it was.
struct MotionPoint {
    uint32_t tick = 0;
    uint8_t param = 0, pad = 0;
    int16_t value = 0;
    uint16_t hold = 0;
    bool lock() const { return hold > 0; }
    bool on(unsigned p, unsigned k) const { return param == p && pad == k; }
    bool operator==(const MotionPoint& o) const { return tick == o.tick && param == o.param && pad == o.pad && value == o.value && hold == o.hold; }
    bool operator!=(const MotionPoint& o) const { return !(*this == o); }
};

// A kit lane of its own length (roadmap P1, polymeter inside a kit): `key`'s notes loop over `length` ticks from
// where the pattern began, shorter than the pattern; its passes counted on their own (trig conditions).
struct LaneLength {
    uint8_t key = 0;
    uint32_t length = 0;
    bool operator==(const LaneLength& o) const { return key == o.key && length == o.length; }
};

// The Euclidean rhythm last laid on a kit lane (roadmap P1, model/generators.h): its hits over its steps, turned.
struct EuclidLane {
    uint8_t key = 0, hits = 0, steps = 16, rotate = 0;
    bool operator==(const EuclidLane& o) const { return key == o.key && hits == o.hits && steps == o.steps && rotate == o.rotate; }
};

// A pattern's feel (roadmap P1, Time Correct heard live): how its notes play without rewriting them. The audio thread
// plays feel_pattern() of the pattern; the pages and the file keep the notes as written.
struct Feel {
    uint8_t strength = 0;          // 0..100: notes drawn that far toward the nearest step of the pattern's Q
    uint8_t swing = 50;            // 50..75: with strength, every second step of a straight Q that late, as Time Correct swings
    uint8_t humanize = 0;          // 0..100: each note early or late, up to a quarter of a step of Q (fixed per note)
    uint8_t humanize_velocity = 0; // 0..100: each note softer or harder, up to 20
    // A groove taken from another pattern (groove_of()): how far each of its steps falls off the grid and how hard it
    // plays against the mean, laid on these notes by the step nearest each, `groove_amount` of it.
    uint16_t groove_from = 0;      // 0 none; else 1 + track * pattern_count + pattern, where it was taken from
    uint8_t groove_amount = 100;
    uint16_t groove_step = sixteenth_ticks;
    std::vector<int8_t> groove_offsets;  // ticks, within half a step
    std::vector<uint8_t> groove_levels;  // percent, 25..175
    bool plain() const { return !strength && !humanize && !humanize_velocity && (groove_offsets.empty() || !groove_amount); }
    bool operator==(const Feel& o) const {
        return strength == o.strength && swing == o.swing && humanize == o.humanize && humanize_velocity == o.humanize_velocity &&
               groove_from == o.groove_from && groove_amount == o.groove_amount && groove_step == o.groove_step &&
               groove_offsets == o.groove_offsets && groove_levels == o.groove_levels;
    }
    bool operator!=(const Feel& o) const { return !(*this == o); }
};

// pad-fx C (round 2): a punch kept in a pattern, looping with it like Motion: from `tick` (of the pattern) for
// `length` ticks, punch `kind` (engine/punch.h) on `target` (255 the master, else a track) at `depth` percent, on the
// grid it was played with (`snap`, engine/punch.h punch_snaps) and letting go as it did (`tail`, punch_tails).
struct PatternPunch {
    uint32_t tick = 0, length = sixteenth_ticks;
    uint8_t kind = 0, target = 255, depth = 100, snap = 2, tail = 1;
    bool operator==(const PatternPunch& o) const {
        return tick == o.tick && length == o.length && kind == o.kind && target == o.target && depth == o.depth && snap == o.snap && tail == o.tail;
    }
};
constexpr unsigned max_pattern_punches = 32;
// A punch as it was heard, in the sequencer's ticks since Play (`until` 0: still sounding); `felt`: its pad's
// pressure has set its depth (the deepest is kept).
struct PunchSpan { uint64_t from = 0, until = 0; uint8_t kind = 0, target = 255, depth = 100, snap = 2, tail = 1; bool felt = false; };
// The take (what B3 KEEP keeps, and the page draws): the spans heard up to one pass (`length` ticks) back from where
// the latest let go (now, while one still sounds), however long ago, as punches of the pattern playing `position`
// ticks into its loop at `now`, so they fall where they were heard; at most max_pattern_punches, oldest first.
inline std::vector<PatternPunch> heard_punches(const PunchSpan* spans, unsigned count, uint64_t now, uint32_t position, uint32_t length) {
    std::vector<PatternPunch> out;
    if (!length || !count) return out;
    uint64_t last = 0;
    for (unsigned n = 0; n < count; ++n) last = std::max(last, spans[n].until ? spans[n].until : now);
    uint64_t window = last >= length ? last - length : 0, start = now >= position ? now - position : 0; // where this pass began
    for (unsigned n = 0; n < count && out.size() < max_pattern_punches; ++n) {
        uint64_t from = std::max(spans[n].from, window), until = spans[n].until ? spans[n].until : now;
        if (until <= from || until <= window) continue;
        int64_t at = (int64_t(from) - int64_t(start)) % int64_t(length);
        if (at < 0) at += int64_t(length);
        PatternPunch p;
        p.tick = uint32_t(at);
        p.length = uint32_t(std::clamp<uint64_t>(until - from, 1, length));
        p.kind = spans[n].kind; p.target = spans[n].target; p.depth = spans[n].depth; p.snap = spans[n].snap; p.tail = spans[n].tail;
        out.push_back(p);
    }
    return out;
}

struct Pattern {
    uint32_t length = 16 * sixteenth_ticks; // ticks
    Division division = Division::Sixteenth; // Q
    std::vector<Event> events;               // by tick, then key
    std::vector<MotionPoint> motion;         // by tick, then lane; at most motion_points
    std::vector<LaneLength> lanes;           // keys looping on their own, by key
    Feel feel;                               // how its notes play (roadmap P1)
    std::vector<EuclidLane> euclids;         // the rhythms laid on its kit lanes, by key (roadmap P1)
    std::vector<PatternPunch> punches;       // punches kept with it (pad-fx C), by tick

    unsigned q() const { return division_ticks(division); }
    // Steps of Q the pattern holds (a last part step counts).
    unsigned steps() const { return std::max(1u, unsigned((length + q() - 1) / q())); }
    bool empty() const { return events.empty() || events.front().tick >= length; }
    bool operator==(const Pattern& o) const {
        return length == o.length && division == o.division && events == o.events && motion == o.motion && lanes == o.lanes &&
               feel == o.feel && euclids == o.euclids && punches == o.punches;
    }
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
    // Drops every note (and Motion point) from `tick` on.
    void cut(uint32_t tick);

    // Motion (roadmap P1). The first point at or after `tick`.
    size_t motion_lower(uint32_t tick) const {
        return size_t(std::lower_bound(motion.begin(), motion.end(), tick, [](const MotionPoint& m, uint32_t t) { return m.tick < t; }) - motion.begin());
    }
    // Puts a point in its place; one on the same tick and lane is replaced. False when full.
    bool write_motion(const MotionPoint& m);
    // A lane's points: removed (locks too: false when there were none), counted, or any there.
    bool clear_lane(unsigned param, unsigned pad);
    unsigned lane_points(unsigned param, unsigned pad) const;
    bool has_lane(unsigned param, unsigned pad) const { return lane_points(param, pad) > 0; }
    // The lock on a lane at `tick`, if one starts there.
    const MotionPoint* lock_at(uint32_t tick, unsigned param, unsigned pad) const;
    // What a lane holds at `tick` as recorded (locks aside): the last point at or before it, else `own`.
    int lane_value(uint32_t tick, unsigned param, unsigned pad, int own) const;
    void sort_motion();

    // Kit lanes (roadmap P1): the ticks `key` loops over (its own, else the pattern's), whether it has its own, and
    // setting it (the pattern's length or more, or 0: none of its own).
    uint32_t lane_length(unsigned key) const {
        for (const auto& l : lanes) if (l.key == key) return std::max<uint32_t>(std::min(l.length, length), 1);
        return length;
    }
    bool own_lane(unsigned key) const { return lane_length(key) < length; }
    void set_lane_length(unsigned key, uint32_t ticks);
    void fit_lanes(); // after the pattern's length changed: lanes as long as it, or longer, go
};

// A track's playback speed (roadmap P1): its patterns play at num / den of the tempo, the notes as written. The
// audio thread plays them scaled (speed_pattern: ticks times den / num); the pages and the file keep them as written.
struct Speed { uint8_t num, den; };
constexpr unsigned speed_count = 5, speed_normal = 2;
constexpr Speed speeds[speed_count] {{1, 2}, {3, 4}, {1, 1}, {3, 2}, {2, 1}};
inline Speed speed_of(unsigned speed) { return speeds[speed < speed_count ? speed : speed_normal]; }
inline const char* speed_name(unsigned speed) {
    static const char* names[] {"\xc3\x97\xc2\xbd", "\xc3\x97\xc2\xbe", "\xc3\x97" "1", "\xc3\x97" "1\xc2\xbd", "\xc3\x97" "2"}; // x1/2 .. x2
    return names[speed < speed_count ? speed : speed_normal];
}
Pattern speed_pattern(const Pattern& p, unsigned speed);
// What the audio thread plays of a pattern on a track at `speed`: its feel laid on, then its speed.
Pattern played_pattern(const Pattern& p, unsigned speed);

// Roadmap P1, the feel: the pattern as the audio thread plays it (its notes moved and their velocities scaled as its
// Feel says; the same pattern when plain), and a groove taken from a pattern's notes (none when it has none).
Pattern feel_pattern(const Pattern& p);
void groove_of(const Pattern& source, Feel& feel);

// The events starting in step `step` of a pattern's Q: events[first, first + count) (Step Edit lists them).
struct StepEvents { size_t first = 0; unsigned count = 0; };
StepEvents step_events(const Pattern& pattern, unsigned step);

// Edits of a whole pattern (Shift + Pad 2, 3, 6, 7 and Time Correct). Each
// keeps the pattern's notes in order; two notes landing on the same tick
// with the same key become one.
bool double_pattern(Pattern& p, uint32_t max_length, bool copy = true); // Double Seq: twice as long, the notes twice (or the new half empty); false past max_length
void half_pattern(Pattern& p);                        // Half Seq: the first half
void scale_pattern(Pattern& p, bool slower);          // Half / Double Speed: every note twice as far apart, or half; the length stays
// The tick a recorded note lands on with Rec Quantize: the nearest step of
// Q; past the last step, the first (next time round).
uint32_t quantise_tick(const Pattern& p, uint32_t tick);
// The same onto a grid of `grid` ticks (Rec Quantize's own: shell/settings.h's rec_grid).
uint32_t quantise_tick(const Pattern& p, uint32_t tick, unsigned grid);
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
