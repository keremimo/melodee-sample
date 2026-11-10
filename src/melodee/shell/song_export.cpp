// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The Song's export (parity phase 6), as Akai's song_mode page has it: B1
// on Song opens it; B1 Cancel, B2 Audio mixdown (the Name sheet, then
// shell/render.cpp plays the song into a WAV file), B3 New pattern.
//
// Song -> new pattern: the first pattern number empty on every track and
// used by no clip takes, track by track, the song's clips laid end to end
// (each clip its pattern repeated from the clip's start, muted clips left
// out) for the song's length, at most 128 bars. One undo. Motion and the
// song's punches stay in the song (a pattern holds neither).
#include "melodee/model/song_flatten.h"
#include "melodee/shell/context.h"
#include <algorithm>

namespace melodee {

void Context::song_export_soft(int key) {
    ui.song_export = false;
    if (key == 1) {
        if (!project.song.length()) { notice("Mixdown: the song is empty"); return; }
        open_name(NameTarget::Mixdown, project.name[0] ? project.name : "Song", sizeof(project.name) - 1, false);
    }
    if (key == 2) song_to_pattern();
}

void Context::song_to_pattern() {
    const auto& song = project.song;
    unsigned bars = song.length();
    if (!bars) { notice("Song: empty"); return; }
    bool cut = bars > max_bars;
    bars = std::min(bars, max_bars);
    uint32_t bar = project.bar_ticks();
    int column = -1;
    for (unsigned p = 0; p < pattern_count && column < 0; ++p) {
        bool free = true;
        for (unsigned t = 0; t < track_count && free; ++t) free = patterns.at[t][p].empty();
        for (unsigned n = 0; n < song.count && free; ++n) free = song.clips[n].pattern != p;
        if (free) column = int(p);
    }
    if (column < 0) { notice("Song: no empty pattern"); return; }
    Edit edit;
    edit.what = "Song to P" + std::to_string(column + 1);
    edit.track = ui.track;
    edit.column = int16_t(column);
    bool full = false;
    for (unsigned t = 0; t < track_count; ++t) {
        Pattern out;
        bool any = flatten_lane(song, t, patterns.at[t], bar, bars, out, full);
        if (!any) continue;
        edit.column_before.emplace_back(uint8_t(t), patterns.at[t][size_t(column)]);
        edit.column_after.emplace_back(uint8_t(t), std::move(out));
    }
    if (edit.column_after.empty()) { notice("Song: no notes"); return; }
    for (const auto& [t, value] : edit.column_after) restore_pattern(t, unsigned(column), value);
    std::string what = edit.what;
    edits.push(std::move(edit));
    dirty = true;
    notice(what + (cut ? ", 128 bars" : full ? ", notes full" : ""));
}

} // namespace melodee
