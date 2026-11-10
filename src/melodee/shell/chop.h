// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sample_data.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Chop: where the slices of a pad's region start and end, as the MPC's Chop
// Mode cuts it (parity phase 5). THRESHOLD keeps the hits at least as strong
// as the threshold asks; REGIONS cuts it into equal parts; BEATS puts a
// slice on every beat at the project's tempo (Melodee's); MANUAL is what the
// pads tapped while it played, or what was edited by hand. Slices touch: one
// ends where the next starts, the last at `last`. The result lands on pads as
// regions of the same file, or one slice as a file of its own (Extract).
namespace melodee {

enum class ChopMode : uint8_t { Threshold, Regions, Beats, Manual, Count };
inline const char* chop_mode_name(ChopMode m) {
    static const char* names[] {"Threshold", "Regions", "Beats", "Manual"};
    return names[unsigned(m) % unsigned(ChopMode::Count)];
}
constexpr unsigned max_slices = 64;
constexpr unsigned manual_slices = 16;   // the MPC's: "up to 16 slices per sample"
constexpr uint32_t min_slice = 882;      // 20 ms: no slice shorter

struct Slices {
    std::array<uint32_t, max_slices> start {};
    unsigned count = 0;
    uint32_t last = 0; // where the last slice ends; 0: the sample's end
    uint32_t end(unsigned n, uint32_t frames) const {
        if (n + 1 < count) return start[n + 1];
        return last && last <= frames ? last : frames;
    }
    // Adds a cut inside the slices, keeping them in order; false when full,
    // already there or too close to a neighbour.
    bool add(uint32_t frame, uint32_t frames, unsigned most = max_slices) {
        if (!count || count >= std::min<unsigned>(most, max_slices) || frame <= start[0]) return false;
        if (frame + min_slice > end(count - 1, frames)) return false;
        unsigned at = 0;
        while (at < count && start[at] < frame) ++at;
        if (start[at - 1] + min_slice > frame || (at < count && frame + min_slice > start[at])) return false;
        for (unsigned n = count; n > at; --n) start[n] = start[n - 1];
        start[at] = frame; ++count;
        return true;
    }
    // Takes slice n away; what it held joins the slice before (the MPC's
    // Merge, and Erase + pad). The first slice has none before it.
    void remove(unsigned n) {
        if (n == 0 || n >= count) return; // the slice before ends where this one did; `last` stays
        for (unsigned k = n + 1; k < count; ++k) start[k - 1] = start[k];
        --count;
    }
    // Slice n cut in half (Split); false when it is too short or the slices are full.
    bool split(unsigned n, uint32_t frames) {
        if (n >= count) return false;
        uint32_t a = start[n], b = end(n, frames);
        return b - a >= 2 * min_slice && add(a + (b - a) / 2, frames);
    }
    // K1 and K2: slice n's start or end, which is its neighbour's end or start too.
    void move_start(unsigned n, uint32_t frame, uint32_t frames) {
        if (n >= count) return;
        uint32_t lo = n ? start[n - 1] + min_slice : 0, hi = end(n, frames);
        hi = hi > min_slice ? hi - min_slice : 0;
        start[n] = std::clamp(frame, lo, std::max(lo, hi));
    }
    void move_end(unsigned n, uint32_t frame, uint32_t frames) {
        if (n >= count) return;
        uint32_t lo = start[n] + min_slice;
        if (n + 1 < count) {
            uint32_t hi = end(n + 1, frames);
            hi = hi > min_slice ? hi - min_slice : 0;
            start[n + 1] = std::clamp(frame, lo, std::max(lo, hi));
        } else last = std::clamp(frame, std::min(lo, frames), frames);
    }
};

// The cut nearest a tapped frame: the strongest hit from 50 ms before it to
// 10 ms after, else the tap itself (a tap comes a little after the sound).
inline uint32_t snap_to_hit(uint32_t frame, const std::array<Hit, max_hits>& hits, unsigned hit_count) {
    uint32_t from = frame > 2205 ? frame - 2205 : 0, to = frame + 441;
    int best = -1;
    for (unsigned n = 0; n < hit_count; ++n)
        if (hits[n].frame >= from && hits[n].frame <= to && (best < 0 || hits[n].strength > hits[size_t(best)].strength)) best = int(n);
    return best < 0 ? frame : hits[size_t(best)].frame;
}

// The slices of [from, to) of a sample of `frames`. `amount`: THRESHOLD
// 0..100 (as on the MPC, the higher, the fewer slices: 0 keeps every hit), REGIONS the number of slices 2..64, BEATS
// beats a slice 1..8. MANUAL starts as one slice, the whole region.
inline Slices chop(ChopMode mode, int amount, uint32_t from, uint32_t to, uint32_t frames, const std::array<Hit, max_hits>& hits,
                   unsigned hit_count, unsigned tempo_tenths) {
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
        double beat = 44100.0 * 60.0 / (std::max(tempo_tenths, 10u) / 10.0) * std::clamp(amount, 1, 8);
        for (double at = from + beat; at < to; at += beat) s.add(uint32_t(at), frames);
        break;
    }
    default: break;
    }
    return s;
}
// The whole sample, as before parity phase 5.
inline Slices chop(ChopMode mode, int amount, uint32_t frames, const std::array<Hit, max_hits>& hits, unsigned hit_count, unsigned tempo_tenths) {
    return chop(mode, amount, 0, frames, frames, hits, hit_count, tempo_tenths);
}

} // namespace melodee
