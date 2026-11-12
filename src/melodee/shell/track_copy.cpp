// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/track_copy.h"
#include "melodee/engine/engine_host.h"
#include "melodee/shell/context.h"
#include "melodee/shell/track_swap.h"
#include <cstring>

namespace melodee {

namespace {
// Track t as it is now, whole, to the audio thread (its patterns' motion in them).
void send_track(Context& c, unsigned t) {
    auto* pack = new TrackPack(t, c.project.tracks[t], c.patterns.at[t]);
    Command load {Command::Type::TrackLoad, uint8_t(t)};
    load.track_pack = pack;
    if (!c.host.send(load)) delete pack;
}
} // namespace

TrackState track_state(const Context& c, unsigned t) {
    t %= track_count;
    return {c.project.tracks[t], c.patterns.at[t]};
}

void restore_track(Context& c, unsigned t, const TrackState& state) {
    t %= track_count;
    c.project.tracks[t] = state.track;
    c.patterns.at[t] = state.patterns;
    c.free_retired();
    send_track(c, t);
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

std::string swap_waits(const Context& c, unsigned a, unsigned b) {
    a %= track_count; b %= track_count;
    if (c.ui.recording) return "not in a take";
    for (unsigned l = 0; l < loop_tracks; ++l) {
        uint8_t source = c.project.looper.loops[l].source;
        if (!looper::source_is_track(source) || (source - 1u != a && source - 1u != b)) continue;
        auto state = c.host.looper().view(l).state; // the Looper keeps its source through a take
        if (state == Looper::State::Recording || state == Looper::State::Overdub) return "after L" + std::to_string(l + 1) + "'s take";
    }
    const auto& in = c.ui.settings.input;
    if (in.source == InputSource::Track && (in.track == a || in.track == b) && c.host_recording()) return "after the recording";
    return {};
}

void trade_tracks(Context& c, unsigned a, unsigned b) {
    a %= track_count; b %= track_count;
    if (a == b) return;
    c.stage_tick(UINT32_MAX); // a sound still waiting to load lands on the track it was turned on first
    auto changed = swap_tracks(c.project, c.patterns, a, b);
    swap_track_views(c.ui, a, b);
    std::swap(c.chain_queued[a], c.chain_queued[b]);
    c.free_retired();
    send_track(c, a);
    send_track(c, b);
    if (changed.song) c.sync_song();
    if (changed.punches) c.sync_punches();
    for (unsigned l = 0; l < loop_tracks; ++l)
        if ((changed.loops >> l) & 1)
            c.send({Command::Type::Looper, uint8_t(l), 0, uint16_t(looper::Do::Source), int16_t(c.project.looper.loops[l].source)});
    if (changed.knob) { // the tracks it is heard on, and both tracks' pads (send_knob_targets() sends the track in hand's)
        const auto& k = c.project.knob_fx;
        Command tracks {Command::Type::KnobFx, 0, 0, 3};
        tracks.position = k.tracks;
        c.send(tracks);
        for (unsigned t : {a, b})
            for (unsigned quarter = 0; quarter < 4; ++quarter) {
                Command pads {Command::Type::KnobFx, uint8_t(t), uint8_t(quarter), 4};
                pads.position = uint32_t(k.pads[t][quarter / 2] >> (quarter % 2 * 32));
                c.send(pads);
            }
    }
    if (changed.key) c.set_compressor(7, c.project.compressor.key);
    const auto& in = c.ui.settings.input; // Input Config's track follows the sound it names
    if (in.source == InputSource::Track && (in.track == a || in.track == b) && !c.host_recording())
        c.set_input_source(InputSource::Track, swapped(in.track, a, b));
    c.dirty = true;
}

void move_track(Context& c, unsigned from, unsigned to) {
    from %= track_count; to %= track_count;
    if (from == to) return;
    if (auto why = swap_waits(c, from, to); !why.empty()) { c.notice("Move track: " + why); return; }
    bool trade = track_clearable(c.project.tracks[to], c.patterns.at[to]); // something there: the two trade places
    std::string a = std::to_string(from + 1), b = std::to_string(to + 1);
    std::string what = trade ? "Swap tracks " + a + " and " + b : "Move track " + a + " to " + b;
    Edit edit = c.begin_edit(what.c_str(), to, -1, false);
    edit.swap_a = int8_t(from); edit.swap_b = int8_t(to);
    trade_tracks(c, from, to);
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
