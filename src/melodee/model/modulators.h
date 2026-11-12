// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Roadmap P4: the project's modulators and macros, as plain data.
//
// A modulator is an LFO (free, so many cycles a second, or on the song's
// clock, a cycle so many quarters) or an envelope follower (how loud a track
// or a bus is, after its chain), moving up to four targets: any Motion lane of
// any track (an engine parameter, an effect of its chain, a kit pad's own
// value, its level, pan or a send, the master's chain or a return), each by its
// own depth around where the lane stands (its value, or where Motion has it).
//
// A macro is one knob turning up to eight such lanes at once, each from a low
// value at the knob's left to a high one at its right (lower than the low
// turns it the other way). The audio thread never sees macros: the shell sets
// each lane as a pot would. Files keep modulators in MODS and macros in MACR.
namespace melodee {

constexpr unsigned modulator_count = 8, modulator_targets = 4, macro_count = 8, macro_targets = 8;

enum class ModKind : uint8_t { Off, Lfo, Follower, Count };
enum class LfoShape : uint8_t { Sine, Triangle, SawUp, SawDown, Square, Hold, Drift, Count };
inline const char* mod_kind_name(ModKind k) {
    static const char* names[] {"OFF", "LFO", "FOLLOW"};
    return names[unsigned(k) < unsigned(ModKind::Count) ? unsigned(k) : 0];
}
inline const char* lfo_shape_name(LfoShape s) {
    static const char* names[] {"SINE", "TRI", "SAW", "RAMP", "SQUARE", "S&H", "DRIFT"};
    return names[unsigned(s) < unsigned(LfoShape::Count) ? unsigned(s) : 0];
}
// On the clock: a cycle's length in quarters.
constexpr unsigned lfo_divisions = 10;
inline double lfo_division_quarters(unsigned d) {
    static const double q[lfo_divisions] {0.25, 1.0 / 3, 0.5, 2.0 / 3, 1, 2, 4, 8, 16, 32};
    return q[d < lfo_divisions ? d : 4];
}
inline const char* lfo_division_name(unsigned d) {
    static const char* names[lfo_divisions] {"1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "8 bars"};
    return names[d < lfo_divisions ? d : 4];
}
// Free: RATE 0..100 from a fiftieth of a hertz to twenty, evenly by octaves.
inline double lfo_hertz(unsigned rate) { return 0.02 * std::pow(1000.0, std::min(rate, 100u) / 100.0); }
// A follower's rise and fall, ms, from 0..100.
inline double follower_attack_ms(unsigned v) { return 1.0 * std::pow(200.0, std::min(v, 100u) / 100.0); }    // 1 .. 200 ms
inline double follower_release_ms(unsigned v) { return 10.0 * std::pow(200.0, std::min(v, 100u) / 100.0); }  // 10 ms .. 2 s

// What a modulator moves: a Motion lane (`param`, `pad`: model/limits.h's motion_* numbers) of track `track`
// (255: none), by `depth` percent (-100..100) of the lane's range (`low`..`high`, as the shell found it when it was
// added): an LFO swings half of that each way, a follower pushes all of it one way.
struct ModTarget {
    uint8_t track = 255, param = 0, pad = 0;
    int8_t depth = 50;
    int16_t low = 0, high = 127;
    int16_t own = 0; // where the lane stands as set, when sent to the audio thread (model/lane_value.h); not saved
    bool used() const { return track < track_count; }
    bool operator==(const ModTarget& o) const {
        return track == o.track && param == o.param && pad == o.pad && depth == o.depth && low == o.low && high == o.high;
    }
};

struct Modulator {
    ModKind kind = ModKind::Off;
    LfoShape shape = LfoShape::Sine;
    bool sync = false;     // on the clock (`division`), else free (`rate`)
    uint8_t rate = 50;     // free: lfo_hertz()
    uint8_t division = 4;  // on the clock: lfo_divisions, 1/4 to start
    uint8_t source = 0;    // a follower's: tracks 0-15, buses 16-19
    uint8_t attack = 20, release = 40;
    std::array<ModTarget, modulator_targets> targets {};
    unsigned target_count() const { unsigned n = 0; for (const auto& t : targets) n += t.used(); return n; }
    bool operator==(const Modulator& o) const {
        return kind == o.kind && shape == o.shape && sync == o.sync && rate == o.rate && division == o.division && source == o.source &&
               attack == o.attack && release == o.release && targets == o.targets;
    }
    bool operator!=(const Modulator& o) const { return !(*this == o); }
};

struct MacroTarget {
    uint8_t track = 255, param = 0, pad = 0;
    int16_t low = 0, high = 127;
    bool used() const { return track < track_count; }
    bool operator==(const MacroTarget& o) const { return track == o.track && param == o.param && pad == o.pad && low == o.low && high == o.high; }
};
struct Macro {
    uint8_t value = 0; // 0..127, where the knob stands
    std::array<MacroTarget, macro_targets> targets {};
    unsigned target_count() const { unsigned n = 0; for (const auto& t : targets) n += t.used(); return n; }
    // Target `t` at the macro's `value`.
    static int at(const MacroTarget& t, int value) { return t.low + int(std::lround(double(t.high - t.low) * std::clamp(value, 0, 127) / 127.0)); }
    bool operator==(const Macro& o) const { return value == o.value && targets == o.targets; }
    bool operator!=(const Macro& o) const { return !(*this == o); }
};

// An LFO's value at `phase` (0..1 of its cycle), -1..1. Hold and Drift read `last` and `next` (random values a cycle,
// drawn by whoever runs it): Hold stays at `next`, Drift glides from `last` to `next` across the cycle.
inline float lfo_value(LfoShape shape, double phase, float last = 0, float next = 0) {
    double p = phase - std::floor(phase);
    switch (shape) {
    case LfoShape::Sine: return float(std::sin(2 * 3.14159265358979323846 * p));
    case LfoShape::Triangle: return float(p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4);
    case LfoShape::SawUp: return float(2 * p - 1);
    case LfoShape::SawDown: return float(1 - 2 * p);
    case LfoShape::Square: return p < 0.5 ? 1.0f : -1.0f;
    case LfoShape::Hold: return next;
    case LfoShape::Drift: { float w = float(0.5 - 0.5 * std::cos(3.14159265358979323846 * p)); return last + (next - last) * w; }
    default: return 0;
    }
}

// A modulated lane: where it stands (`center`), moved by a modulator's `value` (an LFO's -1..1, a follower's 0..1) at
// `depth` percent of the lane's range, kept inside it.
inline int modulated(const ModTarget& t, int center, float value, bool follower) {
    double span = double(t.high - t.low) * t.depth / 100.0;
    double moved = center + (follower ? span * value : span * 0.5 * value);
    int lo = std::min(t.low, t.high), hi = std::max(t.low, t.high);
    return std::clamp(int(std::lround(moved)), lo, hi);
}

} // namespace melodee
