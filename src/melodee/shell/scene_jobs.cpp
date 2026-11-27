// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Roadmap P5: scenes and follow actions. A scene (a sequence on screen) keeps a pattern, or off, for each track,
// with a name and a tempo of its own (model/scene_links.h, shell/sequences.h). A pattern picked by hand is kept in
// the sequence in hand (pick()); launching a sequence queues each track's kept pattern for the next bar, stops a
// track kept off, and leaves a track it keeps nothing for playing. An empty kept pattern takes the sequence's
// length. Its tempo takes over on that bar, and the mutes it keeps. A mute set by hand is kept in the sequence in
// hand too (keep_mute()). A pattern's follow action (model/follow.h) is the audio thread's
// (EngineHost::follow); here the shell tells it which tracks play a chain (their follow actions wait) and follows
// what it brought, so a track's pattern is the one playing. Follow actions, chains and the song keep nothing.
#include "melodee/shell/context.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/sequences.h"
#include "melodee/model/scene_links.h"
#include "melodee/engine/engine_host.h"
#include <algorithm>
#include <string>

namespace melodee {
using Type = Command::Type;

uint32_t Context::stopped_now() const { return host.playing() && !host.song_playing() ? host.stopped_tracks() : 0; }

bool Context::keep(unsigned t, int target) {
    if (host.song_playing()) return false; // the song plays the tracks: nothing is kept
    const unsigned n = project.scene % pattern_count;
    std::array<uint8_t, track_count> before {};
    for (unsigned k = 0; k < track_count; ++k) before[k] = project.tracks[k].in_scene[n];
    bool first = keep_pick(project, n, t % track_count, target, stopped_now());
    for (unsigned k = 0; k < track_count; ++k) dirty |= project.tracks[k].in_scene[n] != before[k];
    return first;
}

void Context::keep_mute(unsigned t, bool on) {
    t %= track_count;
    if (host.song_playing() || !project.tracks[t].used()) return; // the song plays the tracks: nothing is kept
    const unsigned n = project.scene % pattern_count;
    std::array<uint8_t, track_count> before {};
    std::array<bool, track_count> muted {};
    for (unsigned k = 0; k < track_count; ++k) { before[k] = project.tracks[k].in_scene[n]; muted[k] = scene_muted(project.tracks[k], n); }
    melodee::keep_mute(project, n, t, on, stopped_now());
    for (unsigned k = 0; k < track_count; ++k)
        dirty |= project.tracks[k].in_scene[n] != before[k] || scene_muted(project.tracks[k], n) != muted[k];
}

bool Context::pick(unsigned t, unsigned p) {
    launch(t, p);
    return keep(t, int(p % pattern_count));
}

void Context::track_off(unsigned t) {
    t %= track_count;
    auto& tr = project.tracks[t];
    if (!chaining) { // off by hand: the chain ends, as a launch ends it
        tr.chain_count = 0; chain_queued[t] = false;
        if (chain_lead == int(t)) chain_lead = -1;
    }
    tr.off = true;
    send({Type::Pattern, uint8_t(t), 0, 0, int16_t(-1)});
    if (recording_tracks >> t & 1) finish_launches(uint16_t(1u << t)); // Seq Record into the song: its clip ends
}

bool Context::pick_off(unsigned t) {
    track_off(t);
    return keep(t, scene_stops);
}

void Context::launch_scene(unsigned scene, bool quiet) {
    scene %= pattern_count;
    jam_scene = int(scene); // jam capture: a marker on the bar it takes over on
    unsigned count = 0, selected = ui.track % track_count;
    unsigned playing = host.playing() ? unsigned(host.pattern_of(selected)) % pattern_count : track().pattern % pattern_count;
    const auto& fallback = patterns.at[selected][playing];
    uint32_t length = sequence_length(project, patterns, selected, scene, fallback.length);
    bool free_now = !sequence_length(project, patterns, selected, scene, 0) && fallback.is_free(); // no notes there, none playing: Free stays
    const bool kept = scene_kept(project, scene);
    for (unsigned t = 0; t < track_count; ++t) {
        if (!sequence_takes(project.tracks[t])) continue;
        const int target = scene_target(project.tracks[t], scene);
        if (target == scene_plays_on) continue; // it keeps nothing for the track: it plays on
        if (target == scene_stops) { track_off(t); continue; }
        auto& pattern = patterns.at[t][unsigned(target)];
        // Silent, as long as the sequence; one frozen without notes keeps the length its print was made at.
        if (pattern.empty() && !pattern_frozen(t, unsigned(target)) && length && (pattern.length != length || pattern.free != free_now)) {
            pattern.length = length; pattern.free = free_now;
            sync_pattern(t, unsigned(target));
        }
        launch(t, unsigned(target));
        count += !pattern.empty();
    }
    const auto& s = project.scenes[scene];
    if (s.tempo) { // on the bar the patterns take over on (stopped, at once)
        project.tempo = s.tempo;
        send({Type::SceneTempo, 0, 0, 0, int16_t(s.tempo)});
        dirty = true;
    }
    if (const SceneMutes mutes = scene_mutes_of(project, scene); mutes.set) { // its mutes, on that bar too
        for (unsigned t = 0; t < track_count; ++t) {
            if (!(mutes.set >> t & 1)) continue;
            const bool on = mutes.muted >> t & 1;
            dirty |= project.tracks[t].mix.mute != on;
            project.tracks[t].mix.mute = on;
        }
        send({Type::SceneMutes, 0, 0, mutes.set, int16_t(mutes.muted)});
    }
    project.scene = uint8_t(scene);
    if (quiet) return;
    std::string text = "SEQ " + std::to_string(scene + 1);
    if (s.name[0]) text += std::string(" ") + s.name;
    if (kept) text += ": " + std::to_string(count) + (count == 1 ? " track" : " tracks");
    else text += ": new, plays on";
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

void Context::set_scene_colour(unsigned scene, int colour) {
    auto& s = project.scenes[scene % pattern_count];
    uint8_t value = uint8_t(std::clamp(colour, 0, 16));
    if (s.colour == value) return;
    s.colour = value;
    dirty = true;
}

void Context::name_scene(unsigned scene) {
    name_scene_at = uint8_t(scene % pattern_count);
    const auto& s = project.scenes[name_scene_at];
    open_name(NameTarget::Scene, s.name[0] ? std::string(s.name) : "SEQ " + std::to_string(name_scene_at + 1), sizeof(Scene::name) - 1, true);
}

void Context::name_track(unsigned t) {
    name_track_at = uint8_t(t % track_count);
    const auto& track = project.tracks[name_track_at];
    if (!track.used()) return;
    open_name(NameTarget::Track, track.name[0] ? std::string(track.name) : "TRACK " + std::to_string(name_track_at + 1), name_length - 1, true);
}

// A sequence as it is now, for an undo.
static SceneState scene_state(const Project& project, unsigned scene) {
    SceneState state;
    state.index = uint8_t(scene % pattern_count);
    state.scene = project.scenes[state.index];
    for (unsigned t = 0; t < track_count; ++t) {
        state.kept[t] = project.tracks[t].in_scene[state.index];
        if (scene_muted(project.tracks[t], state.index)) state.muted = uint16_t(state.muted | 1u << t);
    }
    return state;
}

// ERASE + pad on Sequences: the sequence keeps nothing, and loses its name, tempo and colour; one undo.
void Context::clear_scene(unsigned scene) {
    scene %= pattern_count;
    const std::string name = "SEQ " + std::to_string(scene + 1);
    if (project.scenes[scene].plain() && !scene_kept(project, scene)) { notice(name + " is empty"); return; }
    Edit edit;
    edit.what = "Clear " + name;
    edit.track = ui.track;
    edit.scene_before = std::make_unique<SceneState>(scene_state(project, scene));
    project.scenes[scene] = Scene {};
    forget_scene(project, scene);
    edit.scene_after = std::make_unique<SceneState>(scene_state(project, scene));
    edits.push(std::move(edit));
    dirty = true;
    notice(name + " cleared");
}

// Shift + Erase on Sequences, then two pads: sequence `to` becomes a copy of `from` (its name, tempo, colour and
// mutes), each pattern it keeps copied into the track's next free pattern so the two are edited apart
// (copy_scene_links); one undo. A track with no free pattern keeps the same pattern in both.
void Context::copy_scene(unsigned from, unsigned to) {
    from %= pattern_count; to %= pattern_count;
    if (from == to) return;
    const std::string a = "SEQ " + std::to_string(from + 1), b = "SEQ " + std::to_string(to + 1);
    if (ui.recording) { notice("Copy: not in a take"); return; }
    Edit edit;
    edit.what = "Copy " + a + " to " + b;
    edit.track = ui.track;
    edit.scene_before = std::make_unique<SceneState>(scene_state(project, to));
    std::vector<PatternCopy> copies;
    const unsigned shared = copy_scene_links(project, patterns, from, to, copies);
    for (const auto& c : copies) {
        Pattern made = patterns.at[c.track][c.from];
        edit.patterns_before.push_back({c.track, c.into, patterns.at[c.track][c.into]});
        edit.patterns_after.push_back({c.track, c.into, std::move(made)});
    }
    for (const auto& p : edit.patterns_after) restore_pattern(p.track, p.pattern, p.value);
    edit.scene_after = std::make_unique<SceneState>(scene_state(project, to));
    edits.push(std::move(edit));
    dirty = true;
    notice(a + " to " + b + (shared ? ", " + std::to_string(shared) + " shared" : std::string()));
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

// B3 ALL on SEQUENCES: the pattern in hand's follow action onto the pattern every other used track is on (with
// notes, not off); one undo.
void Context::follow_to_column() {
    PatternFollow f = pattern().follow;
    Edit edit;
    edit.what = "Follow to SEQ " + std::to_string(project.scene % pattern_count + 1);
    edit.track = ui.track;
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& tr = project.tracks[t];
        const unsigned at = tr.pattern % pattern_count;
        const auto& p = patterns.at[t][at];
        if (t == ui.track % track_count || !tr.used() || tr.off || p.empty() || p.follow == f) continue;
        Pattern after = p;
        after.follow = f;
        edit.patterns_before.push_back({uint8_t(t), uint8_t(at), p});
        edit.patterns_after.push_back({uint8_t(t), uint8_t(at), std::move(after)});
    }
    if (edit.patterns_after.empty()) { notice("Nothing else to follow"); return; }
    for (const auto& p : edit.patterns_after) restore_pattern(p.track, p.pattern, p.value);
    unsigned count = unsigned(edit.patterns_after.size());
    edits.push(std::move(edit));
    dirty = true;
    notice("Follow to " + std::to_string(count) + (count == 1 ? " track" : " tracks"));
}

// CAPTURE on Sequences (Shift + B2, then a pad): sequence n keeps what every used track is on now (off where it is
// stopped) and becomes the sequence in hand; one undo. No notes are copied.
void Context::capture_scene(unsigned scene) {
    scene %= pattern_count;
    ui.scene_capture = false;
    if (host.song_playing()) { notice("Not while the song plays"); return; }
    Edit edit;
    edit.what = "Capture SEQ " + std::to_string(scene + 1);
    edit.track = ui.track;
    edit.scene_before = std::make_unique<SceneState>(scene_state(project, scene));
    unsigned count = keep_all(project, scene, stopped_now());
    if (!count) { notice("No tracks to capture"); return; }
    edit.scene_after = std::make_unique<SceneState>(scene_state(project, scene));
    edits.push(std::move(edit));
    project.scene = uint8_t(scene);
    dirty = true;
    notice("SEQ " + std::to_string(scene + 1) + " captured: " + std::to_string(count) + (count == 1 ? " track" : " tracks"));
}

unsigned Context::tempo_now() const {
    if (!project.song.tempo.empty() && host.song_playing() && host.playing()) return host.tempo_now(); // the lane's, as it plays
    return project.tempo_of(ui.lead_pattern);
}

// Roadmap P5: the song's tempo lane, from Song's Shift + K1 (the tempo at the cursor's bar; under 30 BPM none there)
// and Shift + K2 (stepped to on its bar, or ramped to from the point before); one undo a turn, the song's.
void Context::set_song_tempo(unsigned bar, int tenths) {
    auto& lane = project.song.tempo;
    if (tenths <= 0) { if (!lane.erase(bar)) return; }
    else {
        int at = lane.at_bar(bar);
        bool ramp = at >= 0 && lane.points[size_t(at)].ramp;
        if (at >= 0 && lane.points[size_t(at)].tempo == unsigned(tenths)) return;
        if (!lane.set(bar, unsigned(tenths), ramp)) { notice("Tempo lane full"); return; }
    }
    sync_song();
    dirty = true;
}

void Context::set_song_tempo_shape(unsigned bar, bool ramp) {
    auto& lane = project.song.tempo;
    int at = lane.at_bar(bar);
    if (at < 0 || lane.points[size_t(at)].ramp == ramp) return;
    lane.points[size_t(at)].ramp = ramp;
    sync_song();
    dirty = true;
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
    // The bank in view follows the selected track's pattern (playing, else the one that will) when that changes.
    unsigned shown = host.playing() ? unsigned(host.pattern_of(ui.track % track_count)) % pattern_count : track().pattern % pattern_count;
    if (int(shown) != ui.bank_seen) { ui.bank_seen = int16_t(shown); ui.pattern_bank = uint8_t(shown / pattern_pads); }
}

} // namespace melodee
