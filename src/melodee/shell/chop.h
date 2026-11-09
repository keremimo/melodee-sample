// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sample_data.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Chop: where a sample's slices start. HITS keeps the hits at least as
// strong as the sensitivity asks; EVEN cuts it into equal parts; BEATS puts
// a slice on every beat at the project's tempo. The user can then move a
// slice's start, add markers and remove them; the result lands on pads as
// regions of the same file.
namespace melodee {

enum class ChopMode : uint8_t { Hits, Even, Beats, Count };
inline const char* chop_mode_name(ChopMode m) {
    static const char* names[] {"Hits", "Even", "Beats"};
    return names[unsigned(m) % unsigned(ChopMode::Count)];
}
constexpr unsigned max_slices = 64;

struct Slices {
    std::array<uint32_t, max_slices> start {};
    unsigned count = 0;
    uint32_t end(unsigned n, uint32_t frames) const { return n + 1 < count ? start[n + 1] : frames; }
    // Adds a marker, keeping them in order; false when full or already there.
    bool add(uint32_t frame) {
        if (count == start.size()) return false;
        unsigned at = 0;
        while (at < count && start[at] < frame) ++at;
        if (at < count && start[at] == frame) return false;
        for (unsigned n = count; n > at; --n) start[n] = start[n - 1];
        start[at] = frame; ++count;
        return true;
    }
    void remove(unsigned n) {
        if (n == 0 || n >= count) return; // the first slice always starts the sample
        for (unsigned k = n + 1; k < count; ++k) start[k - 1] = start[k];
        --count;
    }
};

// `amount`: HITS sensitivity 0..100 (100 keeps the faintest), EVEN the
// number of slices 2..64, BEATS beats a slice 1..8.
inline Slices chop(ChopMode mode, int amount, uint32_t frames, const std::array<Hit, max_hits>& hits, unsigned hit_count,
                   unsigned tempo_tenths) {
    Slices s;
    if (!frames) return s;
    switch (mode) {
    case ChopMode::Hits: {
        int threshold = 255 - std::clamp(amount, 0, 100) * 255 / 100;
        for (unsigned n = 0; n < hit_count && s.count < max_slices; ++n)
            if (n == 0 || hits[n].strength >= threshold) s.start[s.count++] = hits[n].frame;
        if (!s.count) s.start[s.count++] = 0;
        break;
    }
    case ChopMode::Even: {
        unsigned parts = unsigned(std::clamp(amount, 2, int(max_slices)));
        for (unsigned n = 0; n < parts; ++n) s.start[s.count++] = uint32_t(uint64_t(frames) * n / parts);
        break;
    }
    case ChopMode::Beats: {
        double beat = 44100.0 * 60.0 / (tempo_tenths / 10.0) * std::clamp(amount, 1, 8);
        for (double at = 0; at < frames && s.count < max_slices; at += beat) s.start[s.count++] = uint32_t(at);
        break;
    }
    default: break;
    }
    return s;
}

} // namespace melodee
