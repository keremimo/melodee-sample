// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

// Chop's slices as plain data (parity phase 5; shell/chop.h cuts them), and a
// sample's chop as the project keeps it (roadmap P3, the file's CHOP section):
// the region it cut, its type and amount, and the slices, so Chop on that
// region again finds them as they were left, after a save and a load too.
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

// A sample's chop (roadmap P3): the region of it Chop cut (to 0: the sample's end), how, and the slices.
struct SampleChop {
    uint32_t from = 0, to = 0;
    ChopMode mode = ChopMode::Threshold;
    int16_t amount = 0;
    Slices slices {};
    bool used() const { return slices.count > 0; }
};

} // namespace melodee
