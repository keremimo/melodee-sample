// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include <utility>

// Track move (Pad Bank held + B2 MOVE, or Shift + B2 on Tracks; shell/track_copy.cpp): a track goes to another
// slot whole, with its sound, kit, patterns and their motion, and its mix strip. A track already in that slot
// trades places with it, so nothing is lost and the same swap undoes it. What names a track by its number
// follows: the song's clips and punches, the Looper's sources, Knob FX's tracks and pads, the compressor's key.
// Plain data, no Context, so the host tests run it.
namespace melodee {

// Track `t` once a and b traded places.
constexpr unsigned swapped(unsigned t, unsigned a, unsigned b) { return t == a ? b : t == b ? a : t; }

// What a swap changed besides the two tracks themselves: what the audio thread must hear again.
struct SwapChanges {
    bool song = false;    // a clip's lane
    bool punches = false; // a song punch's track
    uint8_t loops = 0;    // a loop's source, a bit a loop
    bool knob = false;    // Knob FX's tracks or a track's pads
    bool key = false;     // the compressor's key track
    bool fx_keys = false; // a Compressor's or Gate's key track in some chain (roadmap P4)
};

inline SwapChanges swap_tracks(Project& project, PatternBank& patterns, unsigned a, unsigned b) {
    SwapChanges out;
    a %= track_count; b %= track_count;
    if (a == b) return out;
    std::swap(project.tracks[a], project.tracks[b]);
    std::swap(patterns.at[a], patterns.at[b]);
    auto& song = project.song;
    for (unsigned n = 0; n < song.count; ++n) {
        auto& clip = song.clips[n];
        unsigned t = swapped(clip.track, a, b);
        if (t != clip.track) { clip.track = uint8_t(t); out.song = true; }
    }
    for (unsigned n = 0; n < song.punch_count; ++n) {
        auto& punch = song.punches[n];
        if (punch.target >= track_count) continue; // the master
        unsigned t = swapped(punch.target, a, b);
        if (t != punch.target) { punch.target = uint8_t(t); out.punches = true; }
    }
    for (unsigned l = 0; l < loop_tracks; ++l) {
        auto& source = project.looper.loops[l].source;
        if (!looper::source_is_track(source)) continue;
        unsigned t = swapped(source - 1u, a, b);
        if (t != source - 1u) { source = looper::source_track(t); out.loops = uint8_t(out.loops | 1u << l); }
    }
    auto& k = project.knob_fx;
    if (k.track_on(a) != k.track_on(b)) {
        bool on_a = k.track_on(a);
        k.set_track(a, k.track_on(b)); k.set_track(b, on_a);
        out.knob = true;
    }
    if (k.pads[a] != k.pads[b]) { std::swap(k.pads[a], k.pads[b]); out.knob = true; }
    unsigned key = swapped(project.compressor.key, a, b);
    if (key != project.compressor.key) { project.compressor.key = uint8_t(key); out.key = true; }
    for (unsigned n = 0; n < chain_count; ++n) // roadmap P4: a keyed Compressor or Gate follows its key track, in every chain
        for (auto& s : project.chain(n).slots)
            if (fx_key_is_track(s.key)) {
                unsigned t = swapped(s.key - 1u, a, b);
                if (t != s.key - 1u) { s.key = fx_key_track(t); out.fx_keys = true; }
            }
    return out;
}

// What the screen keeps for each track (Edit's page and row, Stage's button and page, the Seq editor, a MIDI
// track's last send) goes with it, and the track in hand stays the one it was.
inline void swap_track_views(UiState& ui, unsigned a, unsigned b) {
    a %= track_count; b %= track_count;
    if (a == b) return;
    for (auto* v : {&ui.edit_page, &ui.edit_row, &ui.stage_group, &ui.stage_sub, &ui.seq_editor}) std::swap((*v)[a], (*v)[b]);
    std::swap(ui.track_out_at[a], ui.track_out_at[b]);
    ui.track = uint8_t(swapped(ui.track, a, b));
}

} // namespace melodee
