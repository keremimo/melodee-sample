// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include "melodee/model/project.h"
#include <algorithm>
#include <array>

// Song -> new pattern (parity phase 6): one track's lane of the song laid
// out as a single pattern of `bars` bars. Each clip plays its pattern from
// the clip's first bar, over and over, until the clip ends; muted clips and
// notes past a pattern's own end (never heard) are left out. Returns
// whether the lane had any notes; `full` is set when the pattern ran out of room.
namespace melodee {

inline bool flatten_lane(const Song& song, unsigned track, const std::array<Pattern, pattern_count>& patterns, uint32_t bar_ticks,
                         unsigned bars, Pattern& out, bool& full) {
    out = Pattern {};
    out.length = std::max(1u, bars) * bar_ticks;
    bool any = false;
    for (unsigned n = 0; n < song.count; ++n) {
        const auto& clip = song.clips[n];
        if (clip.track != track || clip.muted) continue;
        const auto& source = patterns[clip.pattern % pattern_count];
        if (source.empty()) continue;
        if (!any) out.division = source.division;
        any = true;
        uint32_t length = std::max<uint32_t>(source.length, 1), from = clip.bar * bar_ticks;
        uint32_t to = std::min<uint32_t>(clip.end() * bar_ticks, out.length);
        for (uint32_t round = from; round < to; round += length)
            for (const auto& e : source.events) {
                if (e.tick >= length || round + e.tick >= to) break;
                Event placed = e;
                placed.tick = round + e.tick;
                if (!out.add(placed)) full = true;
            }
    }
    return any;
}

} // namespace melodee
