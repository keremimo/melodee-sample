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
    auto* pack = new TrackPack(t, state.track, state.patterns, c.project.motion);
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
    // Motion: the target's lanes go, the source's come with its patterns.
    auto& motion = c.project.motion;
    Motion moved;
    for (unsigned n = 0; n < motion.count; ++n) if (motion.events[n].track != to) moved.write(motion.events[n]);
    bool full = false;
    for (unsigned n = 0; n < motion.count; ++n)
        if (motion.events[n].track == from) { auto e = motion.events[n]; e.track = uint8_t(to); full |= !moved.write(e); }
    motion = moved;
    restore_track(c, to, *next);
    edit.track_after = std::move(next);
    if (std::memcmp(edit.motion_before.get(), &motion, sizeof(Motion)) != 0) edit.motion_after = std::make_unique<Motion>(motion);
    else edit.motion_before.reset();
    c.notice(full ? what + ": motion full" : what);
    c.edits.push(std::move(edit));
}

} // namespace melodee
