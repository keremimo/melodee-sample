// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Freeze plan F3, audio tracks on the MIDI thread: each audio track's
// patterns' takes sent to the audio thread as a pack (made on the audio
// tracks' thread, shell/audio_tracks.h) whenever they change; Seq Record on
// an audio track arming a take of one pass of its pattern; the take put in
// the pattern it recorded, as one edit Undo takes back; and Stage's cards on
// the take of the pattern in hand.
#include "melodee/engine/engine_host.h"
#include "melodee/shell/audio_tracks.h"
#include "melodee/shell/context.h"
#include "melodee/shell/song_take.h"
#include <algorithm>
#include <string>

namespace melodee {
using Type = Command::Type;

namespace {
std::vector<std::pair<uint8_t, AudioRegion>> takes_of(const Project& project, const PatternBank& bank, unsigned t) {
    std::vector<std::pair<uint8_t, AudioRegion>> out;
    if (!project.tracks[t].is_audio()) return out;
    for (unsigned p = 0; p < pattern_count; ++p)
        if (bank.at[t][p].audio.used()) out.emplace_back(uint8_t(p), bank.at[t][p].audio);
    return out;
}
// The highest take number any pattern plays (0: none).
unsigned last_take(const PatternBank& bank) {
    unsigned most = 0;
    for (const auto& track : bank.at) for (const auto& p : track) most = std::max<unsigned>(most, p.audio.take);
    return most;
}
} // namespace

void Context::sync_audio() {
    if (!printer) return; // the host tests and the monkey: no card, no takes
    if (!audio_tracks) {
        audio_tracks = std::make_shared<AudioTracks>(host);
        audio_tracks->set_pager(&host.streams());
    }
    const std::string folder = takes_folder();
    const bool all = project_path != audio_sent_path;
    audio_sent_path = project_path;
    for (unsigned t = 0; t < track_count; ++t) {
        auto takes = takes_of(project, patterns, t);
        const bool stale = all || ((audio_stale >> t) & 1);
        if (!stale && takes == audio_sent[t]) continue;
        const bool had = stale || !audio_sent[t].empty();
        audio_sent[t] = takes;
        ++audio_serial[t];
        if (takes.empty() || folder.empty()) {
            if (had) { Command c {Type::AudioRegions, uint8_t(t)}; c.audio = nullptr; host.send(c); }
            continue;
        }
        audio_tracks->build({t, audio_serial[t], folder, std::move(takes)});
    }
    audio_stale = 0;
}

void Context::audio_tick() {
    // What Stage shows: the take armed, and whether it runs. One that ended by itself (its pass done) ends Seq Record.
    const int engine_track = host.audio_take_track();
    if (audio_take_track >= 0 && engine_track == audio_take_track) audio_began = true;
    ui.audio_rec_track = int8_t(audio_take_track);
    ui.audio_rec_running = audio_take_track >= 0 && host.audio_take().running();
    song_take_tick();
    ui.song_rec_pattern = int8_t(song_take.track >= 0 && !song_take.ended ? song_take.pattern : -1);
    if (audio_began && engine_track < 0) {
        audio_began = false;
        if (song_take.track >= 0 && !song_take.ended) set_recording(false); // its pattern came round: the clip ends
        else if (ui.recording && track().is_audio()) {
            ui.recording = false;
            if (ui.settings.click == 2) send_click();
        }
    }
    if (!printer) return;
    sync_audio(); // a few patterns' takes compared: what changed (a take, an undo, a copy) is made again
    if (audio_prune && !project_path.empty() && audio_take_track < 0) {
        audio_prune = false;
        std::vector<unsigned> kept;
        for (const auto& track : patterns.at) for (const auto& p : track) if (p.audio.used()) kept.push_back(p.audio.take);
        audio_tracks->prune_later(takes_folder(), std::move(kept));
    }
    AudioPack* done = nullptr;
    while (host.take_done_audio(done)) audio_tracks->discard(done);
    AudioPackMade made;
    while (audio_tracks->take_made(made)) {
        const unsigned t = made.track % track_count;
        if (made.serial != audio_serial[t] || !project.tracks[t].is_audio()) { audio_tracks->discard(made.pack); continue; }
        if (!made.peaks.empty()) add_peaks(std::move(made.peaks));
        Command c {Type::AudioRegions, uint8_t(t)};
        c.audio = made.pack;
        if (!host.send(c)) { audio_tracks->discard(made.pack); audio_stale |= 1u << t; continue; } // again next pass
        if (made.missing)
            notice("Track " + std::to_string(t + 1) + ": " + std::to_string(made.missing) + (made.missing == 1 ? " take missing" : " takes missing"));
    }
    AudioTakeDone take;
    while (audio_tracks->take_recorded(take)) {
        audio_take_track = -1;
        audio_began = false;
        const unsigned t = take.job.track % track_count;
        if (song_take.track == int(t)) { land_song_take(take); continue; }
        if (!take.error.empty()) {
            if (take.error != "nothing recorded") notice("Audio: " + take.error);
            continue;
        }
        // Another project since (its file stays in the old one's folder), or the track is no longer an audio track.
        if (take.pattern < 0 || take.job.folder != takes_folder() || !project.tracks[t].is_audio()) continue;
        if (!take.peaks.empty()) { TakePeaks one; one[take.job.take] = std::move(take.peaks); add_peaks(std::move(one)); }
        AudioRegion r;
        r.take = uint16_t(take.job.take);
        r.frames = r.take_frames = uint32_t((uint64_t(take.frames) * 44100 + take.rate / 2) / std::max(1u, take.rate));
        r.tempo = take.job.tempo;
        if (!r.frames) continue;
        const unsigned p = unsigned(take.pattern) % pattern_count;
        Edit edit = begin_edit("Record audio", t, int(p), false);
        patterns.at[t][p].audio = r;
        sync_pattern(t, p);
        commit_edit(std::move(edit));
        dirty = true;
        notice("Track " + std::to_string(t + 1) + ": P" + std::to_string(p + 1) + " recorded");
    }
}

std::string Context::takes_folder() const {
    if (!project_path.empty()) return audio_folder(project_path);
    return audio_staging_from.empty() ? std::string() : audio_folder(audio_staging_from);
}

void Context::add_peaks(TakePeaks&& more) {
    auto next = std::make_shared<TakePeaks>(audio_peaks ? *audio_peaks : TakePeaks {});
    for (auto& [take, peaks] : more) (*next)[take] = std::move(peaks);
    audio_peaks = std::move(next);
}

bool Context::record_audio(unsigned track) {
    const unsigned t = track % track_count;
    if (!project.tracks[t].is_audio()) return false;
    if (!printer) { notice("Audio: no card"); return false; }
    if (takes_folder().empty()) { notice("Audio: save the project first"); return false; }
    if (!input_free(project.tracks[t].audio.source)) return false;
    sync_audio();
    if (audio_take_track >= 0 || audio_tracks->writing()) { notice("Audio: the last take is still being written"); return false; }
    AudioTakeJob job;
    job.folder = takes_folder();
    job.take = last_take(patterns) + 1; // at least; the thread takes the next free number in the folder
    job.track = t;
    job.tempo = uint16_t(tempo_now());
    if (!audio_tracks->record(job)) { notice("Audio: the last take is still being written"); return false; }
    if (!host.send({Type::AudioRecord, uint8_t(t), 0, 0, 0})) { notice("Audio: try again"); return false; }
    audio_take_track = int(t);
    audio_began = false;
    ui.audio_rec_track = int8_t(t);
    return true;
}

void Context::end_audio(bool keep) {
    if (audio_take_track < 0) return;
    host.send({Type::AudioRecord, uint8_t(audio_take_track), 0, 0, int16_t(keep ? -2 : -1)});
}

// Seq Record on an audio track's lane with the song playing, or Play about to play it from `bar`: the clip first (the
// track plays only where a clip is, and a take starts at its pattern's top), as long as the pattern can be while it
// grows, then the take armed as record_audio() arms one. The song plays on past its end while it records.
bool Context::record_song_audio(unsigned track, unsigned bar) {
    const unsigned t = track % track_count;
    if (song_take.track >= 0) { notice("Audio: the last take is still being written"); return false; }
    if (bar + 1 >= song_bars) { notice("Audio: past the song's last bar"); return false; }
    const int found = song_take_pattern(project.song, patterns, t);
    if (found < 0) { notice("No empty pattern left"); return false; }
    const unsigned p = unsigned(found);
    auto edit = std::make_unique<Edit>(begin_edit("Record audio", t, int(p), true));
    auto back = [&] { // the song and the pattern as they were
        restore_song(*edit->song_before);
        restore_pattern(t, p, *edit->pattern_before);
    };
    if (!project.song.place({uint8_t(t), uint8_t(p), uint16_t(bar), 1, false})) { back(); notice("Song is full"); return false; } // place() cuts first
    auto& pat = patterns.at[t][p];
    pat.free = false;
    pat.length = project.max_length();
    pat.fit_lanes();
    sync_pattern(t, p);
    sync_song();
    if (!record_audio(t)) { back(); return false; }
    send({Type::SongRecord, 0, 0, 0, 1});
    song_take = {};
    song_take.track = int(t);
    song_take.pattern = uint8_t(p);
    song_take.bar = uint16_t(bar);
    song_take.edit = std::move(edit);
    return true;
}

// While the take runs (the song's bar is the audio thread's own then): the clip reaches the bar after the one playing.
// The song back before the bar last heard (the loop came round, or a jump): Seq Record ends, one pass as a pattern's.
void Context::song_take_tick() {
    auto& st = song_take;
    if (st.track < 0 || st.ended || !host.audio_take().running()) return;
    const unsigned bar = host.song_bar();
    if (st.last != ~0u && bar < st.last) { set_recording(false); return; }
    if (bar >= unsigned(st.bar) + max_bars || bar >= song_bars) { set_recording(false); return; } // the longest a pattern is
    st.last = bar;
    if (song_take_grow(project.song, unsigned(st.track), st.pattern, st.bar, bar)) sync_song();
}

void Context::end_song_take() {
    auto& st = song_take;
    if (st.track < 0 || st.ended) return;
    st.ended = true;
    send({Type::SongRecord, 0, 0, 0, 0});
    if (host.audio_take().running()) { // a bar it heard that song_take_tick() has not seen yet
        const unsigned bar = host.song_bar();
        if (st.last == ~0u || bar > st.last) st.last = bar;
    }
    const unsigned t = unsigned(st.track), p = st.pattern;
    if (st.last == ~0u) { // it never ran (stopped in the count-in): the song and the pattern as they were
        restore_song(*st.edit->song_before);
        restore_pattern(t, p, *st.edit->pattern_before);
        st.edit.reset();
        return;
    }
    const unsigned bars = song_take_close(project.song, t, p, st.bar, st.last);
    auto& pat = patterns.at[t][p];
    pat.length = std::max(bars, 1u) * std::max(project.bar_ticks(), 1u);
    pat.fit_lanes();
    sync_pattern(t, p);
    sync_song();
    const unsigned before = edits.pushes;
    commit_edit(std::move(*st.edit));
    st.edit.reset();
    st.keep = edits.pushes != before;
    st.pushes = edits.pushes;
    dirty = true;
}

// The take's file is whole (or failed): into the step its clip was kept as, while that is the newest step, or the next
// redo (undone as it was written); else it is left out (its file goes when the project is opened again).
void Context::land_song_take(const AudioTakeDone& take) {
    if (!song_take.ended) { // it failed while it ran
        end_song_take();
        if (ui.recording) set_recording(false);
    }
    SongTake st = std::move(song_take);
    song_take = {};
    if (!take.error.empty()) {
        if (take.error != "nothing recorded") notice("Audio: " + take.error);
        return;
    }
    const unsigned t = unsigned(st.track) % track_count, p = st.pattern % pattern_count;
    if (!st.keep || take.job.folder != takes_folder() || !project.tracks[t].is_audio()) return;
    AudioRegion r;
    r.take = uint16_t(take.job.take);
    r.frames = r.take_frames = uint32_t((uint64_t(take.frames) * 44100 + take.rate / 2) / std::max(1u, take.rate));
    r.tempo = take.job.tempo;
    if (!r.frames) return;
    auto ours = [&](const Edit* e) {
        return e && e->what == "Record audio" && e->track == t && e->pattern == int(p) && e->song_after && e->pattern_after;
    };
    Edit* step = nullptr;
    bool live = false; // the step is done (not undone): the pattern takes the take now
    if (edits.pushes == st.pushes) {
        if (ours(edits.latest())) { step = edits.latest(); live = true; }
        else if (ours(edits.upcoming())) step = edits.upcoming();
    }
    if (!step) return;
    r.frames = uint32_t(std::min<uint64_t>(r.frames, ticks_to_frames(step->pattern_after->length, r.tempo))); // no longer than the clip
    if (!take.peaks.empty()) { TakePeaks one; one[take.job.take] = take.peaks; add_peaks(std::move(one)); }
    step->pattern_after->audio = r;
    if (live) {
        patterns.at[t][p].audio = r;
        sync_pattern(t, p);
        dirty = true;
    }
    notice("Track " + std::to_string(t + 1) + ": P" + std::to_string(p + 1) + " recorded");
}

// Stage's cards on the take of the pattern in hand: level, where it starts and ends in the take (it always plays from
// the pattern's top), its fades and its direction.
void Context::set_region_value(const Binding& b, int value) {
    using K = Binding::Kind;
    const unsigned t = ui.track % track_count, p = track().pattern % pattern_count;
    auto& r = patterns.at[t][p].audio;
    if (!r.used()) return;
    auto frames = [](int ms) { return uint32_t(uint64_t(std::max(ms, 0)) * 441 / 10); };
    switch (b.kind) {
    case K::RegionGain: r.gain = int16_t(std::clamp(value, region_gain_min, region_gain_max)); break;
    case K::RegionStart: {
        const uint32_t end = r.offset + r.frames, start = std::min(frames(value), end - 1);
        r.offset = start; r.frames = end - start;
        break;
    }
    case K::RegionEnd: r.frames = std::clamp<uint32_t>(frames(value), r.offset + 1, r.take_frames) - r.offset; break;
    case K::RegionFadeIn: r.fade_in = std::min(frames(value), r.frames); break;
    case K::RegionFadeOut: r.fade_out = std::min(frames(value), r.frames); break;
    case K::RegionReverse: r.reverse = value != 0; break;
    case K::RegionWarp: r.warp = value != 0; r.mode = value == 2 ? WarpMode::Repitch : WarpMode::Stretch; break;
    case K::RegionBpm: r.tempo = uint16_t(std::clamp(value, 300, 3000)); break;
    case K::RegionSemi: r.semi = int8_t(std::clamp(value, -24, 24)); break;
    case K::RegionFine: r.fine = int8_t(std::clamp(value, -50, 50)); break;
    default: return;
    }
    r.fade_in = std::min(r.fade_in, r.frames); r.fade_out = std::min(r.fade_out, r.frames);
    sync_pattern(t, p);
    dirty = true;
}

} // namespace melodee
