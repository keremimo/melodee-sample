// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Roadmap P5: scenes and follow actions. A scene is a column of patterns
// across the tracks (pattern n of each), with a name and a tempo of its own:
// launching it queues pattern n on every track holding notes there (and on
// the selected track) for the next bar, as Seq held + Shift + pad always
// launched a column, and its tempo takes over on that bar. A pattern's follow
// action (model/follow.h) is the audio thread's (EngineHost::follow); here the
// shell tells it which tracks play a chain (their follow actions wait) and
// follows what it brought, so a track's pattern is the one playing.
#include "melodee/shell/context.h"
#include "melodee/engine/engine_host.h"
#include <algorithm>
#include <string>

namespace melodee {
using Type = Command::Type;

void Context::launch_scene(unsigned scene) {
    scene %= pattern_count;
    unsigned count = 0;
    for (unsigned t = 0; t < track_count; ++t) {
        if (!project.tracks[t].used()) continue;
        if (t != ui.track % track_count && patterns.at[t][scene].empty()) continue; // nothing there: it plays on
        launch(t, scene);
        ++count;
    }
    const auto& s = project.scenes[scene];
    if (s.tempo) { // on the bar the patterns take over on (stopped, at once)
        project.tempo = s.tempo;
        send({Type::SceneTempo, 0, 0, 0, int16_t(s.tempo)});
        dirty = true;
    }
    ui.scene = uint8_t(scene);
    std::string text = "S" + std::to_string(scene + 1);
    if (s.name[0]) text += std::string(" ") + s.name;
    text += ": " + std::to_string(count) + (count == 1 ? " track" : " tracks");
    if (s.tempo) text += ", " + std::to_string(s.tempo / 10) + (s.tempo % 10 ? "." + std::to_string(s.tempo % 10) : "") + " BPM";
    notice(text);
}

void Context::set_scene_tempo(unsigned scene, int tenths) {
    auto& s = project.scenes[scene % pattern_count];
    uint16_t value = uint16_t(tenths < 300 ? 0 : std::min(tenths, 3000)); // below 30 BPM: none of its own
    if (s.tempo == value) return;
    s.tempo = value;
    dirty = true;
}

void Context::name_scene(unsigned scene) {
    name_scene_at = uint8_t(scene % pattern_count);
    const auto& s = project.scenes[name_scene_at];
    open_name(NameTarget::Scene, s.name[0] ? std::string(s.name) : "S" + std::to_string(name_scene_at + 1), sizeof(Scene::name) - 1, true);
}

void Context::clear_scene(unsigned scene) {
    auto& s = project.scenes[scene % pattern_count];
    if (s.plain()) { notice("S" + std::to_string(scene % pattern_count + 1) + " has no name or tempo"); return; }
    s = Scene {};
    dirty = true;
    notice("S" + std::to_string(scene % pattern_count + 1) + " cleared");
}

// The selected track's pattern's follow action, from its cards on SCENES (one undo a turn: the pattern's).
void Context::set_follow(Binding::Kind kind, int value) {
    using Kind = Binding::Kind;
    auto& f = pattern().follow;
    PatternFollow was = f;
    switch (kind) {
    case Kind::FollowAction: f.action = uint8_t(std::clamp(value, 0, int(FollowAction::Count) - 1)); break;
    case Kind::FollowOther: f.other = uint8_t(std::clamp(value, 0, int(FollowAction::Count) - 1)); break;
    case Kind::FollowAfter: follow_after_of_card(f, std::clamp(value, 0, 127)); break;
    case Kind::FollowChance: f.chance = uint8_t(std::clamp(value, 0, 100)); break;
    default: return;
    }
    if (f != was) sync_pattern(ui.track, track().pattern);
}

// B3 ALL on SCENES: the pattern in hand's follow action onto every track's pattern with notes in its column; one undo.
void Context::follow_to_column() {
    unsigned column = track().pattern % pattern_count;
    PatternFollow f = pattern().follow;
    Edit edit;
    edit.what = "Follow to S" + std::to_string(column + 1);
    edit.track = ui.track;
    edit.column = int16_t(column);
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& p = patterns.at[t][column];
        if (t == ui.track % track_count || !project.tracks[t].used() || p.empty() || p.follow == f) continue;
        Pattern after = p;
        after.follow = f;
        edit.column_before.emplace_back(uint8_t(t), p);
        edit.column_after.emplace_back(uint8_t(t), std::move(after));
    }
    if (edit.column_after.empty()) { notice("S" + std::to_string(column + 1) + ": nothing else to follow"); return; }
    for (const auto& [t, value] : edit.column_after) restore_pattern(t, column, value);
    unsigned count = unsigned(edit.column_after.size());
    edits.push(std::move(edit));
    dirty = true;
    notice("Follow to " + std::to_string(count) + (count == 1 ? " track" : " tracks"));
}

// MIDI thread, often: a chained track's follow actions wait (the chain is its order, docs/ROADMAP.md P5), and a
// follow action that came moves the track's pattern to the one it brought.
void Context::follow_tick() {
    bool patterns_playing = host.playing() && !host.song_playing();
    for (unsigned t = 0; t < track_count; ++t) {
        auto& tr = project.tracks[t];
        bool chained = tr.chained();
        if (chained != chained_sent[t]) {
            chained_sent[t] = chained;
            send({Type::Chained, uint8_t(t), 0, 0, int16_t(chained)});
        }
        uint32_t came = host.follows_of(t);
        if (came == follows_seen[t]) continue;
        follows_seen[t] = came;
        if (patterns_playing && !(host.stopped_tracks() >> t & 1)) {
            tr.pattern = uint8_t(host.pattern_of(t) % pattern_count);
            if (t == ui.track % track_count) ui.lead_pattern = tr.pattern;
        }
    }
}

} // namespace melodee
