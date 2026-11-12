// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Roadmap P5, always-on capture, on round 2's Shift + Seq Record (Recall,
// capture C): every note played on any track (the pads, the keys, MIDI in)
// goes into a ring made once (shell/capture_ring.h), Record on or not; where
// the transport runs it is placed as Seq Record would place it (its stamp
// less the output latency, on the nearest tick). Shift + Seq Record lays the
// last of it into patterns after the fact: the phrase (or as many bars as
// Settings keeps) the same bars for every track that played in it, each
// track's into its next empty pattern, which then plays; one undo for all.
#include "melodee/shell/context.h"
#include "melodee/engine/engine_host.h"
#include <algorithm>
#include <string>

namespace melodee {
using Type = Command::Type;

// The transport's runs: a new one when it starts, and when it jumps back (Locate, the song's loop, a new Play).
void Context::capture_tick() {
    bool running = host.playing() && !host.count_in();
    uint32_t ticks = host.ticks();
    if (!running) { capture_run = 0; return; }
    if (!capture_run || ticks < capture_ticks) capture_run = uint16_t(capture_run_next++ % 0xffff + 1);
    capture_ticks = ticks;
}

void Context::keep_played(unsigned t, int k, int velocity, bool down) {
    capture_tick(); // the run this note falls in, even between the loop's looks
    int position = 0;
    uint32_t ticks = 0;
    bool placed = capture_run && host.heard_place(t, hit_ns, position, ticks);
    if (capture_run && !placed) ticks = host.ticks();
    if (down) capture_ring->press(t, k, velocity, now, capture_run, ticks);
    else capture_ring->release(t, k, now, ticks);
}

void Context::capture() {
    const auto& set = ui.settings;
    CaptureAsk ask;
    ask.bar = std::max(1u, project.bar_ticks());
    ask.bars = set.capture_bars;
    ask.grid = set.capture_quantise == 0 ? 1 : set.capture_quantise == 1 ? sixteenth_ticks : 2 * sixteenth_ticks;
    ask.tick_ms = 60000.0 / (std::max(tempo_now(), 300u) / 10.0) / ticks_per_quarter;
    ask.now = now;
    ask.ticks = host.ticks();
    ask.tracks = 0;
    for (unsigned t = 0; t < track_count; ++t) if (project.tracks[t].used()) ask.tracks |= uint16_t(1u << t);
    CaptureWindow window = capture_window(*capture_ring, ask);
    if (!window.any) { notice("Nothing played to capture"); return; }
    Edit edit;
    edit.what = "Capture";
    edit.track = ui.track;
    unsigned full = 0;
    int selected = -1;
    for (unsigned k = 0; k < track_count; ++k) {
        unsigned t = (ui.track + k) % track_count; // the selected track first
        if (!(ask.tracks >> t & 1)) continue;
        Pattern laid;
        if (!lay_capture(*capture_ring, t, window, ask, laid)) continue;
        int into = -1; // the next empty pattern after the one playing, round to the first
        unsigned from = project.tracks[t].pattern;
        for (unsigned n = 1; n <= pattern_count && into < 0; ++n)
            if (patterns.at[t][(from + n) % pattern_count].empty()) into = int((from + n) % pattern_count);
        if (into < 0) { ++full; continue; }
        laid.feel = patterns.at[t][size_t(into)].feel; // an empty pattern's own settings stay
        laid.follow = patterns.at[t][size_t(into)].follow;
        if (t == ui.track) selected = into;
        edit.patterns_before.push_back({uint8_t(t), uint8_t(into), patterns.at[t][size_t(into)]});
        edit.patterns_after.push_back({uint8_t(t), uint8_t(into), std::move(laid)});
    }
    if (edit.patterns_after.empty()) { notice(full ? "No empty pattern to capture into" : "Nothing played to capture"); return; }
    for (const auto& p : edit.patterns_after) {
        restore_pattern(p.track, p.pattern, p.value);
        launch(p.track, p.pattern); // and it plays from here (the next bar while playing)
    }
    unsigned count = unsigned(edit.patterns_after.size());
    ui.captured_pattern = uint8_t(selected >= 0 ? selected : edit.patterns_after.front().pattern);
    ui.captured_tracks = uint8_t(count);
    ui.captured_at = now ? now : 1;
    edits.push(std::move(edit));
    dirty = true;
    if (full) notice(std::to_string(full) + (full == 1 ? " track had" : " tracks had") + " no empty pattern");
}

} // namespace melodee
