// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Renders of the mix (parity phase 6), on the MIDI thread:
//
// - Resample (Shift + Pad 11, as printed): then a pad; the selected track's
//   pattern (with every other track's playing alongside) is played from its
//   start and rendered onto that pad, as Akai's editing_sequences page has
//   it. Up to 60 s, the longest sample a pad loads. One undo.
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
#include "melodee/shell/smf_jobs.h"
#include <cstdio>

namespace melodee {
using Type = Command::Type;

void Context::resample() {
    if (ui.rendering) { notice("Rendering already"); return; }
    if (ui.resample_pick) { ui.resample_pick = false; notice("Resample off"); return; }
    if (track().engine != EngineKind::Sampler) { notice("Resample: pick a kit track"); return; }
    const bool song = ui.page == PageId::Song && project.song.length(); // roadmap P3: on Song, the song
    bool notes = song; // the patterns the tracks play: any notes to render?
    for (unsigned t = 0; t < track_count && !notes; ++t) notes = project.tracks[t].used() && !patterns.at[t][project.tracks[t].pattern % pattern_count].empty();
    if (!notes) { notice("Resample: no notes to play"); return; }
    ui.resample_pick = true;
    ui.resample_song = song;
    notice(song ? "Resample song: tap a pad" : "Resample: tap a pad");
}

// Roadmap P3: the song onto `kit`, as the mixdown plays it (from bar 1, once, loop or not, on through its tails), into
// a slot instead of a file; past 60 s its first 60 s, the song then stopped.
void Context::resample_song(unsigned kit) {
    double tick_frames = 44100.0 * 60.0 / (tempo_now() / 10.0) / ticks_per_quarter;
    double song_frames = double(project.song.length()) * project.bar_ticks() * tick_frames;
    bool cut = song_frames > TakeRecorder::max_frames;
    int slot = hold_file("RESAMPLE", ui.track, int(kit), 1);
    if (slot < 0) { notice("Resample: samples full"); return; }
    SampleLibrary::Render job;
    job.slot = uint8_t(slot);
    job.frames = uint32_t(std::min<double>(song_frames, TakeRecorder::max_frames)) + 1;
    if (!library->render(job)) { drop_file(slot); notice("Resample: busy"); return; }
    ui.render_resume = false;
    if (ui.recording) set_recording(false);
    ui.rec_armed = false;
    send({Type::Stop});
    send({Type::BounceArm, 0, 0, 0, 1});
    send({Type::Play, 0, 0, 0, 1}); // the song from bar 1
    played_song = true;
    ui.rendering = 3; ui.render_pad = int16_t(kit); ui.render_slot = int16_t(slot); ui.render_track = ui.track;
    // 60 s of 44.1 kHz frames, and a moment for the last of them to be drained
    ui.render_stop_at = cut ? (now + uint32_t(uint64_t(TakeRecorder::max_frames) * 1000 / 44100) + 500) | 1 : 0;
    render_started = false; render_since = now;
    notice(pad_text(cut ? "Song 60 s to" : "Song to", kit));
}

void Context::resample_pad(unsigned kit) {
    ui.resample_pick = false;
    if (!library || track().engine != EngineKind::Sampler) return;
    if (host_recording()) { notice("Resample: a take runs"); return; }
    kit %= sampler_pads;
    if (ui.resample_song) { ui.resample_song = false; resample_song(kit); return; }
    // The pattern's length, at most 60 s at the tempo in play.
    double tick_frames = 44100.0 * 60.0 / (tempo_now() / 10.0) / ticks_per_quarter;
    uint32_t ticks = std::max<uint32_t>(pattern().length, 1);
    auto most = uint32_t(TakeRecorder::max_frames / tick_frames);
    bool cut = ticks > most;
    ticks = std::min(ticks, most);
    int slot = hold_file("RESAMPLE", ui.track, int(kit), 1);
    if (slot < 0) { notice("Resample: samples full"); return; }
    SampleLibrary::Render job;
    job.slot = uint8_t(slot);
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
    if (ui.export_kind == 4) { export_midi(*this, name); return; } // roadmap P4: a MIDI file, written at once
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
        StemRing* ring = library->stem_ring();
        ring->arm(mask);
        host.set_stems(ring);
    }
    if (!library->render(job)) { host.set_stems(nullptr); notice("Mixdown: busy"); return; }
    if (ui.recording) set_recording(false);
    ui.rec_armed = false;
    send({Type::Stop});
    send({Type::BounceArm, 0, 0, 0, 1});
    send({Type::Play, 0, 0, 0, 1}); // the song from bar 1
    played_song = true;
    ui.rendering = 2; ui.render_pad = -1; ui.render_slot = -1;
    render_started = false; render_since = now;
    notice("Mixdown: playing the song");
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
    if (ui.rendering == 1 && !ui.render_resume && host.playing()) send({Type::Stop});
    ui.rendering = 0;
    render_started = false;
}

} // namespace melodee
