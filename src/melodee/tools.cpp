// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/tools.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/prophet_params.h"
#include "melodee/engine/sampler.h"
#include "melodee/engine/take_recorder.h"
#include "melodee/model/scales.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/pages/page.h"
#include "melodee/shell/shell.h"
#include "runtime/screen.h"
#include "runtime/util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace melodee {
namespace {

// The boards' sample content, close enough to compare page by page. Names
// and numbers are placeholders, as on the boards.
void board_project(Shell& shell) {
    auto& project = shell.project();
    std::snprintf(project.name, sizeof(project.name), "Night drive");
    project.tempo = 1240;
    project.root = 2; project.scale = 2; // D Dorian
    const unsigned sounds[] {1, 2, 6, 7, 12, 5, 3, 4, 11, 9};
    for (unsigned t = 0; t < 10; ++t) load_sound(project.tracks[t], EngineKind::Prophet, sounds[t]);
    const uint8_t levels[] {98, 88, 74, 56, 104, 64, 81, 51, 70, 46};
    for (unsigned t = 0; t < 10; ++t) project.tracks[t].mix.level = levels[t];
    const int8_t pans[] {0, -12, 20, 0, 0, -20, 12, 25, -6, 0};
    for (unsigned t = 0; t < 10; ++t) project.tracks[t].mix.pan = pans[t];
    project.tracks[3].mix.mute = true;
    project.tracks[0].pattern = 1;
    project.tracks[0].set_name("FUNK BASS II");
    for (unsigned t = 1; t < 7; ++t) {
        auto& p = shell.patterns().at[t][0];
        p.steps[0].notes[0] = {48, 100};
    }
    // Track 1's pattern 2: the bass line of the Steps board, in each of its four bars.
    auto& line = shell.patterns().at[0][1];
    line = shell.patterns().at[0][0];
    line.length = 64;
    for (unsigned s = 16; s < 64; ++s) {
        line.steps[s] = line.steps[s % 16];
        for (auto& n : line.steps[s].notes) if (n.velocity) n.key = uint8_t(n.key + 12); // as on the Roll board
    }
    line.steps[22].notes[0] = {48, 96}; line.steps[22].length = 2; line.steps[22].flags = step_accent;
    // The Patterns board: which patterns hold notes, a small tune in each.
    const char* state[] {"dpdeeeeedeeeeeee", "pdeeeeeeeeeeeeee", "pqddeeeedeeeeeee", "ddpeeeeeeeeeeeee",
                         "dpddeeeeeeeeeeee", "pdeeeeeeeeeeeeee", "pdeeeeeeeeeeeeee", "deeeeeeeeeeeeeee"};
    const int tune[] {0, 3, 7, 5, 10, 7};
    const unsigned at[] {0, 3, 6, 8, 11, 14};
    for (unsigned t = 0; t < 8; ++t)
        for (unsigned n = 0; n < 16; ++n) {
            if (state[t][n] == 'e' || (t == 0 && n == 1)) continue;
            auto& pattern = shell.patterns().at[t][n];
            for (unsigned k = 0; k < 6; ++k) pattern.steps[at[k] + (n % 2)].notes[0] = {uint8_t(48 + tune[(k + n + t) % 6]), 100};
        }
    // The Song board: markers, clips and the loop.
    auto& song = project.song;
    song.markers[0] = {16, "VERSE"}; song.markers[1] = {24, "DROP"}; song.marker_count = 2;
    const Clip clips[] {{0, 0, 16, 8, false}, {0, 1, 24, 8, false}, {1, 0, 16, 4, false}, {1, 1, 24, 8, false},
                        {2, 0, 16, 8, false}, {2, 2, 24, 2, false}, {2, 2, 26, 6, false}, {3, 2, 16, 16, false},
                        {4, 0, 20, 4, false}, {4, 1, 24, 8, false}, {5, 0, 24, 8, false}, {7, 0, 28, 4, false}};
    for (const auto& clip : clips) song.place(clip);
    song.loop_start = 24; song.loop_end = 32;
    // The effects boards: track 3's chain, four buses, the master.
    auto put = [&](Chain& chain, unsigned slot, EffectKind kind) {
        chain.slots[slot].kind = uint8_t(kind);
        const auto& info = effect_info(kind);
        for (unsigned q = 0; q < effect_params; ++q) chain.slots[slot].params[q] = info.params[q].label ? info.params[q].initial : 0;
    };
    put(project.tracks[2].fx, 0, EffectKind::Chorus); put(project.tracks[2].fx, 1, EffectKind::Tape); put(project.tracks[2].fx, 2, EffectKind::Eq);
    put(project.buses[0].fx, 0, EffectKind::Room); put(project.buses[0].fx, 1, EffectKind::Echo); put(project.buses[0].fx, 2, EffectKind::Width);
    put(project.buses[1].fx, 0, EffectKind::Echo); put(project.buses[2].fx, 0, EffectKind::Crush);
    put(project.master, 0, EffectKind::Compressor); put(project.master, 1, EffectKind::Limiter);
    project.buses[0].level = 70; project.buses[1].level = 55; project.buses[2].level = 40;
    const uint8_t sends[] {8, 0, 42, 60, 4, 72, 55, 30, 24, 80};
    for (unsigned t = 0; t < 10; ++t) project.tracks[t].mix.send = sends[t];
    // The sampler boards: track 4 is a kit, its first pads one-shots, then a break chopped in twelve.
    {
        auto& kit = project.tracks[3];
        load_sound(kit, EngineKind::Sampler, 0);
        kit.set_name("DUST KIT");
        const char* names[] {"KICK", "SNARE", "HAT", "OPEN", "DUST BREAK"};
        const uint32_t lengths[] {22050, 26460, 8820, 30870, 230087};
        for (unsigned n = 0; n < 5; ++n) {
            std::snprintf(project.samples[n].path, sizeof(project.samples[n].path), "/media/card/Samples/%s.wav", names[n]);
            std::snprintf(project.samples[n].name, sizeof(project.samples[n].name), "%s", names[n]);
            project.samples[n].frames = lengths[n];
        }
        const char* short_names[] {"Kick", "Snare", "Hat", "Open"};
        for (unsigned n = 0; n < 4; ++n) { kit.pads[n].sample = uint16_t(n); std::snprintf(project.samples[n].name, sizeof(project.samples[n].name), "%s", short_names[n]); }
        for (unsigned n = 0; n < 12; ++n) {
            kit.pads[4 + n].sample = 4; kit.pads[4 + n].start = 230087 * n / 12; kit.pads[4 + n].end = 230087 * (n + 1) / 12;
        }
        kit.last_pad = 8;
    }
    // The Mod board's six routes on track 1.
    const ModRoute routes[] {
        {uint8_t(ModSource::Lfo), uint8_t(ModDest::Cutoff), 34}, {uint8_t(ModSource::Velocity), uint8_t(ModDest::AmpLevel), 60},
        {uint8_t(ModSource::Aftertouch), uint8_t(ModDest::OscBPitch), -18}, {uint8_t(ModSource::Wheel), uint8_t(ModDest::LfoRate), 45},
        {uint8_t(ModSource::KeyTrack), uint8_t(ModDest::Cutoff), 50}, {uint8_t(ModSource::Random), uint8_t(ModDest::PulseWidth), 30},
    };
    for (unsigned r = 0; r < 6; ++r) project.tracks[0].mods[r] = routes[r];
    // Motion on CUTOFF and RESO, as drawn on the Motion board.
    const int cutoff[] {40, 44, 52, 60, 72, 80, 84, 80, 70, 62, 54, 48, 44, 50, 58, 62};
    for (unsigned n = 0; n < 16; ++n) project.motion.write({0, 1, uint16_t(n * 4), uint8_t(prophet::Cutoff), int16_t(cutoff[n])});
    const int reso[] {40, 52, 60, 52, 40, 35};
    for (unsigned n = 0; n < 6; ++n) project.motion.write({0, 1, uint16_t(24 + n * 4), uint8_t(prophet::Resonance), int16_t(reso[n])});
}

// A two-bar break for the sampler previews: twelve hits fading away, drawn as its overview.
LoadedSample break_sample() {
    LoadedSample s;
    s.name = "DUST BREAK"; s.frames = 230087; s.ready = true;
    for (unsigned c = 0; c < s.overview.size(); ++c) {
        float in_hit = std::fmod(c * 12.0f / s.overview.size(), 1.0f);
        float level = (c % 86 < 40 ? 0.9f : 0.6f) * std::exp(-in_hit * 4.0f) * (0.75f + 0.25f * std::sin(c * 1.3f));
        s.overview[c] = uint8_t(std::clamp(level, 0.03f, 1.0f) * 255);
    }
    s.hit_count = 12;
    for (unsigned n = 0; n < 12; ++n) s.hits[n] = {230087u * n / 12, 200};
    return s;
}

// The Project board's card: two folders and four projects, Night drive open with changes.
void preview_projects(Snapshot& s) {
    auto& v = s.projects;
    v.root = "/media/card/Melodee/Projects"; v.free_bytes = 21400000000ull;
    using K = ProjectEntry::Kind;
    v.entries = {{K::Folder, "Live sets", v.root + "/Live sets", 0, 0, 6, 0}, {K::Folder, "Sketches", v.root + "/Sketches", 0, 0, 14, 0},
                 {K::Project, "Night drive", v.root + "/Night drive.mldp", 10, 1240, 0, 4}, {K::Project, "Tape loops", v.root + "/Tape loops.mldp", 7, 920, 0, 3},
                 {K::Project, "Sunday sketch", v.root + "/Sunday sketch.mldp", 3, 1100, 0, 2}, {K::Project, "Basement jam", v.root + "/Basement jam.mldp", 12, 1400, 0, 1}};
    s.project_path = v.root + "/Night drive.mldp";
    s.dirty = true;
}

struct Preview {
    const char* name;
    void (*set)(Shell&, Snapshot&);
};

const Preview previews[] {
    {"stage", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.playing = true; }},
    {"keys", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Keys; s.ui.held_pads = 1u << 4 | 1u << 9; s.playing = true;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"sounds", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Sounds; s.ui.track = 2; s.ui.sounds_cursor = 2; s.playing = true;
         auto engine = unsigned(s.project.tracks[2].engine);
         s.ui.settings.toggle_favourite(engine, 1); s.ui.settings.toggle_favourite(engine, 4);
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"edit", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.edit_page[0] = 3; s.playing = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
         s.ui.pots[1] = {81, false, Binding {Binding::Kind::Param, prophet::Resonance, 0, 100, 0}.key(0)};
     }},
    {"soundmap", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.edit_page[0] = 3; s.ui.map_open = true; s.ui.map_cursor = 3; s.playing = true;
     }},
    {"mod", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.edit_page[0] = 10; s.playing = true;
     }},
    {"mix", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Mix; s.ui.track = 2; s.playing = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
         s.ui.pots[2] = {90, false, Binding {Binding::Kind::Send, 0, 0, 100, 0}.key(2)};
         const float meters[] {0.5f, 0.42f, 0.2f, 0, 0.6f, 0.25f, 0.3f, 0.05f, 0.18f, 0.08f};
         for (unsigned t = 0; t < 10; ++t) s.meters[t] = meters[t];
     }},
    {"steps", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Steps; s.ui.step_cursor = 6; s.ui.held_step = 6; s.playing = true; s.position = 9;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"roll", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Roll; s.ui.step_cursor = 22; s.playing = true; s.position = 26; s.ui.recording = true;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"notesheet", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Roll; s.ui.step_cursor = 22; s.playing = true; s.position = 26; s.ui.sheet_open = true;
     }},
    {"motion", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Motion; s.playing = true; s.position = 40; s.ui.recording = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"patterns", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Patterns; s.ui.track = 2; s.playing = true;
         const uint8_t live[] {1, 0, 0, 2, 1, 0, 0, 0};
         for (unsigned t = 0; t < 8; ++t) { s.playing_patterns[t] = live[t]; s.project.tracks[t].pattern = live[t]; s.steps[t] = int(4 + t); }
         s.project.tracks[2].pattern = 1;
     }},
    {"song", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 24; s.ui.song_view = 16; s.ui.song_zoom = 2;
         s.ui.song_pattern = 2; s.ui.song_length = 4;
         s.playing = s.song_playing = true; s.song_bar = 20; s.ticks = 48;
         // Punches written with Seq Record: a stutter on the master, a low pass on track 3.
         s.project.song.add_punch({23, 12, 4, 4, 255, 100});
         s.project.song.add_punch({27, 0, 16, 8, 2, 80});
         s.project.song.add_punch({31, 8, 8, 1, 255, 100});
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"songzoom", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 26; s.ui.song_view = 24; s.ui.song_zoom = 0;
         s.playing = s.song_playing = true; s.song_bar = 25; s.ticks = 40;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"clipsheet", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 26; s.ui.song_view = 16; s.ui.sheet_open = true;
         s.playing = true;
     }},
    {"length", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Length; s.playing = true; s.steps[0] = 34; s.project.swing = 54;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"rack", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Rack; s.ui.track = 2; s.ui.fx_cursor[2] = 1; s.playing = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"sends", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Sends; s.ui.track = 2; s.playing = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"bus", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Bus; s.ui.track = 2; s.playing = true;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"fxpick", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Rack; s.ui.track = 2; s.ui.fx_cursor[2] = 3; s.playing = true;
         s.ui.picker_open = true; s.ui.picker_items = true; s.ui.picker_family = 2; s.ui.picker_item = 2;
     }},
    {"pads", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true; s.sample = break_sample(); s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"samples", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Samples; s.ui.track = 3; s.playing = true; s.ui.files_cursor = 2; s.sample_root = "/media/card/Samples";
         const char* files[] {"AMEN TYPE", "DUST BREAK", "FUNK LOOP 04", "RHODES STAB", "TAKE 1", "TAPE HIT", "VINYL CHORD", "WOOD BLOCK"};
         for (auto name : files) s.files.push_back({std::string("/media/card/Samples/") + name + ".wav", name, 0});
         s.files[1].path = s.project.samples[4].path;
         s.ui.files_count = uint16_t(s.files.size()); s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"trim", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Trim; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& pad = s.project.tracks[3].pads[8]; pad.start = 18000; pad.end = 150000;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"chop", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Chop; s.ui.track = 3; s.playing = true; s.sample = break_sample(); s.ui.chop_sample = 4;
         s.ui.slices = chop(ChopMode::Even, 12, s.sample.frames, s.sample.hits, s.sample.hit_count, 920);
         s.ui.chop_slice = 4; s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"levels", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Levels; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3]; t.levels = true; t.levels_pad = 1; t.last_pad = 1;
         s.sample.frames = 26460;
         s.ui.levels_hit = 10; s.ui.levels_value = level_velocity(10);
     }},
    {"record", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Record; s.ui.track = 3; s.playing = true; s.ui.rec_source = 1; s.ui.rec_bars = 4; s.ui.rec_gain = 6;
         s.ui.rec_pad = 16;
         s.take.state = uint8_t(TakeRecorder::State::Recording); s.take.length = 4 * 85355; s.take.frames = 85355 + 50000;
         s.take.level = 0.5f;
         for (unsigned c = 0; c < 128 * s.take.frames / s.take.length; ++c)
             s.take.peaks[c] = uint8_t(std::clamp((c % 22 < 3 ? 0.9 : 0.55) * (0.8 + 0.2 * std::sin(c * 0.9)) * (c < 4 ? 0.1 : 1.0), 0.0, 1.0) * 255);
         s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    {"project", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_cursor = 3;
         preview_projects(s);
     }},
    {"filesheet", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_cursor = 3; s.ui.sheet_open = true;
         preview_projects(s);
     }},
    {"tempo", [](Shell&, Snapshot& s) { s.ui.page = PageId::Tempo; s.playing = true; s.ticks = 24; s.project.click = true; s.project.swing = 54; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"arp", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Arp; s.ui.track = 2; s.playing = true;
         auto& t = s.project.tracks[2]; t.arp = true; t.chord = true; t.arp_octaves = 2; t.chord_type = 1;
         s.project.root = 2; s.project.scale = 2; t.octave = 2;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"chord", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Chord; s.ui.track = 2; s.playing = true; s.ui.chord_pad = 1;
         auto& t = s.project.tracks[2]; t.chord = true; t.chord_type = 1; t.octave = 2;
         s.project.root = 2; s.project.scale = 2;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"capture", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Capture; s.ui.track = 0; s.playing = true; s.ui.capture_into = 2;
         // Four bars of a bass line played on the pads, just now.
         double step = 60000.0 / 124.0 / 4;
         const int keys[] {38, 41, 36, 33, 45, 40, 38, 43};
         for (unsigned n = 0; n < 40; ++n)
             s.history[n] = {0, uint8_t(keys[n % 8] + (n % 5 == 0 ? 12 : 0)), 100, uint32_t(s.now - 64 * step + n * 1.6 * step), uint32_t(step * (1 + n % 3))};
         s.history_at = 40;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"padfx", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::PadFx; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3];
         t.last_pad = 1;
         auto& chain = t.pad_fx.at[1];
         const EffectKind kinds[] {EffectKind::Transient, EffectKind::Drive};
         for (unsigned k = 0; k < 2; ++k) {
             chain.slots[k].kind = uint8_t(kinds[k]);
             const auto& info = effect_info(kinds[k]);
             for (unsigned q = 0; q < effect_params; ++q) chain.slots[k].params[q] = info.params[q].label ? info.params[q].initial : 0;
         }
         chain.slots[0].params[1] = -20;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"settings", [](Shell&, Snapshot& s) { s.ui.page = PageId::SettingsPage; s.playing = true; s.ui.settings_cursor = 0; }},
    {"about", [](Shell&, Snapshot& s) { s.ui.page = PageId::About; s.playing = true; }},
    {"punch", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Punch; s.ui.track = 2; s.playing = true; s.project.punch_target = 1;
         s.ui.punch_held = 4; s.ui.punch_locked = 8; s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"padpunch", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::PadPunch; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3];
         const uint8_t own[][2] {{1, 5}, {2, 14}, {3, 6}, {4, 2}, {8, 8}, {11, 15}};
         for (auto& o : own) t.pads[o[0]].punch = o[1];
         t.last_pad = 1; s.ui.pad_punch_held = 1; s.ui.pad_punch_pressure = 92; t.pads[1].punch_depth = 72;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"pad", [](Shell&, Snapshot& s) { s.ui.page = PageId::PadPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.pitch = 3; pad.fine = -8; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"play", [](Shell&, Snapshot& s) { s.ui.page = PageId::PlayPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.last_pad = 3; t.pads[2].group = 1; t.pads[3].group = 1; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"envelope", [](Shell&, Snapshot& s) { s.ui.page = PageId::EnvelopePage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.attack = 30; pad.decay = 60; pad.sustain = 55; pad.release = 40; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"filter", [](Shell&, Snapshot& s) { s.ui.page = PageId::FilterPage; s.ui.track = 3; s.playing = true;
         s.project.tracks[3].params[sampler::Filter] = 81; s.project.tracks[3].params[sampler::Resonance] = 34; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"follow", [](Shell&, Snapshot& s) { s.ui.page = PageId::FollowPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.params[sampler::Follow] = 1; t.last_pad = 4; t.pads[4].start = 0; t.pads[4].end = 230087; t.pads[4].own_tempo = 920;
         s.ui.turning = 1; s.ui.turned_at = s.now; }},
    {"cloud", [](Shell&, Snapshot& s) { s.ui.page = PageId::CloudPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.params[sampler::Cloud] = 1; t.last_pad = 4; t.pads[4].start = 0; t.pads[4].end = 230087; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"shift", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.shift = true; s.playing = true; }},
    {"exit", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.exit_open = true; }},
};

void draw(juce::Graphics& g, const Snapshot& s) {
    ui::Paint p(g);
    g.fillAll(ui::colour::bg);
    page_spec(s.ui.page).draw(p, s);
    if (s.ui.exit_open) draw_exit_sheet(p, s);
    else if (s.ui.shift && !s.ui.shift_used) draw_shift_layer(p, s);
}

int preview(const std::string& folder, const std::vector<std::string>& names) {
    EngineHost host;
    Shell shell(host);
    board_project(shell);
    unsigned written = 0;
    for (const auto& preview : previews) {
        if (!names.empty() && std::find(names.begin(), names.end(), preview.name) == names.end()) continue;
        auto snapshot = std::make_unique<Snapshot>();
        shell.snapshot(*snapshot, 100000);
        snapshot->ui = UiState {};
        snapshot->now = 100000;
        preview.set(shell, *snapshot);
        // The pattern in view follows the track the preview picked.
        const auto& track = snapshot->project.tracks[snapshot->ui.track];
        snapshot->pattern = shell.patterns().at[snapshot->ui.track][track.pattern];
        for (int scale : {1, 2}) {
            juce::Image image(juce::Image::ARGB, screen_width * scale, screen_height * scale, true);
            {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(float(scale)));
                draw(g, *snapshot);
            }
            save_png(image, folder + "/" + preview.name + (scale == 2 ? "@2x" : "") + ".png");
        }
        ++written;
    }
    require(written > 0, "Unknown Melodee page; use stage keys sounds edit soundmap mod mix steps roll notesheet motion patterns song songzoom clipsheet length rack sends bus fxpick pads samples trim chop levels record project filesheet tempo arp chord capture pad play envelope filter follow cloud shift exit");
    std::cout << "MELODEE_PREVIEW pages=" << written << '\n';
    return 0;
}

void little(std::ofstream& file, uint32_t value, unsigned bytes) {
    for (unsigned n = 0; n < bytes; ++n) file.put(char((value >> (n * 8)) & 255));
}

// Two bars of the starting project at 120 BPM (4 s), then two seconds of
// tail: the bass line must sound, stay below full scale and end in silence.
int sound(const std::string& path) {
    EngineHost host;
    Shell shell(host);
    host.load(shell.project(), shell.patterns());
    host.send({Command::Type::Play});
    constexpr unsigned rate = 44100, frames = rate * 6, block = EngineHost::block;
    std::ofstream file(path, std::ios::binary);
    require(bool(file), "Create Melodee test audio");
    file.write("RIFF", 4); little(file, 36 + frames * 4, 4); file.write("WAVEfmt ", 8);
    little(file, 16, 4); little(file, 1, 2); little(file, 2, 2); little(file, rate, 4);
    little(file, rate * 4, 4); little(file, 4, 2); little(file, 16, 2);
    file.write("data", 4); little(file, frames * 4, 4);
    std::array<int32_t, block * 2> samples {};
    double energy = 0, tail = 0;
    float peak = 0;
    for (unsigned offset = 0; offset < frames; offset += block) {
        if (offset >= rate * 4 && offset < rate * 4 + block) host.send({Command::Type::Stop});
        host.render(samples.data(), block, 0.7f);
        for (unsigned i = 0; i < block * 2; ++i) {
            float value = samples[i] / 2147483648.0f;
            peak = std::max(peak, std::fabs(value));
            energy += double(value) * value;
            if (offset > rate * 5) tail += double(value) * value;
            little(file, uint16_t(samples[i] >> 16), 2);
        }
    }
    double rms = std::sqrt(energy / (frames * 2)), tail_rms = std::sqrt(tail / (rate * 2));
    std::cout << "MELODEE_SOUND peak=" << peak << " rms=" << rms << " tail_rms=" << tail_rms << '\n';
    require(file.good() && rms > 0.005 && peak < 0.999f && tail_rms < 0.001, "Melodee sound check failed");
    return 0;
}

} // namespace

std::optional<int> run_tool(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "--melodee-preview")) {
        std::vector<std::string> names(argv + 3, argv + argc);
        return preview(argv[2], names);
    }
    if (argc == 3 && !std::strcmp(argv[1], "--melodee-sound")) return sound(argv[2]);
    return std::nullopt;
}

} // namespace melodee
