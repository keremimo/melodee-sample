// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <algorithm>
#include <cstdint>

// An audio track's take on Song (shell/audio_jobs.cpp): Seq Record with an audio track's lane picked and the song
// playing makes a clip where the take starts, of the track's first empty pattern no clip plays. While the take runs the
// clip holds the bar after the one playing (what was there gives way, as a launch written into the song does); the take
// ended, it stops with the last bar heard, and its pattern is as long as it. Pure, for the shell and the tests.
namespace melodee {

// The track's first pattern with nothing in it that no clip on its lane plays (-1: none).
inline int song_take_pattern(const Song& song, const PatternBank& bank, unsigned track) {
    track %= track_count;
    for (unsigned p = 0; p < pattern_count; ++p) {
        if (!bank.at[track][p].empty()) continue;
        bool placed = false;
        for (unsigned n = 0; n < song.count && !placed; ++n) placed = song.clips[n].track == track && song.clips[n].pattern == p;
        if (!placed) return int(p);
    }
    return -1;
}

// The take's clip: `pattern` on `track`'s lane from `bar` (-1: gone).
inline int song_take_clip(const Song& song, unsigned track, unsigned pattern, unsigned bar) {
    for (unsigned n = 0; n < song.count; ++n) {
        const auto& c = song.clips[n];
        if (c.track == track && c.pattern == pattern && c.bar == bar) return int(n);
    }
    return -1;
}

// The song on bar `playing`: the clip reaches the bar after it (no further than max_bars, nor past the song's last
// bar, nor past the loop's end while the loop is ahead: the bar after it is the loop's first). False: nothing changed.
inline bool song_take_grow(Song& song, unsigned track, unsigned pattern, unsigned bar, unsigned playing) {
    int n = song_take_clip(song, track, pattern, bar);
    if (n < 0) return false;
    const Clip c = song.clips[size_t(n)];
    unsigned end = std::min<unsigned>({playing + 2, unsigned(c.bar) + max_bars, song_bars});
    if (song.looping() && playing < song.loop_end) end = std::min<unsigned>(end, song.loop_end);
    if (end <= c.end()) return false;
    song.cut(track, c.end(), end); // clips after it move in the list
    n = song_take_clip(song, track, pattern, bar);
    if (n < 0) return false;
    song.clips[size_t(n)].bars = uint16_t(end - c.bar);
    return true;
}

// The take ended with bar `last` the last one heard: the clip ends with it, a bar long at least. Its bars (0: gone).
inline unsigned song_take_close(Song& song, unsigned track, unsigned pattern, unsigned bar, unsigned last) {
    const int n = song_take_clip(song, track, pattern, bar);
    if (n < 0) return 0;
    auto& c = song.clips[size_t(n)];
    const unsigned heard = last >= bar ? last + 1 - bar : 1;
    c.bars = uint16_t(std::clamp<unsigned>(heard, 1, c.bars));
    return c.bars;
}

} // namespace melodee
