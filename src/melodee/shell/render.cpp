// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Renders of the mix (parity phase 6), on the MIDI thread:
//
// - Resample (Shift + Pad 11, as printed): then a pad; the selected track's
//   pattern (with every other track's playing alongside) is played from its
//   start and rendered onto that pad, as Akai's editing_sequences page has
//   it. Up to 60 s, the longest sample a pad loads. One undo. On a plug-in
//   track it opens AUTO SAMPLE instead (shell/auto_sample.h).
// - Resample on Song (roadmap P3): then a pad; the song from its first bar to
//   its end (and its tails, as a mixdown has them) onto that pad, its first
//   60 s when it is longer. One undo.
// - Audio mixdown (Song, B1 Export, B2): the song from its first bar to its
//   end, loop or not, and on until its tails fall silent, as
//   Melodee/Mixdowns/<name>.wav on the card.
//
// Both play in real time through the engine host that is heard, not in a
// second engine faster than real time: the render is exactly what plays
// (effects, punches and pots moved meanwhile included), nothing is held
// twice in memory (sixteen engines, their chains and delay lines, the
// samples) and no second core is pinned while the live engine runs. The
// render takes as long as the music; Stop drops it.
#include "melodee/engine/engine_host.h"
#include "melodee/shell/context.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/sample_memory.h"
#include "melodee/shell/smf_jobs.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace melodee {
using Type = Command::Type;

void Context::resample() {
    if (ui.rendering) { notice("Rendering already"); return; }
    if (ui.resample_pick) { ui.resample_pick = false; notice("Resample off"); return; }
    const bool song = ui.page == PageId::Song && project.song.length(); // roadmap P3: on Song, the song
    if ((track().engine == EngineKind::Plugin || track().engine == EngineKind::Midi) && !song) { // a plug-in or MIDI track: AUTO SAMPLE, its keys (the instrument outside's) into a keygroup program; again closes it
        if (ui.page == PageId::AutoSample) auto_sheet_cancel();
        else open_auto_sheet();
        return;
    }
    if (track().engine != EngineKind::Sampler) { notice("Resample: pick a kit track"); return; }
    bool notes = song; // the patterns the tracks play: any notes to render?
    for (unsigned t = 0; t < track_count && !notes; ++t) notes = project.tracks[t].used() && !patterns.at[t][project.tracks[t].pattern % pattern_count].empty();
    if (!notes) { notice("Resample: no notes to play"); return; }
    ui.resample_pick = true;
    ui.resample_song = song;
    notice(song ? "Resample song: tap a pad" : "Resample: tap a pad");
}

// The longest Resample, 44.1 kHz frames: with a card it is written straight into its file (6 hours at most), without one
// it is kept in memory (60 s).
static uint32_t resample_most(const SampleLibrary* library) {
    return library && !library->root().empty() ? uint32_t(stream_most_seconds) * 44100u : TakeRecorder::max_frames;
}

// Roadmap P3: the song onto `kit`, as the mixdown plays it (from bar 1, once, loop or not, on through its tails), into
// a slot instead of a file; past resample_most() that much of it, the song then stopped.
void Context::resample_song(unsigned kit) {
    double tick_frames = 44100.0 * 60.0 / (tempo_now() / 10.0) / ticks_per_quarter;
    double song_frames = double(project.song.length()) * project.bar_ticks() * tick_frames;
    const uint32_t most = resample_most(library);
    bool cut = song_frames > most;
    int slot = hold_file("RESAMPLE", ui.track, int(kit), 1);
    if (slot < 0) { notice("Resample: samples full"); return; }
    SampleLibrary::Render job;
    job.slot = uint16_t(slot);
    job.frames = uint32_t(std::min<double>(song_frames, most)) + 1;
    if (!library->render(job)) { drop_file(slot); notice("Resample: busy"); return; }
    ui.render_resume = false;
    if (ui.recording) set_recording(false);
    ui.rec_armed = false;
    send({Type::Stop});
    send({Type::BounceArm, 0, 0, 0, 1});
    send({Type::Play, 0, 0, 0, 1}); // the song from bar 1
    played_song = true;
    ui.rendering = 3; ui.render_pad = int16_t(kit); ui.render_slot = int16_t(slot); ui.render_track = ui.track;
    // that much of it in 44.1 kHz frames, and a moment for the last of them to be drained
    ui.render_stop_at = cut ? (now + uint32_t(uint64_t(most) * 1000 / 44100) + 500) | 1 : 0;
    render_started = false; render_since = now;
    notice(pad_text(!cut ? "Song to" : most > TakeRecorder::max_frames ? "Song 6 h to" : "Song 60 s to", kit));
}

void Context::resample_pad(unsigned kit) {
    ui.resample_pick = false;
    if (!library || track().engine != EngineKind::Sampler) return;
    if (host_recording()) { notice("Resample: a take runs"); return; }
    kit %= sampler_pads;
    if (ui.resample_song) { ui.resample_song = false; resample_song(kit); return; }
    // The pattern's length, at most resample_most() at the tempo in play.
    double tick_frames = 44100.0 * 60.0 / (tempo_now() / 10.0) / ticks_per_quarter;
    uint32_t ticks = std::max<uint32_t>(pattern().length, 1);
    auto most = uint32_t(resample_most(library) / tick_frames);
    bool cut = ticks > most;
    ticks = std::min(ticks, most);
    int slot = hold_file("RESAMPLE", ui.track, int(kit), 1);
    if (slot < 0) { notice("Resample: samples full"); return; }
    SampleLibrary::Render job;
    job.slot = uint16_t(slot);
    job.frames = uint32_t(ticks * tick_frames) + 1;
    if (!library->render(job)) { drop_file(slot); notice("Resample: busy"); return; }
    ui.render_resume = host.playing() && !host.song_playing();
    if (ui.recording) set_recording(false);
    ui.rec_armed = false;
    send({Type::Stop});
    Command arm {Type::BounceArm, 0, 0, 0, 0};
    arm.position = ticks;
    send(arm);
    send({Type::Play, 0}); // the patterns from their start, no count-in
    played_song = false;
    ui.rendering = 1; ui.render_pad = int16_t(kit); ui.render_slot = int16_t(slot); ui.render_track = ui.track;
    render_started = false; render_since = now;
    notice(pad_text(cut ? "Resample 60 s to" : "Resample to", kit));
}

void Context::mixdown(const std::string& name) {
    if (!library) return;
    if (ui.export_kind == export_kinds::midi) { export_midi(*this, name); return; } // roadmap P4: a MIDI file, written at once
    if (ui.export_kind == export_kinds::mpc3 || ui.export_kind == export_kinds::live) { // a project, on a thread of its own
        export_project(name, ui.export_kind == export_kinds::live);
        return;
    }
    if (ui.rendering) { notice("Rendering already"); return; }
    if (!project.song.length()) { notice("Mixdown: the song is empty"); return; }
    if (library->card().empty()) { notice("Mixdown: no card"); return; }
    if (host_recording()) { notice("Mixdown: a take runs"); return; }
    SampleLibrary::Render job;
    job.to_file = true;
    std::snprintf(job.name, sizeof(job.name), "%s", name.c_str());
    // Roadmap P4: 24 or 16 bits (dithered: the only integer files Melodee renders), or the stems beside a float mix.
    job.bits = uint8_t(ui.export_kind == 1 ? 24 : ui.export_kind == 2 ? 16 : 32);
    job.stems = ui.export_kind == 3;
    if (job.stems) {
        uint32_t mask = 0;
        for (unsigned t = 0; t < track_count; ++t) {
            const auto& tr = project.tracks[t];
            if (!tr.used()) continue;
            mask |= 1u << t;
            std::snprintf(job.stem_names[t].data(), 24, "%s", tr.name[0] ? tr.name : ("Track " + std::to_string(t + 1)).c_str());
        }
        for (unsigned b = 0; b < bus_count; ++b) { // a bus something sends into
            bool fed = false;
            for (const auto& tr : project.tracks) fed |= tr.used() && tr.mix.sends[b] > 0;
            if (!fed) continue;
            mask |= 1u << (track_count + b);
            std::snprintf(job.stem_names[track_count + b].data(), 24, "Bus %c", char('A' + b));
        }
        for (unsigned n = 0; n < sub_count; ++n) { // the mixer: a submix a used track plays into
            bool fed = false;
            for (const auto& tr : project.tracks) fed |= tr.used() && tr.mix.out == out_sub(n);
            if (!fed) continue;
            const unsigned stem = track_count + bus_count + n;
            mask |= 1u << stem;
            const auto& sub = project.subs[n];
            std::snprintf(job.stem_names[stem].data(), 24, "%s", sub.name[0] ? sub.name : ("Submix " + std::to_string(n + 1)).c_str());
        }
        // Each kit pad the song plays, apart: as many as this pass takes beside the stems, the rest in later passes.
        song_pad_stems();
        unsigned fixed = 1; // the mix
        for (unsigned s = 0; s < StemRing::stem_count; ++s) fixed += (mask >> s) & 1;
        const unsigned room = stems_a_pass() > fixed ? stems_a_pass() - fixed : 0, later = std::min(stems_a_pass() - 1, StemRing::pad_most);
        const unsigned first = std::min({unsigned(stem_pads.size()), room, StemRing::pad_most});
        const size_t rest = stem_pads.size() - first;
        stem_passes = 1 + unsigned((rest + later - 1) / later);
        stem_pass = 1;
        stem_pads_taken = first;
        stem_name = name;
        StemRing* ring = library->stem_ring();
        ring->arm(mask, first);
        for (unsigned n = 0; n < first; ++n) {
            ring->set_pad(n, stem_pads[n].track, stem_pads[n].pad);
            job.pad_paths[n] = stem_pads[n].path;
            job.pad_tracks[n] = stem_pads[n].track;
        }
        job.pass = 1; job.passes = uint8_t(std::min(stem_passes, 255u));
        host.set_stems(ring);
    } else stem_pass = stem_passes = 0;
    if (!library->render(job)) { host.set_stems(nullptr); stem_pass = stem_passes = 0; notice("Mixdown: busy"); return; }
    if (ui.recording) set_recording(false);
    ui.rec_armed = false;
    send({Type::Stop});
    send({Type::BounceArm, 0, 0, 0, 1});
    send({Type::Play, 0, 0, 0, 1}); // the song from bar 1
    played_song = true;
    ui.rendering = 2; ui.render_pad = -1; ui.render_slot = -1;
    render_started = false; render_since = now;
    notice(stem_passes > 1 ? "Stems: pass 1 of " + std::to_string(stem_passes) : std::string("Mixdown: playing the song"));
}

// The stems one pass of the song writes, the mix counted: as many as the card keeps up with as the song plays (48 at
// 44.1 kHz; StemWriter's big chunks take the MPC's card to about 18 MB/s, a stem 0.35 MB/s), fewer at a higher rate.
unsigned Context::stems_a_pass() const {
    const unsigned rate = std::max(8000u, host.bounce().rate());
    return std::max(8u, 48u * 44100u / rate);
}

// The kit pads the song plays, each a pad stem: a kit track's pads its clips' patterns hit (a muted clip's none),
// with the pads those link to (Pad Link), each holding a sound; a frozen kit's none (its print plays). Each written
// as "NN <track> pads/A01 <sample>.wav" in the stems' folder, NN its track's stem's number.
void Context::song_pad_stems() {
    stem_pads.clear();
    auto clean = [](std::string text) { // a name a file can have
        for (auto& c : text) if (std::string("/\\:*?\"<>|").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) c = '-';
        while (!text.empty() && (text.back() == ' ' || text.back() == '.')) text.pop_back();
        return text;
    };
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& tr = project.tracks[t];
        if (tr.engine != EngineKind::Sampler || !tr.used() || frozen(t)) continue;
        std::array<bool, sampler_pads> hit {};
        for (unsigned n = 0; n < project.song.count; ++n) {
            const auto& clip = project.song.clips[n];
            if (clip.track != t || clip.muted) continue;
            for (const auto& e : patterns.at[t][clip.pattern % pattern_count].events) {
                const int pad = int(e.key) - int(sampler_first_note);
                if (pad >= 0 && pad < int(sampler_pads)) hit[size_t(pad)] = true;
            }
        }
        for (unsigned pad = 0; pad < sampler_pads; ++pad) { // a linked pad plays with it, and what that one links to
            if (!hit[pad]) continue;
            unsigned at = pad;
            for (unsigned k = 0; k < 16 && tr.pads[at].link; ++k) {
                at = at / 16 * 16 + (tr.pads[at].link - 1u) % 16;
                if (hit[at]) break;
                hit[at] = true;
            }
        }
        const std::string kit = clean(tr.name[0] ? tr.name : "Track " + std::to_string(t + 1));
        for (unsigned pad = 0; pad < sampler_pads; ++pad) {
            if (!hit[pad] || !tr.pads[pad].used()) continue;
            PadStem stem;
            stem.track = uint8_t(t); stem.pad = uint8_t(pad);
            std::string sample = clean(std::filesystem::path(project.samples[tr.pads[pad].sample % sample_slots].path).stem().string());
            if (sample.empty()) sample = "Pad";
            std::snprintf(stem.path.data(), stem.path.size(), "%02u %.24s pads/%c%02u %.32s", t + 1, kit.c_str(), char('A' + pad / 16), pad % 16 + 1, sample.c_str());
            stem_pads.push_back(stem);
        }
    }
}

// The next pass of the song for the pad stems the passes before could not take, into the same folder; false when it
// could not start.
bool Context::next_stem_pass(uint32_t now) {
    if (!library || stem_pads_taken >= stem_pads.size()) return false;
    SampleLibrary::Render job;
    job.to_file = job.stems = job.more = true;
    std::snprintf(job.name, sizeof(job.name), "%s", stem_name.c_str());
    const unsigned count = std::min({unsigned(stem_pads.size() - stem_pads_taken), stems_a_pass() - 1, StemRing::pad_most});
    StemRing* ring = library->stem_ring();
    ring->arm(0, count);
    for (unsigned n = 0; n < count; ++n) {
        const auto& stem = stem_pads[stem_pads_taken + n];
        ring->set_pad(n, stem.track, stem.pad);
        job.pad_paths[n] = stem.path;
        job.pad_tracks[n] = stem.track;
    }
    job.pass = uint8_t(std::min(stem_pass + 1, 255u)); job.passes = uint8_t(std::min(stem_passes, 255u));
    host.set_stems(ring);
    if (!library->render(job)) { host.set_stems(nullptr); return false; }
    stem_pads_taken += count;
    ++stem_pass;
    send({Type::Stop});
    send({Type::BounceArm, 0, 0, 0, 1});
    send({Type::Play, 0, 0, 0, 1}); // the song from bar 1, as the first pass
    played_song = true;
    ui.rendering = 2;
    render_started = false; render_since = now;
    notice("Stems: pass " + std::to_string(stem_pass) + " of " + std::to_string(stem_passes));
    return true;
}

// A render runs while the engine host's is busy; once it is back to Idle
// (the service thread took it all), Resample stops what it started or lets
// what was playing play on.
void Context::render_tick(uint32_t at) {
    if (!ui.rendering) return;
    // A song onto a pad past 60 s: its first 60 s are in; the rest is not wanted (roadmap P3).
    if (ui.rendering == 3 && ui.render_stop_at && int32_t(at - ui.render_stop_at) >= 0) { ui.render_stop_at = 0; if (host.playing()) send({Type::Stop}); }
    auto state = host.bounce().state();
    if (state != Bounce::State::Idle) { render_started = true; render_since = at; }
    // A pattern render ends on its last frame; the transport is stopped then, not at the next pass.
    if (ui.rendering == 1 && !ui.render_resume && (state == Bounce::State::Done) && host.playing()) send({Type::Stop});
    if (state != Bounce::State::Idle || (!render_started && at - render_since < 2000)) return;
    if (ui.rendering == 2 && stem_passes) { // stems: their files closed first, then the next pass, if any
        const int result = library ? library->stems_result() : -1;
        if (render_started && result == 0) return;
        if (render_started && result > 0 && stem_pass < stem_passes && next_stem_pass(at)) return;
        stem_pass = stem_passes = 0;
    }
    if (ui.rendering == 1 && !ui.render_resume && host.playing()) send({Type::Stop});
    ui.rendering = 0;
    render_started = false;
}

} // namespace melodee
