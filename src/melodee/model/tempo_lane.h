// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Roadmap P5: the song's tempo lane (Song, Shift + K1 / K2). Points on bars, each a tempo (tenths of a BPM) the song
// steps to on its bar, or ramps to from the point before (the first from bar 1 at the project's tempo), evenly across
// the bars between. Before the first point, and with none, the project's tempo. The audio thread follows it a tick
// at a time while the song plays (EngineHost::tick), so everything on the clock does: the tempo-synced effects and
// LFOs, the arps, the click, MIDI clock out, Seq Record's places, a mixdown and its stems.
namespace melodee {

struct TempoPoint {
    uint16_t bar = 0;     // from the song's start, 0 the first bar
    uint16_t tempo = 1200; // tenths of a BPM, 30.0 .. 300.0
    bool ramp = false;    // reached evenly from the point before (else stepped to on its bar)
    bool operator==(const TempoPoint& o) const { return bar == o.bar && tempo == o.tempo && ramp == o.ramp; }
};

struct TempoLane {
    std::array<TempoPoint, song_tempo_points> points {};
    uint8_t count = 0; // by bar, at most one a bar
    bool empty() const { return !count; }
    bool operator==(const TempoLane& o) const {
        if (count != o.count) return false;
        for (unsigned n = 0; n < count; ++n) if (!(points[n] == o.points[n])) return false;
        return true;
    }
    int at_bar(unsigned bar) const {
        for (unsigned n = 0; n < count; ++n) if (points[n].bar == bar) return int(n);
        return -1;
    }
    // Puts a point on its bar (replacing one there), kept in order; false when full.
    bool set(unsigned bar, unsigned tempo, bool ramp) {
        TempoPoint p {uint16_t(std::min(bar, song_bars - 1)), uint16_t(std::clamp(tempo, 300u, 3000u)), ramp};
        if (int at = at_bar(p.bar); at >= 0) { points[size_t(at)] = p; return true; }
        if (count == points.size()) return false;
        unsigned n = count++;
        while (n > 0 && points[n - 1].bar > p.bar) { points[n] = points[n - 1]; --n; }
        points[n] = p;
        return true;
    }
    bool erase(unsigned bar) {
        int at = at_bar(bar);
        if (at < 0) return false;
        for (unsigned n = unsigned(at) + 1; n < count; ++n) points[n - 1] = points[n];
        --count;
        return true;
    }
    // The tempo (tenths) `bars` into the song (a fraction of a bar between bar lines); `base` the project's.
    double at(double bars, unsigned base) const {
        double from_bar = 0, from = base;
        for (unsigned n = 0; n < count; ++n) {
            const auto& p = points[n];
            if (bars < p.bar) {
                if (p.ramp && p.bar > from_bar) return from + (p.tempo - from) * (bars - from_bar) / (p.bar - from_bar);
                return from;
            }
            from_bar = p.bar; from = p.tempo;
        }
        return from;
    }
    // Seconds from the song's start to `bars` into it, a bar `quarters` quarter notes long: a stepped stretch at its
    // tempo, a ramp as tempo rising evenly with the bars (its seconds the logarithm's).
    double seconds(double bars, double quarters, unsigned base) const {
        auto span = [&](double a, double b, double t0, double t1) { // bars a..b, tempo t0 at a to t1 at b (tenths)
            if (b <= a) return 0.0;
            double per_bar = quarters * 600.0; // seconds a bar at a tempo of one tenth of a BPM
            if (std::fabs(t1 - t0) < 1e-9) return (b - a) * per_bar / t0;
            return (b - a) * per_bar * std::log(t1 / t0) / (t1 - t0);
        };
        double total = 0, from_bar = 0, from = base;
        for (unsigned n = 0; n < count && from_bar < bars; ++n) {
            const auto& p = points[n];
            double to = std::min<double>(bars, p.bar);
            if (p.ramp && p.bar > from_bar) total += span(from_bar, to, from, from + (p.tempo - from) * (to - from_bar) / (p.bar - from_bar));
            else total += span(from_bar, to, from, from);
            from_bar = p.bar; from = p.tempo;
        }
        if (bars > from_bar) total += span(from_bar, bars, from, from);
        return total;
    }
    // And back: the bars into the song `seconds` reach (MTC in places the song by it).
    double bars_at(double seconds_in, double quarters, unsigned base) const {
        double lo = 0, hi = 1;
        while (seconds(hi, quarters, base) < seconds_in && hi < 1e6) hi *= 2;
        for (unsigned k = 0; k < 60; ++k) {
            double mid = (lo + hi) / 2;
            (seconds(mid, quarters, base) < seconds_in ? lo : hi) = mid;
        }
        return (lo + hi) / 2;
    }
};

} // namespace melodee
