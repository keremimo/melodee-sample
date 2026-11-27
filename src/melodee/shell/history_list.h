// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/history.h"
#include <cstdint>
#include <string>

// The undo list (ease roadmap item 2): Shift + Minus held history_hold_ms opens the steps kept (History: 512, within its memory budget), newest
// on top. A tap still undoes one step, on its release. The rows, top down: the steps undone (redo's, the newest
// first), then the steps done (undo's, newest first), then START, the state before them all. The cursor opens on
// NOW, the state in hand: the first step done, or START when every step is undone. The wheel, Minus / Plus move it;
// Push or B3 goes to the state after the step under it (undo or redo as many as it takes), B1 BACK closes. Any
// other button closes it and does its own job; Play, Stop and Shift leave it open. The rules here are pure, for the
// shell, the screen and the tests; the jobs are in shell/history_jobs.cpp.
namespace melodee {

constexpr uint32_t history_hold_ms = 400;

// Shift + Minus: a tap (undo, on its release) or a hold (the list).
struct HistoryHold {
    bool down = false, fired = false;
    uint32_t at = 0;
};
inline void history_press(HistoryHold& h, uint32_t now) { h = HistoryHold {true, false, now}; }
// Each pass while it is down: true once, when it has been held long enough (the list opens).
inline bool history_tick(HistoryHold& h, uint32_t now) {
    if (!h.down || h.fired || now - h.at < history_hold_ms) return false;
    h.fired = true;
    return true;
}
// Let go: true when it was a tap (one undo).
inline bool history_release(HistoryHold& h) {
    bool tap = h.down && !h.fired;
    h.down = false;
    return tap;
}

// How many rows: the steps undone, the steps done, START.
inline unsigned history_rows(unsigned redos, unsigned kept) { return redos + kept + 1; }
// The row of the state in hand.
inline unsigned history_now(unsigned redos) { return redos; }
// Going to row `row`: steps to redo (> 0) or to undo (< 0); 0 stays.
inline int history_step(unsigned row, unsigned redos) { return row < redos ? int(redos - row) : -int(row - redos); }
// A row is drawn dim when its step is undone once the cursor's state is reached: every row above the cursor.
inline bool history_row_dim(unsigned row, unsigned cursor) { return row < cursor; }
// Row `row`'s step: true with `redo` set when it is one undone (History::undone(index)), else one done
// (History::kept(index)); false for START.
inline bool history_edit_at(unsigned row, unsigned redos, unsigned kept, bool& redo, unsigned& index) {
    if (row < redos) { redo = true; index = row; return true; }
    if (row - redos < kept) { redo = false; index = kept - 1 - (row - redos); return true; }
    return false;
}

// How long ago a step was made, as the row's fact: "now" under a minute, then minutes, then hours.
inline std::string history_age(uint32_t now, uint32_t at) {
    uint32_t seconds = (now - at) / 1000;
    if (seconds < 60) return "now";
    if (seconds < 3600) return std::to_string(seconds / 60) + " min";
    return std::to_string(seconds / 3600) + " h";
}

// What kind of step it is, for its row's glyph.
enum class HistoryKind : uint8_t { Other, Pad, Pattern, Sound, Mix, Effects, Chop, Tempo, Song, Track, Plugin };
inline HistoryKind history_kind(const Edit& e) {
    if (e.plugin) return HistoryKind::Plugin;
    if (e.track_before || e.swap_a >= 0 || e.name_before) return HistoryKind::Track;
    if (e.chop_before) return HistoryKind::Chop;
    if (e.song_before) return HistoryKind::Song;
    if (e.tempo_before) return HistoryKind::Tempo;
    if (e.pattern >= 0 || e.column >= 0 || !e.patterns_before.empty() || e.scene_before) return HistoryKind::Pattern;
    if (e.pad >= 0 || !e.pads_before.empty()) return HistoryKind::Pad;
    if (e.chain >= 0 || e.master_before) return HistoryKind::Effects;
    if (e.mix_before) return HistoryKind::Mix;
    if (e.sound_before) return HistoryKind::Sound;
    return HistoryKind::Other;
}

// A row as the screen draws it (Snapshot::history), copied from the History while the list is open.
struct HistoryRow {
    std::string what;
    uint8_t track = 0;
    uint32_t at = 0;
    HistoryKind kind = HistoryKind::Other;
};

} // namespace melodee
