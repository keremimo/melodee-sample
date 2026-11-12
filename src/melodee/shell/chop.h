// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/onsets.h"
#include "melodee/engine/sample_data.h"
#include "melodee/model/chop_slices.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Chop: where the slices of a pad's region start and end, as the MPC's Chop
// Mode cuts it (parity phase 5). THRESHOLD keeps the hits at least as strong
// as the threshold asks; REGIONS cuts it into equal parts; BEATS puts a
// slice on every beat (or every 2 ... 8) from the region's start at the
// sample's own tempo, each cut on its tracked beat or a hit near it, or at the
// project's tempo where the sample has none (Melodee's); MANUAL is what the
// pads tapped while it played, or what was edited by hand. Slices touch: one
// ends where the next starts, the last at `last`. The result lands on pads as
// regions of the same file, or one slice as a file of its own (Extract).
namespace melodee {

// The cut nearest a tapped frame: the strongest hit from 50 ms before it to
// 10 ms after, else the tap itself (a tap comes a little after the sound).
inline uint32_t snap_to_hit(uint32_t frame, const std::array<Hit, max_hits>& hits, unsigned hit_count) {
    uint32_t from = frame > 2205 ? frame - 2205 : 0, to = frame + 441;
    int best = -1;
    for (unsigned n = 0; n < hit_count; ++n)
        if (hits[n].frame >= from && hits[n].frame <= to && (best < 0 || hits[n].strength > hits[size_t(best)].strength)) best = int(n);
    return best < 0 ? frame : hits[size_t(best)].frame;
}

// Zero Snap (Melodee's, on unless turned off on Chop's B2): a cut pulled onto
// the nearest place the wave crosses zero, so a slice starts and stops
// without a click. The two sides are summed (left and right cross at
// different places; the sum is what a mono listener hears). It looks up to
// 5 ms before the cut and 1 ms after, and a crossing after counts double the
// distance: a cut sits on or just before a hit, and moving it into the hit
// would blunt it. Where nothing crosses (a DC offset, a deep bass held over
// the window) it takes the frame nearest zero. The slice edges' 2 ms fades
// (engine/sampler.cpp, Extract) catch what is left.
constexpr uint32_t zero_back = 220, zero_ahead = 44; // 5 ms and 1 ms at 44.1 kHz
inline uint32_t zero_crossing(const float* left, const float* right, uint32_t frames, uint32_t frame) {
    if (!frames || frame >= frames) return frame;
    auto sum = [&](uint32_t i) { return left[i] + right[i]; };
    uint32_t lo = frame > zero_back ? frame - zero_back : 0, hi = std::min(frames - 1, frame + zero_ahead);
    auto cost = [&](uint32_t i) { return i < frame ? frame - i : 2 * (i - frame); };
    uint32_t best = frame, quiet = frame;
    bool found = false;
    for (uint32_t i = lo; i <= hi; ++i) {
        float m = sum(i);
        if (std::fabs(m) < std::fabs(sum(quiet)) || (std::fabs(m) == std::fabs(sum(quiet)) && cost(i) < cost(quiet))) quiet = i;
        // A crossing between i - 1 and i (or a zero on i): the side of it nearer zero.
        if (i == 0) continue;
        float before = sum(i - 1);
        if (!(m == 0 || (before < 0) != (m < 0))) continue;
        uint32_t at = m == 0 || std::fabs(m) <= std::fabs(before) || i - 1 < lo ? i : i - 1;
        if (!found || cost(at) < cost(best)) { best = at; found = true; }
    }
    return found ? best : quiet;
}
inline uint32_t zero_crossing(const SampleData& sample, uint32_t frame) {
    return zero_crossing(sample.left.data(), sample.right.data(), std::min<uint32_t>(sample.frames, uint32_t(sample.left.size())), frame);
}

// The wheel's Zero Snap: the first crossing at or past `frame` going `way` (+1 later, -1 earlier), within 50 ms;
// `frame` itself where none is. The frame of the pair nearer zero, as above, but never back past `frame`.
constexpr uint32_t zero_reach = 2205;
inline uint32_t zero_crossing_toward(const float* left, const float* right, uint32_t frames, uint32_t frame, int way) {
    if (!frames || frame >= frames) return frame;
    auto sum = [&](uint32_t i) { return left[i] + right[i]; };
    for (uint32_t d = 0; d <= zero_reach; ++d) {
        int64_t i = way < 0 ? int64_t(frame) - d : int64_t(frame) + d;
        if (i < 1 || i >= int64_t(frames)) break;
        float m = sum(uint32_t(i)), before = sum(uint32_t(i - 1));
        if (!(m == 0 || (before < 0) != (m < 0))) continue;
        // The crossing lies between i - 1 and i: the side nearer zero, unless that steps back past `frame`.
        uint32_t at = m == 0 || std::fabs(m) <= std::fabs(before) ? uint32_t(i) : uint32_t(i - 1);
        if (way > 0 && at < frame) at = uint32_t(i);
        if (way < 0 && at > frame) at = uint32_t(i - 1);
        return at;
    }
    return frame;
}
inline uint32_t zero_crossing_toward(const SampleData& sample, uint32_t frame, int way) {
    return zero_crossing_toward(sample.left.data(), sample.right.data(), std::min<uint32_t>(sample.frames, uint32_t(sample.left.size())), frame,
                                way);
}

// The cut for a beat expected at `want`, a `step` after the cut before (`beat` frames a beat): the sample's tracked
// beat within an eighth of a beat of it and within 6% of the step (so the slices follow a take that drifts, yet a
// tracker that found the off-beats, a 16th or more away, or squeezed its last beats to reach a hit, never pulls
// them off the region's own beat), else the hit within 30 ms, else `want` itself. `tracked`: it was a beat.
inline uint32_t beat_cut(double want, double step, double beat, const BeatGrid& grid, const std::array<Hit, max_hits>& hits,
                         unsigned hit_count, bool& tracked) {
    auto gap = [&](uint32_t frame) { return std::fabs(double(frame) - want); };
    tracked = false;
    auto b = std::lower_bound(grid.at.begin(), grid.at.end(), uint32_t(want));
    uint32_t best = uint32_t(want);
    double nearest = std::min(beat / 8, step * 0.06);
    for (auto it : {b, b == grid.at.begin() ? b : b - 1})
        if (it != grid.at.end() && gap(*it) <= nearest) { nearest = gap(*it); best = *it; tracked = true; }
    if (tracked) return best;
    nearest = 1323; // 30 ms
    for (unsigned n = 0; n < hit_count; ++n)
        if (hits[n].frame && gap(hits[n].frame) <= nearest) { nearest = gap(hits[n].frame); best = hits[n].frame; }
    return best;
}

// The slices of [from, to) of a sample of `frames`. `amount`: THRESHOLD
// 0..100 (as on the MPC, the higher, the fewer slices: 0 keeps every hit), REGIONS the number of slices 2..64, BEATS
// beats a slice 1..8 (`grid`: the sample's own, else `tempo_tenths`, the project's). MANUAL starts as one slice, the whole region.
inline Slices chop(ChopMode mode, int amount, uint32_t from, uint32_t to, uint32_t frames, const std::array<Hit, max_hits>& hits,
                   unsigned hit_count, unsigned tempo_tenths, const BeatGrid* grid = nullptr) {
    Slices s;
    if (!frames) return s;
    to = to && to <= frames ? to : frames;
    from = std::min(from, to > min_slice ? to - min_slice : 0);
    s.start[s.count++] = from;
    s.last = to;
    switch (mode) {
    case ChopMode::Threshold: {
        int threshold = std::clamp(amount, 0, 100) * 255 / 100;
        for (unsigned n = 0; n < hit_count; ++n)
            if (hits[n].frame > from && hits[n].strength >= threshold) s.add(hits[n].frame, frames);
        break;
    }
    case ChopMode::Regions: {
        unsigned parts = unsigned(std::clamp(amount, 2, int(max_slices)));
        for (unsigned n = 1; n < parts; ++n) s.add(from + uint32_t(uint64_t(to - from) * n / parts), frames);
        break;
    }
    case ChopMode::Beats: {
        if (grid && grid->found()) { // a slice every `amount` of the sample's beats, from where the region starts
            const double beat = 44100.0 * 600 / grid->tempo, step = beat * std::clamp(amount, 1, 8);
            const double reach = std::min(beat / 8, step * 0.06); // a beat this near the end is the end
            for (double at = from + step; at + reach < to;) {
                bool tracked = false;
                uint32_t cut = beat_cut(at, step, beat, *grid, hits, hit_count, tracked);
                if (cut >= to) break;
                s.add(cut, frames);
                at = (tracked ? cut : at) + step; // the next from a tracked beat; from the grid past a hit
            }
            break;
        }
        double beat = 44100.0 * 60.0 / (std::max(tempo_tenths, 10u) / 10.0) * std::clamp(amount, 1, 8);
        for (double at = from + beat; at < to; at += beat) s.add(uint32_t(at), frames);
        break;
    }
    default: break;
    }
    return s;
}
// The whole sample, as before parity phase 5.
inline Slices chop(ChopMode mode, int amount, uint32_t frames, const std::array<Hit, max_hits>& hits, unsigned hit_count, unsigned tempo_tenths,
                   const BeatGrid* grid = nullptr) {
    return chop(mode, amount, 0, frames, frames, hits, hit_count, tempo_tenths, grid);
}

} // namespace melodee
