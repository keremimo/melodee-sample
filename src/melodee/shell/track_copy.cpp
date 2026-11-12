// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/track_copy.h"
#include "melodee/engine/engine_host.h"
#include "melodee/shell/context.h"
#include <cstring>

namespace melodee {

TrackState track_state(const Context& c, unsigned t) {
    t %= track_count;
    return {c.project.tracks[t], c.patterns.at[t]};
}

void restore_track(Context& c, unsigned t, const TrackState& state) {
    t %= track_count;
    c.project.tracks[t] = state.track;
    c.patterns.at[t] = state.patterns;
    c.free_retired();
    auto* pack = new TrackPack(t, state.track, state.patterns); // their motion in them
    Command load {Command::Type::TrackLoad, uint8_t(t)};
    load.track_pack = pack;
    if (!c.host.send(load)) delete pack;
    c.dirty = true;
}

void copy_track(Context& c, unsigned from, unsigned to) {
    from %= track_count; to %= track_count;
    if (from == to) return;
    if (c.ui.recording) { c.notice("Copy track: not in a take"); return; }
    std::string what = "Copy track " + std::to_string(from + 1) + " to " + std::to_string(to + 1);
    Edit edit = c.begin_edit(what.c_str(), to, -1, false);
    edit.track_before = std::make_unique<TrackState>(track_state(c, to));
    auto next = std::make_unique<TrackState>(track_state(c, from));
    next->track.mix = c.project.tracks[to].mix; // the target keeps its strip
    restore_track(c, to, *next); // the source's motion comes in its patterns
    edit.track_after = std::move(next);
    c.notice(what);
    c.edits.push(std::move(edit));
}

void clear_track(Context& c, unsigned t) {
    t %= track_count;
    std::string name = "Track " + std::to_string(t + 1);
    if (c.ui.recording) { c.notice("Clear track: not in a take"); return; }
    if (!track_clearable(c.project.tracks[t], c.patterns.at[t])) { c.notice(name + " is empty"); return; }
    std::string what = "Clear track " + std::to_string(t + 1);
    Edit edit = c.begin_edit(what.c_str(), t, -1, false);
    edit.track_before = std::make_unique<TrackState>(track_state(c, t));
    auto next = std::make_unique<TrackState>();
    next->track.mix = c.project.tracks[t].mix; // the strip stays
    for (auto& p : next->patterns) p.length = c.project.bar_ticks();
    restore_track(c, t, *next);
    edit.track_after = std::move(next);
    c.notice(name + " cleared");
    c.edits.push(std::move(edit));
}

} // namespace melodee
