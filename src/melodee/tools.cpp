// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/tools.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/expansion_install.h"
#include "melodee/shell/mpc_program.h"
#include "melodee/shell/mpc_project.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/seq_layer.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/engine/sampler.h"
#include "melodee/engine/take_recorder.h"
#include "melodee/model/scales.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/model/fader.h"
#include "melodee/model/pad_params.h"
#include "melodee/pages/page.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/midi_config_rows.h"
#include "melodee/ui/boot.h"
#include "melodee/ui/shades.h"
#include "drivers/usb_audio.h"
#include "runtime/screen.h"
#include "runtime/util.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace melodee {
namespace {

// The boards' sample content, close enough to compare page by page. Names
// and numbers are placeholders, as on the boards.
// One note on a step of a pattern's Q, in place of what the step held.
void put(Pattern& p, unsigned step, uint8_t key, uint8_t velocity, uint8_t length = 1, uint8_t flags = 0) {
    Step s;
    s.notes[0] = {key, velocity};
    s.length = length; s.flags = flags;
    p.set_step(step, s);
}

// Track `t` a PLUGIN track on Melodee's synth `synth` (0 Melodee Poly, 1 PD, 2 Chip), in plug-in entry `entry`,
// named `name`: what a synth track is since the synths became plug-ins (they need not be on a card to be drawn).
void synth_track(Project& project, unsigned t, unsigned synth, unsigned entry, const char* name) {
    static const char* const folders[] {"Ellic Studio - VST - Melodee Poly", "Ellic Studio - VST - Melodee PD",
                                        "Ellic Studio - VST - Melodee Chip"};
    static const char* const names[] {"Melodee Poly", "Melodee PD", "Melodee Chip"};
    load_sound(project.tracks[t], EngineKind::Plugin, 0);
    project.tracks[t].plugin = uint8_t(entry);
    project.tracks[t].set_name(name);
    auto& ref = project.plugins[entry];
    std::snprintf(ref.path, sizeof(ref.path), "Plugins/%s", folders[synth % 3]);
    std::snprintf(ref.name, sizeof(ref.name), "%s", names[synth % 3]);
}

// The drum kit of the starter project: pads A01-A03 kick, snare and hat, in sample slots 0-2.
constexpr const char* drum_names[] {"KICK", "SNARE", "HAT"};
void drum_kit(Project& project, unsigned t) {
    load_sound(project.tracks[t], EngineKind::Sampler, 0);
    project.tracks[t].set_name("DRUMS");
    for (unsigned n = 0; n < 3; ++n) {
        std::snprintf(project.samples[n].path, sizeof(project.samples[n].path), "/media/card/Samples/%s.wav", drum_names[n]);
        std::snprintf(project.samples[n].name, sizeof(project.samples[n].name), "%s", drum_names[n]);
        project.samples[n].frames = 22050;
        project.tracks[t].pads[n].sample = uint16_t(n);
        project.tracks[t].pads[n].play = PadPlay::OneShot;
    }
}

// A full band for the previews and the sound check: four synth tracks (Melodee's synths, as plug-ins), an empty
// kit and a drum kit, a bass line in D and a beat.
void starter_project(Project& project, PatternBank& patterns) {
    project = Project {};
    std::snprintf(project.name, sizeof(project.name), "Starter");
    project.tempo = 1200;
    synth_track(project, 0, 0, 0, "BASS");
    synth_track(project, 1, 0, 1, "PAD");
    synth_track(project, 2, 1, 2, "KEYS");
    synth_track(project, 3, 2, 3, "LEAD");
    load_sound(project.tracks[4], EngineKind::Sampler, 0); // an empty kit
    drum_kit(project, 5);                                   // drums, with a beat
    project.tracks[1].mix.level = 80; project.tracks[1].octave = 3;
    project.tracks[2].octave = 4; project.tracks[3].octave = 4;
    patterns = PatternBank {};
    auto note = [](Pattern& p, unsigned step, uint8_t key, uint8_t velocity, unsigned length) {
        Event e;
        e.tick = step * sixteenth_ticks; e.length = uint16_t(length * sixteenth_ticks); e.key = key; e.velocity = velocity;
        p.add(e);
    };
    struct { unsigned step; uint8_t key, length, velocity; } line[] {
        {0, 38, 2, 110}, {3, 38, 1, 70}, {4, 41, 1, 96}, {6, 36, 2, 96}, {8, 33, 3, 120},
        {11, 36, 1, 64}, {12, 38, 2, 104}, {14, 31, 2, 88},
    };
    for (auto& n : line) note(patterns.at[0][0], n.step, n.key, n.velocity, n.length);
    // The kit's pads A01-A03: kick, snare, hat.
    auto& beat = patterns.at[5][0];
    const uint8_t kick = sampler_first_note, snare = sampler_first_note + 1, hat = sampler_first_note + 2;
    for (unsigned st = 0; st < 16; st += 4) note(beat, st, kick, 120, 1);
    for (unsigned st : {4u, 12u}) note(beat, st, snare, 110, 1);
    for (unsigned st = 2; st < 16; st += 4) note(beat, st, hat, 90, 1);
}

void board_project(Shell& shell) {
    starter_project(shell.project(), shell.patterns()); // the previews show a full project, not a new one
    auto& project = shell.project();
    std::snprintf(project.name, sizeof(project.name), "Project 1");
    project.tempo = 1240;
    project.root = 2; project.scale = 2; // D Dorian
    const char* const sounds[] {"BASS 1", "BASS 2", "PLUCK", "KEYS", "HAT", "PAD",
                                "LEAD 1", "BRASS", "LEAD 2", "STRINGS"};
    for (unsigned t = 0; t < 10; ++t) synth_track(project, t, t % 3, t, sounds[t]);
    const uint8_t levels[] {98, 88, 74, 56, 104, 64, 81, 51, 70, 46};
    for (unsigned t = 0; t < 10; ++t) project.tracks[t].mix.level = levels[t];
    const int8_t pans[] {0, -12, 20, 0, 0, -20, 12, 25, -6, 0};
    for (unsigned t = 0; t < 10; ++t) project.tracks[t].mix.pan = pans[t];
    project.tracks[3].mix.mute = true;
    project.tracks[0].pattern = 1;
    for (unsigned t = 1; t < 7; ++t) {
        put(shell.patterns().at[t][0], 0, 48, 100);
    }
    // Track 1's pattern 2: the bass line of the Steps board, in each of its four bars.
    auto& line = shell.patterns().at[0][1];
    line = shell.patterns().at[0][0];
    line.length = 64 * sixteenth_ticks;
    for (unsigned s = 16; s < 64; ++s) {
        Step step = line.step(s % 16);
        for (auto& n : step.notes) if (n.velocity) n.key = uint8_t(n.key + 12); // as on the Roll board
        line.set_step(s, step);
    }
    put(line, 22, 48, 96, 2, step_accent);
    { Step played = line.step(26); for (auto& n : played.notes) if (n.velocity) n.offset = 7; line.set_step(26, played); } // played late, Rec Quantize off
    // The Patterns board: which patterns hold notes, a small tune in each.
    const char* state[] {"dpdeeeeedeeeeeee", "pdeeeeeeeeeeeeee", "pqddeeeedeeeeeee", "ddpeeeeeeeeeeeee",
                         "dpddeeeeeeeeeeee", "pdeeeeeeeeeeeeee", "pdeeeeeeeeeeeeee", "deeeeeeeeeeeeeee"};
    const int tune[] {0, 3, 7, 5, 10, 7};
    const unsigned at[] {0, 3, 6, 8, 11, 14};
    for (unsigned t = 0; t < 8; ++t)
        for (unsigned n = 0; n < 16; ++n) {
            if (state[t][n] == 'e' || (t == 0 && n == 1)) continue;
            auto& pattern = shell.patterns().at[t][n];
            for (unsigned k = 0; k < 6; ++k) put(pattern, at[k] + (n % 2), uint8_t(48 + tune[(k + n + t) % 6]), 100);
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
    for (unsigned t = 0; t < 10; ++t) project.tracks[t].mix.sends[0] = sends[t];
    project.tracks[0].mix.sends[1] = 20; // the first track feeds a second bus too (Stage SENDS)
    // The sampler boards: track 4 is a kit, its first pads one-shots, then a break chopped in twelve.
    {
        auto& kit = project.tracks[3];
        load_sound(kit, EngineKind::Sampler, 0);
        kit.set_name("DRUM KIT");
        const char* names[] {"KICK", "SNARE", "HAT", "OPEN", "BREAK"};
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
    // Motion on the kit's FILTER and RESO, as drawn on the Motion board (its pattern 2; a plug-in's own values
    // take no Motion).
    const int cutoff[] {40, 44, 52, 60, 72, 80, 84, 80, 70, 62, 54, 48, 44, 50, 58, 62};
    auto point = [&](unsigned sixteenth, unsigned param, int value) {
        MotionPoint m;
        m.tick = sixteenth * sixteenth_ticks; m.param = uint8_t(param); m.value = int16_t(value);
        shell.patterns().at[3][1].write_motion(m);
    };
    shell.patterns().at[3][1].length = 64 * sixteenth_ticks;
    for (unsigned n = 0; n < 16; ++n) point(n * 4, sampler::Filter, cutoff[n]);
    const int reso[] {40, 52, 60, 52, 40, 35};
    for (unsigned n = 0; n < 6; ++n) point(24 + n * 4, sampler::Resonance, reso[n]);
}

// A two-bar break for the sampler previews: twelve hits fading away, drawn as its overview.
LoadedSample break_sample() {
    LoadedSample s;
    s.name = "BREAK"; s.frames = 230087; s.ready = true;
    for (unsigned c = 0; c < s.overview.size(); ++c) {
        float in_hit = std::fmod(c * 12.0f / s.overview.size(), 1.0f);
        float level = (c % 86 < 40 ? 0.9f : 0.6f) * std::exp(-in_hit * 4.0f) * (0.75f + 0.25f * std::sin(c * 1.3f));
        float right = level * (0.8f + 0.2f * std::cos(c * 0.7f)); // stereo: the right side a little apart
        s.sides[0][c] = uint8_t(std::clamp(level, 0.03f, 1.0f) * 255);
        s.sides[1][c] = uint8_t(std::clamp(right, 0.03f, 1.0f) * 255);
        s.overview[c] = std::max(s.sides[0][c], s.sides[1][c]);
    }
    s.stereo = true;
    s.hit_count = 12;
    for (unsigned n = 0; n < 12; ++n) s.hits[n] = {230087u * n / 12, 200};
    return s;
}

// Pads B: each used pad of the bank in view as a waveform of its own, a hit or a few and their decay.
void pad_waves(Snapshot& s) {
    const auto& t = s.project.tracks[s.ui.track % track_count];
    s.pad_waves_shown = 0;
    for (unsigned n = 0; n < 16; ++n) {
        if (!t.pads[t.bank % sampler_banks * 16 + n].used()) continue;
        unsigned hits = n < 4 ? 1 : 1 + n % 3;
        float decay = 2.0f + float(n % 4);
        for (unsigned k = 0; k < 32; ++k) {
            float in = std::fmod(float(k * hits) / 32, 1.0f);
            s.pad_waves[n][k] = uint8_t(std::clamp(std::exp(-in * decay) * (0.8f + 0.2f * std::sin(float(k) * 1.7f + float(n))), 0.04f, 1.0f) * 255);
        }
        s.pad_waves_shown = uint16_t(s.pad_waves_shown | 1u << n);
    }
}

// Track 3's pad `pad` holding a Logic EXS24 instrument: 22 zones over C0..C7 at
// every velocity, all its files read, Note On as a loaded instrument is.
void choir_pad(Snapshot& s, unsigned pad) {
    auto& sound = s.project.tracks[3].pads[pad % sampler_pads];
    auto& ref = s.project.samples[sound.sample % sample_slots];
    std::snprintf(ref.path, sizeof(ref.path), "/media/card/Samples/Choir/Choir.exs");
    std::snprintf(ref.name, sizeof(ref.name), "CHOIR");
    ref.frames = 0;
    sound.play = PadPlay::Gate; sound.start = sound.end = 0;
    LoadedSample m;
    m.name = ref.name; m.path = ref.path; m.instrument = true; m.ready = true;
    m.zones = 22; m.parts = 22; m.parts_ready = 22; m.low_key = 24; m.high_key = 108;
    for (unsigned key = 24; key <= 108; ++key) m.map[key] = 0xffff;
    for (unsigned n = 0; n < 22; ++n) { // a zone a file, over C1..C8 (round 3 "instrument C")
        ZoneShape z;
        z.low_key = uint8_t(24 + n * 85 / 22); z.high_key = uint8_t(24 + (n + 1) * 85 / 22 - 1);
        z.root = uint8_t(z.low_key + (z.high_key - z.low_key + 1) / 2); z.part = uint16_t(n);
        m.shapes.push_back(z);
    }
    m.part_state.assign(22, PartState::Landed);
    ref.zones = 22;
    s.sample = m;
    s.ui.zone = 9; s.zone_file = "Choir C4"; // C4's zone, A#3..C#4
}

// The same pad holding a felt piano SFZ: A0..C8 in 30 key ranges, three velocity layers, two turns of round robin.
void piano_pad(Snapshot& s, unsigned pad) {
    auto& sound = s.project.tracks[3].pads[pad % sampler_pads];
    auto& ref = s.project.samples[sound.sample % sample_slots];
    std::snprintf(ref.path, sizeof(ref.path), "/media/card/Samples/Piano/Piano.sfz");
    std::snprintf(ref.name, sizeof(ref.name), "PIANO");
    ref.frames = 0;
    sound.play = PadPlay::Gate; sound.start = sound.end = 0;
    LoadedSample m;
    m.name = ref.name; m.path = ref.path; m.instrument = true; m.ready = true;
    static const uint8_t layers[3][2] {{1, 48}, {49, 96}, {97, 127}};
    for (unsigned n = 0; n < 30; ++n)
        for (unsigned v = 0; v < 3; ++v)
            for (unsigned turn = 1; turn <= 2; ++turn) {
                ZoneShape z;
                z.low_key = uint8_t(21 + n * 88 / 30); z.high_key = uint8_t(21 + (n + 1) * 88 / 30 - 1);
                z.root = uint8_t(z.low_key + (z.high_key - z.low_key + 1) / 2);
                z.low_velocity = layers[v][0]; z.high_velocity = layers[v][1]; z.round = uint8_t(turn);
                z.part = uint16_t(m.shapes.size());
                m.shapes.push_back(z);
            }
    m.zones = m.parts = m.parts_ready = unsigned(m.shapes.size());
    m.part_state.assign(m.shapes.size(), PartState::Landed);
    m.low_key = 21; m.high_key = 108; m.layers = 3; m.rounds = 2;
    ref.zones = uint16_t(m.shapes.size());
    s.sample = m;
    s.ui.zone = uint16_t(zone_at(m.shapes, 60, 100)); s.zone_file = "Piano C4 mf rr1";
}

// A voice of the kit's pad A09 heard `into` frames into its region, its envelope `level` (shell/playheads.h, as the
// trail leaves it in the snapshot): the head and its wake.
void playing_head(Snapshot& s, double into, float level = 1) {
    const auto& pad = s.project.tracks[3].pads[8];
    PlayHead h;
    h.serial = 7; h.pad = 8; h.slot = pad.sample; h.at = pad.start + into;
    h.from = pad.start; h.to = pad.end ? pad.end : s.sample.frames; h.loop_at = h.from;
    h.step = 1; h.level = level; h.wake = 1;
    s.heads[0] = h;
}

// A voice of kit pad `pad` (track 4) `through` its region (0..1), its envelope `level`, as heads[k].
void pad_voice(Snapshot& s, unsigned k, unsigned pad, uint32_t serial, float through, float level) {
    const auto& kp = s.project.tracks[3].pads[pad];
    PlayHead h;
    h.serial = serial; h.pad = int16_t(pad); h.slot = kp.sample;
    h.from = kp.start; h.to = kp.end ? kp.end : kp.start + 20000; h.loop_at = h.from;
    h.at = h.from + (h.to - h.from) * double(through);
    h.step = 1; h.level = level; h.wake = 1;
    s.heads[k] = h;
}

// A voice of kit pad A09 (track 4) as heads[k] at frame `at` of the file, its envelope `level` (mocks 2 and 3).
PlayHead& stage_voice(Snapshot& s, unsigned k, uint32_t serial, double at, float level) {
    const auto& pad = s.project.tracks[3].pads[8];
    PlayHead& h = s.heads[k];
    h = PlayHead {};
    h.serial = serial; h.pad = 8; h.slot = pad.sample; h.at = at;
    h.from = pad.start; h.to = pad.end ? pad.end : s.sample.frames; h.loop_at = pad.loop_point();
    h.step = pad.reverse ? -1 : 1; h.level = level; h.wake = 1;
    if (pad.loop) h.flags |= tap::Loop;
    return h;
}

// Chop's audition voice (Chop's pads play through it) on frames `from`..`to` of slot 4, `through` it (0..1), as heads[0].
void chop_voice(Snapshot& s, uint32_t from, uint32_t to, float through, float level) {
    PlayHead h;
    h.serial = 60; h.pad = int16_t(sampler_pads); h.slot = 4;
    h.from = from; h.to = to; h.loop_at = from; h.at = from + double(to - from) * through;
    h.step = 1; h.level = level; h.wake = 1;
    s.heads[0] = h;
}

// Kit pad A09's cloud heard (track 4, mock 6): its voice's head at frame `centre` (where its grains come from now),
// twelve grains round it at their own points in their windows, newest last, and two just gone, their marks fading.
void cloud_heard(Snapshot& s, double centre) {
    const auto& pad = s.project.tracks[3].pads[8];
    PlayHead& h = s.heads[0];
    h = PlayHead {};
    h.serial = 30; h.pad = 8; h.slot = pad.sample; h.at = centre;
    h.from = pad.start; h.to = pad.end ? pad.end : s.sample.frames; h.loop_at = h.from;
    h.level = 0.8f; h.wake = 1; h.flags = tap::Cloud;
    const double length = 0.1 * 44100, spread = 0.05 * double(h.to - h.from);
    for (unsigned k = 0; k < 14; ++k) {
        GrainPlace& g = s.grains[k];
        g = GrainPlace {};
        g.serial = 200 + k; g.pad = 8; g.slot = pad.sample;
        g.from = centre + std::sin(k * 2.3) * spread;
        float through = k < 12 ? 1.0f - (k + 0.5f) / 12.0f : 1.0f; // the newest just born
        g.at = g.from + length * through;
        if (k < 12) g.window = std::sin(3.14159f * through);
        else g.mark = k == 12 ? 0.7f : 0.3f;
    }
}

// The Project board's card: two folders and four projects, Project 1 open with changes.
void preview_projects(Snapshot& s) {
    auto& v = s.projects;
    v.root = "/media/card/Melodee/Projects"; v.free_bytes = 21400000000ull;
    using K = ProjectEntry::Kind;
    v.entries = {{K::Folder, "Beats", v.root + "/Beats", 0, 0, 6, 0}, {K::Folder, "Songs", v.root + "/Songs", 0, 0, 14, 0},
                 {K::Project, "Project 1", v.root + "/Project 1.mldp", 10, 1240, 0, 4}, {K::Project, "Project 2", v.root + "/Project 2.mldp", 7, 920, 0, 3},
                 {K::Project, "Project 3", v.root + "/Project 3.mldp", 3, 1100, 0, 2}, {K::Project, "Project 4", v.root + "/Project 4.mldp", 12, 1400, 0, 1}};
    v.folders = {"Beats", "Beats/Drafts", "Songs", "Songs/2026"};
    s.project_path = v.root + "/Project 1.mldp";
    s.dirty = true;
    s.ui.project_rows = uint16_t(project_rows(v.entries.size())); // as the shell copies it in
}

// Step Edit on the kit (track 4), step entry: the caret on step 5 of pattern 1, which holds a kick, a hat and
// a snare played a little late; the steps before it typed in, the transport stopped.
void step_edit_kit(Shell& shell, Snapshot& s) {
    s.ui.page = PageId::StepEdit; s.ui.track = 3; s.playing = false;
    auto& pattern = shell.patterns().at[3][0];
    pattern = Pattern {};
    pattern.length = 2 * 4 * ticks_per_quarter;
    const struct { unsigned tick; uint8_t pad, velocity; uint16_t length; } hits[] {
        {0, 0, 118, 24}, {48, 2, 70, 12}, {96, 0, 112, 24}, {96, 2, 84, 12}, {103, 1, 104, 36}, {144, 2, 66, 12}, {192, 1, 110, 24},
    };
    for (const auto& h : hits) {
        Event e;
        e.tick = h.tick; e.key = uint8_t(sampler_first_note + h.pad); e.velocity = h.velocity; e.length = h.length;
        pattern.add(e);
    }
    s.project.tracks[3].pattern = 0;
    s.ui.step_cursor = 4; s.ui.step_event = 1;
}

// Roll on track 1: a bass line over bars 2 and 3 (the playable prototype's), a chord at 2.2.3, an accent at 2.3.1,
// A4 over the rows and D3 under them; the cursor on `step` and `key`.
// The place bar's previews: the kit on Pads, playing pattern slot 8, 4 bars long.
void place_scene(Shell& shell, Snapshot& s) {
    s.ui.page = PageId::Pads; s.ui.track = 3;
    s.project.tracks[3].pattern = 7;
    shell.patterns().at[3][7].length = 4 * 16 * sixteenth_ticks;
}

void roll_bass(Shell& shell, Snapshot& s, unsigned step, uint8_t key) {
    s.ui.page = PageId::Roll; s.ui.track = 0;
    auto& pattern = shell.patterns().at[0][s.project.tracks[0].pattern % pattern_count];
    pattern = Pattern {};
    pattern.length = 4 * 4 * ticks_per_quarter;
    const struct { unsigned step, length; uint8_t key, velocity, flags; } notes[] {
        {16, 3, 62, 100, 0}, {18, 1, 50, 80, 0}, {19, 1, 62, 70, 0}, {20, 1, 65, 90, 0}, {22, 2, 60, 96, 0}, {22, 2, 64, 84, 0},
        {24, 4, 57, 112, step_accent}, {25, 1, 69, 88, 0}, {26, 1, 60, 60, 0}, {28, 2, 62, 100, 0}, {30, 4, 55, 90, 0},
        {34, 2, 62, 100, 0}, {38, 2, 60, 90, 0}, {40, 4, 57, 100, 0},
    };
    for (const auto& n : notes) {
        Event e;
        e.tick = n.step * sixteenth_ticks; e.length = uint16_t(n.length * sixteenth_ticks);
        e.key = n.key; e.velocity = n.velocity; e.flags = n.flags;
        pattern.add(e);
    }
    s.ui.step_cursor = int16_t(step); s.ui.roll_key = key; s.ui.roll_view = 16; s.ui.roll_top = 67;
}

// Motion on the kit: pad A09's level and tune recorded over pattern 1, and
// A01's level (not shown: A09 is the last pad hit).
void pad_motion_kit(Shell& shell, Snapshot& s) {
    s.ui.page = PageId::Motion; s.ui.track = 3; s.playing = true; s.position = 9 * sixteenth_ticks; s.ui.recording = true;
    auto& t = s.project.tracks[3];
    t.pattern = 0; t.last_pad = 8;
    auto& p = shell.patterns().at[3][0];
    p.motion.clear();
    auto point = [&](unsigned sixteenth, PadParam which, int value, unsigned pad) {
        MotionPoint m;
        m.tick = sixteenth * sixteenth_ticks; m.param = uint8_t(motion_pad_param(which)); m.value = int16_t(value); m.pad = uint8_t(pad);
        p.write_motion(m);
    };
    const int level[] {100, 84, 62, 50, 70, 96, 110, 120};
    for (unsigned n = 0; n < 8; ++n) point(n * 2, PadParam::Level, level[n], 8);
    const int tune[] {0, 0, 5, 7, -5, 0};
    for (unsigned n = 0; n < 6; ++n) point(2 + n * 2, PadParam::Pitch, tune[n], 8);
    point(4, PadParam::Level, 40, 0);
}

// Knob FX with list entry `entry` loaded, at its initial values (as Context::knob_fx_load does).
void load_knob(KnobFx& k, unsigned entry) {
    const auto& e = knob_fx_entry(entry);
    k.choice = uint8_t(entry); k.kind = uint8_t(e.kind);
    const auto& info = effect_info(e.kind);
    for (unsigned q = 0; q < effect_params; ++q) k.params[q] = info.params[q].label ? info.params[q].initial : 0;
    if (e.param >= 0) k.params[size_t(e.param)] = e.value;
    k.settle();
}

// Knob FX C: A and B of K1-K3 as fractions of each one's range, the fader at `morph`, heard there.
void knob_settings(KnobFx& k, std::array<float, 3> a, std::array<float, 3> b, uint8_t morph) {
    const auto& info = effect_info(EffectKind(k.kind));
    for (unsigned q = 0; q < 3; ++q) {
        const auto& pi = info.params[q];
        k.a[q] = int16_t(pi.min + std::lround(a[q] * (pi.max - pi.min)));
        k.b[q] = int16_t(pi.min + std::lround(b[q] * (pi.max - pi.min)));
    }
    k.morph = morph;
    for (unsigned q = 0; q < 3; ++q) k.params[q] = k.morphed(q);
}

// Stage on `track`, on button `group`'s page `sub` (shell/stage_map.h).
// The Compressor's picture: a kick on each beat (15 columns), its hit, the mix ducking and coming back.
void kick_trail(Snapshot& s) {
    for (unsigned n = 0; n < comp_columns; ++n) {
        unsigned beat = (n + 6) % 15;
        s.comp_trail[n] = {beat < 6 ? -3.0f - 6.0f * float(beat) : -90.0f, beat == 0 ? 4.0f : 8.0f * std::exp(-float(beat - 1) / 3.5f)};
    }
}
// Compressor B: a kick on each beat of a 4/4 bar, the pump after each; this bar played up to `at` (0..1).
void kick_bar(Snapshot& s, float at, float depth) {
    constexpr unsigned per_beat = comp_bar_columns / 4;
    for (unsigned n = 0; n < comp_bar_columns; ++n) {
        unsigned in = n % per_beat;
        float pump = in < 2 ? depth * float(in + 1) / 2 : depth * std::exp(-float(in - 2) / 9.0f);
        s.comp_ghost[n] = {pump * 0.92f, in == 0};
        s.comp_bar[n] = {pump, in == 0};
    }
    s.comp_bar_at = int(at * comp_bar_columns);
}

// Time Correct C: a loose hand over the first two beats, notes off the sixteenths either way.
void loose_hand(Pattern& p) {
    p = Pattern {};
    p.length = 4 * 4 * ticks_per_quarter;
    const unsigned played[] {3, 21, 52, 70, 98, 117, 140, 165};
    for (unsigned n = 0; n < 8; ++n) { Event e; e.tick = played[n]; e.key = uint8_t(48 + n % 3 * 4); p.add(e); }
}

// midi-track A: what a MIDI track sent over the last four seconds, a bass line and a chord, two notes on their way now.
void sent_notes(Snapshot& s, unsigned track) {
    const struct { int ago, held; uint8_t key, velocity; } line[] {
        {3900, 300, 40, 100}, {3450, 250, 43, 80}, {3000, 600, 45, 110}, {2300, 250, 40, 90}, {1900, 700, 52, 70}, {1900, 700, 55, 70},
        {1900, 700, 59, 70}, {1050, 300, 47, 104}, {600, 260, 45, 96}, {260, 0, 40, 120}, {90, 0, 52, 84},
    };
    unsigned n = 0;
    for (const auto& l : line) {
        uint32_t at = s.now - uint32_t(l.ago);
        s.ui.sent_notes[n++] = {uint8_t(track), l.key, l.velocity, at, l.held ? at + uint32_t(l.held) : 0};
    }
    s.ui.sent_next = uint8_t(n);
}

// Roadmap P3: the dust kit's pad A09 with three layers (the break's slice, the snare up two, the hat), layer 2 in hand
// and sounding.
void layered_pad(Snapshot& s, LayerPlay play) {
    auto& pad = s.project.tracks[3].pads[8];
    pad.layer_play = play; pad.low_velocity = 1; pad.high_velocity = 63;
    pad.layers[0].sample = 1; pad.layers[0].low = 64; pad.layers[0].high = 110; pad.layers[0].pitch = 2; pad.layers[0].gain = -300;
    pad.layers[1].sample = 2; pad.layers[1].low = 100; pad.layers[1].high = 127;
    s.ui.pad_layer = 1;
    PlayHead& h = s.heads[0];
    h = PlayHead {};
    h.serial = 41; h.pad = 8; h.slot = 1; h.from = 0; h.to = 26460; h.at = 6000; h.level = 0.8f; h.wake = 1; h.step = 1;
}

void on_stage(Snapshot& s, unsigned track, unsigned group, unsigned sub) {
    s.ui.page = PageId::Stage; s.ui.track = uint8_t(track);
    s.ui.stage_group[track] = uint8_t(group); s.ui.stage_sub[track] = uint8_t(sub);
}
// Freeze plan F2c's previews: track 3 (a plug-in) with patterns P1-P4, P9 and P10, P2 playing; on the PRINT sheet all
// but P10 lit. frozen_scene(): P1-P3 frozen, Post FX, Loop, its print of P2 playing.
void print_scene(Snapshot& s) {
    s.ui.page = PageId::Print; s.ui.track = 2; s.playing = true;
    for (unsigned n : {0u, 1u, 2u, 3u, 8u, 9u}) { s.sketches[2][n].notes = true; s.sketches[2][n].length = 16 * ticks_per_quarter; }
    s.project.tracks[2].pattern = 1; s.playing_patterns[2] = 1; s.positions[2] = int(22 * sixteenth_ticks);
    auto& sheet = s.ui.print_sheet;
    sheet.track = 2;
    for (unsigned n : {0u, 1u, 2u, 3u, 8u}) sheet.pick(n, true);
    s.machine.track_load[2] = 0.22f;
    s.audio_rate = 48000;
}
void frozen_scene(Snapshot& s) {
    print_scene(s);
    s.ui.page = PageId::Stage;
    auto& f = s.freeze[2];
    f.state = FreezeState::Frozen;
    for (unsigned n : {0u, 1u, 2u}) f.set(n, true);
    s.prints_kept[2] = 3;
    s.printing_tracks = 1u << 2;
    s.machine.track_load[2] = 0.004f; s.live_load[2] = 0.22f;
    s.print_peaks_known = true;
    for (unsigned n = 0; n < s.print_peaks.size(); ++n) { // sixteen notes with a short tail each, a loud first of four
        unsigned in_note = n % 6;
        float level = (n / 6) % 4 == 0 ? 0.9f : 0.55f;
        s.print_peaks[n] = uint8_t(255 * level * std::exp(-0.45f * float(in_note)));
    }
}
// Freeze plan F2d: track 3 deep frozen, every pattern with notes printed (P1-P4, P9, P10), its plug-ins let go.
void deep_scene(Snapshot& s) {
    frozen_scene(s);
    auto& f = s.freeze[2];
    f.state = FreezeState::Deep;
    for (unsigned n : {0u, 1u, 2u, 3u, 8u, 9u}) f.set(n, true);
    s.prints_kept[2] = 6;
    s.machine.track_load[2] = 0.002f;
}

// Stage on track 12, a MIDI track to a synth on USB-C, channel 3.
void stage_midi(Snapshot& s, unsigned group, unsigned sub) {
    auto& t = s.project.tracks[11];
    load_sound(t, EngineKind::Midi, 0);
    t.set_name("SYNTH");
    t.params[midi_track::Port] = midi_track::UsbC; t.params[midi_track::Channel] = 3;
    t.params[midi_track::Program] = 12; t.params[midi_track::Cc2] = 20; t.params[midi_track::Cc3] = 0;
    s.ui.midi_outputs = 1 | 2;
    on_stage(s, 11, group, sub); s.playing = true;
}

// The Looper's previews: L1 playing with an overdub (stretched to another tempo), L2 recording a free take,
// L3 stopped with a change waiting, L4 armed; peaks drawn as a beat would.
void looper_scene(Snapshot& s) {
    s.ui.page = PageId::Looper; s.playing = true; s.ui.loop = 0;
    s.project.looper.on = true; s.looper_on = true; s.loop_tick = 384 * 2 + 96 * 2 + 10;
    auto fill = [](Snapshot::LoopShown& l, unsigned seed) {
        for (unsigned c = 0; c < looper::peak_columns; ++c) {
            unsigned beat = c % 32;
            float env = beat < 3 ? 1.0f : std::exp(-float(beat) / (8.0f + seed));
            l.peaks[c] = uint8_t(std::min(255.0f, 40 + 200 * env * (0.6f + 0.4f * std::fabs(std::sin(c * 0.37f + seed)))));
        }
    };
    auto& a = s.loops[0].view; a.state = Looper::State::Overdub; a.length = 176400; a.position = 70000; a.span = a.length; a.ticks = 4 * 384;
    a.tempo = 1200; a.layers = 3; a.undo = looper::Undo::Undo; a.ready = true; a.stretched = true; a.source = looper::source_input; fill(s.loops[0], 1);
    s.loops[0].meter = 0.5f;
    auto& b = s.loops[1].view; b.state = Looper::State::Recording; b.length = 0; b.position = 60000; b.span = 176400; b.ready = true; b.layers = 1;
    b.source = looper::source_track(2); fill(s.loops[1], 3);
    auto& c = s.loops[2].view; c.state = Looper::State::Stopped; c.pending = Looper::Pending::Play; c.length = 44100; c.position = 12000; c.span = c.length;
    c.ticks = 384; c.tempo = 1200; c.layers = 1; c.ready = true; c.source = looper::source_mix; fill(s.loops[2], 5);
    auto& d = s.loops[3].view; d.state = Looper::State::Armed; d.bars = 2; d.ready = true; d.source = looper::source_loop(0);
    s.project.looper.loops[3].bars = 2; s.project.looper.loops[1].source = looper::source_track(2);
    s.project.loop_mix[0].level = 110; s.project.loop_mix[0].send = 30; s.project.loop_mix[1].pan = -20; s.project.loop_mix[2].mute = true;
    s.project.loop_mix[3].solo = true; s.project.loop_mix[3].bus = 2; s.project.loop_mix[3].send = 55;
}

// The loop's sheet (B3 MORE) on loop `l`, no change waiting, with the kit (track 4) in hand: COMMIT lands on B01.
void looper_sheet(Snapshot& s, unsigned l) {
    looper_scene(s);
    s.ui.loop_sheet = true; s.ui.loop = uint8_t(l); s.ui.track = 3;
    s.loops[l].view.pending = Looper::Pending::None;
    s.project.looper.loops[2].source = looper::source_mix; s.project.looper.loops[3].source = looper::source_loop(0); // as their views say
}

// Plug-ins (docs/MELODEE.md): a Plugins folder as the picker lists it, and a PLUGIN track's panel as the rack fills it.
void plugin_listing(Snapshot& s) {
    auto& v = s.plugins;
    v.root = "/media/card/Melodee/Plugins"; v.folder = v.root; v.top = false; v.home = true; // above it: Melodee, then the card's top
    auto add = [&](const char* name, bool folder, PluginFormat format, int8_t instrument, const char* why = "") {
        PluginFile f;
        f.name = name; f.folder = folder; f.format = format; f.instrument = instrument; f.why = why;
        f.path = v.root + "/" + name;
        v.files.push_back(f);
    };
    add("Open MPC", true, PluginFormat::Vst3, -1);
    add("Reverbs", true, PluginFormat::Vst3, -1);
    add("Bass Synth", false, PluginFormat::Vst2Arm32, 1);
    add("Melodee Poly", false, PluginFormat::Vst2, 1);
    add("Drum Machine", false, PluginFormat::Vst2, 1);
    add("Plate Reverb", false, PluginFormat::Vst3, 0);
    add("Desktop Synth", false, PluginFormat::Vst3, -1, "Windows and Mac builds only");
    add("Melodee PD", false, PluginFormat::Vst2, -1);
    add("Effect Pack", false, PluginFormat::Vst3, 0);
    v.files.back().plugins = 214; // a bundle of many: a row that opens
}

void plugin_panel(Snapshot& s, unsigned entry, const char* name, const char* format, bool instrument, PluginStatus status) {
    auto& panel = s.ui.plugin_panel;
    panel = {};
    panel.entry = uint8_t(entry); panel.status = status; panel.instrument = instrument;
    std::snprintf(panel.name, sizeof(panel.name), "%s", name);
    std::snprintf(panel.format, sizeof(panel.format), "%s", format);
    if (status == PluginStatus::Failed) std::snprintf(panel.error, sizeof(panel.error), "libGL.so.1: cannot open shared object file");
    if (status != PluginStatus::Ready) return;
    panel.rows = 8; panel.row = 1; panel.load = 216; panel.programs = 12;
    const char* names[] {"Cutoff", "Filter envelope amount", "Decay time"};
    const char* texts[] {"5.0", "7.2", "3.5"};
    const int16_t values[] {500, 720, 350};
    for (unsigned k = 0; k < 3; ++k) {
        auto& c = panel.params[k];
        c.used = true; c.value = values[k];
        std::snprintf(c.name, sizeof(c.name), "%s", names[k]);
        std::snprintf(c.text, sizeof(c.text), "%s", texts[k]);
    }
    // The list's window: the row before (Waveform, Tuning, Volume), then the three in hand.
    const char* before[] {"Waveform", "Tuning", "Volume"};
    const char* before_texts[] {"0.42", "3.1", "0.8"};
    panel.window_first = 0;
    for (unsigned k = 0; k < 6; ++k) {
        auto& c = panel.window[k];
        c.used = true;
        if (k < 3) { std::snprintf(c.name, sizeof(c.name), "%s", before[k]); std::snprintf(c.text, sizeof(c.text), "%s", before_texts[k]); }
        else c = panel.params[k - 3];
    }
    s.ui.plugin_row[entry] = 1;
}

void plugin_track(Snapshot& s, PluginStatus status) {
    s.ui.page = PageId::Edit; s.ui.track = 2;
    auto& t = s.project.tracks[2];
    t.engine = EngineKind::Plugin; t.plugin = 0; t.set_name("BASS SYNTH");
    std::snprintf(s.project.plugins[0].path, plugin_path_length, "/media/card/Melodee/Plugins/Bass Synth");
    plugin_panel(s, 0, "Bass Synth", "ARMV7", true, status);
}

// The same by the kind of page, wherever the track's Stage keeps it (shell/stage_map.h); a PLUGIN track's plug-in
// open, as the MPC has it on every Stage page (B1 then says PRESETS).
// Track 4's kit in colours, as an MPC kit brings them (kick red, snare blue, hats green) and Stage's COLOUR picks them
// (breaks 1-4 Violet, A09 in hand Teal); breaks 6-12 stay in the track's colour.
void coloured_kit(Snapshot& s) {
    auto& pads = s.project.tracks[3].pads;
    pads[0].colour = full_colour(0x7F0000); pads[1].colour = 0x0000FF; pads[2].colour = pads[3].colour = 0x00FF00;
    for (unsigned n = 4; n < 8; ++n) pads[n].colour = ui::track_shades[7][0];
    pads[8].colour = ui::track_shades[2][0];
}
void on_stage(Snapshot& s, unsigned track, StageKind kind) {
    const auto& t = s.project.tracks[track];
    auto f = stage_find(t.engine, kind);
    on_stage(s, track, f.at.group, f.at.sub);
    if (t.engine == EngineKind::Plugin) plugin_panel(s, t.plugin, t.plugin == 0 ? "Melodee Poly" : "Plug-in", "VST3", true, PluginStatus::Ready);
}
// Stage B1 on track 1 (Melodee Poly, plug-in entry 0): its presets, `programs` of them (0: its face instead).
void stage_plugin(Snapshot& s, StageKind kind, bool presets) {
    on_stage(s, 0, kind); s.playing = true;
    plugin_panel(s, 0, "Melodee Poly", "VST3", true, PluginStatus::Ready);
    if (!presets) { s.ui.plugin_panel.programs = 0; return; }
    static const char* const names[] {"Init Voice", "Funk Bass II", "Rubber Bass", "Sync Lead", "Brass Stab", "Soft Pad", "Glass Pluck",
                                      "Warm Keys", "Sub Bass", "String Machine", "Poly Sweep", "Unison Lead", "Noise Hat"};
    auto& v = s.presets;
    v.entry = 0;
    std::vector<PluginProgram> list;
    for (int i = 0; i < 13; ++i) list.push_back({i, names[i], {}});
    v.list = PluginProgramList(std::move(list));
    v.current = 2;
    s.ui.plugin_panel.programs = 13;
}

// PRESETS for Surge XT, a CLAP plug-in whose presets are files in folders (plugin/preset_rows.h): Factory Presets'
// folders as Surge XT ships them, with their counts, the first Leads by name, and Third Party Presets beside it.
void surge_presets(Snapshot& s) {
    s.ui.page = PageId::PluginPresets; s.ui.track = 0; s.ui.presets_back = PageId::Edit; s.ui.presets_entry = 0;
    auto& t = s.project.tracks[0];
    t.engine = EngineKind::Plugin; t.plugin = 0; t.set_name("SURGE XT");
    std::snprintf(s.project.plugins[0].path, plugin_path_length, "/media/card/Melodee/Plugins/Surge XT/Surge XT.clap");
    plugin_panel(s, 0, "Surge XT", "CLAP", true, PluginStatus::Ready);
    static const std::pair<const char*, int> folders[] {{"Basses", 59}, {"Brass", 11}, {"Chords", 8}, {"FX", 27}, {"Keys", 16},
        {"Leads", 131}, {"MPE", 9}, {"Pads", 65}, {"Percussion", 9}, {"Plucks", 111}, {"Polysynths", 104}, {"Sequences", 45},
        {"Splits", 5}, {"Templates", 16}, {"Tutorials", 14}, {"Vocoder", 3}, {"Winds", 8}};
    static const char* const leads[] {"Acidofil", "Agroculture", "Asymptote", "Bad Childhood", "Banjo Remains", "Banter", "Bee", "Bitten"};
    std::vector<PluginProgram> list;
    for (auto [folder, count] : folders)
        for (int n = 0; n < count; ++n) {
            bool lead = std::string(folder) == "Leads" && n < 8;
            list.push_back({int(list.size()), lead ? leads[n] : std::string(folder) + " " + std::to_string(n + 1),
                            std::string("Factory Presets/") + folder});
            if (lead && n == 4) s.presets.current = list.back().index; // Banjo Remains loaded
        }
    for (int n = 0; n < 2922; ++n) list.push_back({int(list.size()), "Patch " + std::to_string(n + 1), "Third Party Presets"});
    s.ui.plugin_panel.programs = uint16_t(list.size());
    s.presets.entry = 0;
    s.presets.list = PluginProgramList(std::move(list));
}

struct Preview {
    const char* name;
    void (*set)(Shell&, Snapshot&);
};

const Preview previews[] {
    {"stage", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.playing = true; }}, // track 1, a synth plug-in: B1 MIX
    // The place bar (shell/place_bar.h) on the kit's Pads, its pattern slot 8 made 4 bars long (no other preview
    // plays it): playing, at the top; recording, at the bottom; stopped, held where Shift + Play carries on.
    {"placebar", [](Shell& shell, Snapshot& s) {
         place_scene(shell, s); s.playing = true; s.position = 22 * sixteenth_ticks + 2;
     }},
    {"placebarbottom", [](Shell& shell, Snapshot& s) {
         place_scene(shell, s); s.playing = true; s.ui.recording = true; s.ui.settings.place_bottom = true; s.position = 53 * sixteenth_ticks;
     }},
    {"placebarstopped", [](Shell& shell, Snapshot& s) {
         place_scene(shell, s); s.ui.locate = 37 * sixteenth_ticks;
     }},
    // Stage, context aware: each track type's pages (pages/stage_page.cpp).
    // Sample held a moment (the SOUND hub, docs/HUBS_PLAN.md H-b): the track's pads over the page; on Steps the editor stays.
    {"sampleheldkit", [](Shell&, Snapshot& s) { // over the kit's Stage TRIM, A09 the pad in hand
         on_stage(s, 3, StageKind::Trim); s.sample = break_sample(); s.playing = true; pad_waves(s);
         s.ui.sample_held = true; s.ui.sample_down = s.now - 400;
     }},
    {"sampleheldkeys", [](Shell&, Snapshot& s) { // over a plug-in's MIX: its Keys
         on_stage(s, 0, StageKind::Mix); s.playing = true;
         s.ui.sample_held = true; s.ui.sample_down = s.now - 400;
     }},
    {"sampleheldsteps", [](Shell&, Snapshot& s) { // on Step Edit: the steps stay, the header names the held notes
         s.ui.page = PageId::StepEdit; s.ui.step_pads = true; s.ui.step_cursor = 6; s.playing = true; s.position = 9 * sixteenth_ticks;
         s.ui.sample_held = true; s.ui.sample_down = s.now - 400;
         s.held_keys.fill(-1); s.held_keys[0] = 48; s.held_keys[2] = 52; s.held_keys[4] = 55; s.ui.held_pads = 1u | 4u | 16u;
     }},
    {"stagepluginpresets", [](Shell&, Snapshot& s) { stage_plugin(s, StageKind::PluginSound, true); }}, // B1: Melodee Poly's presets
    {"stagepluginface", [](Shell&, Snapshot& s) { stage_plugin(s, StageKind::PluginSound, false); }}, // B1, a plug-in with none
    {"stagepluginedit", [](Shell&, Snapshot& s) { stage_plugin(s, StageKind::PluginEdit, true); }},   // B1 again: its parameters
    {"stagemix", [](Shell&, Snapshot& s) { on_stage(s, 0, StageKind::Mix); s.playing = true; }},
    {"stagesends", [](Shell&, Snapshot& s) { on_stage(s, 0, StageKind::Sends); s.playing = true; s.ui.turning = 1; s.ui.turned_at = s.now; }}, // the four sends
    {"stagearp", [](Shell&, Snapshot& s) { on_stage(s, 0, 1, 0); s.playing = true; s.project.tracks[0].arp_octaves = 2; }},
    {"stagearpheld", [](Shell&, Snapshot& s) { // keys held: the notes it plays from them, up and down
         on_stage(s, 0, 1, 0); s.playing = true; auto& t = s.project.tracks[0]; t.arp = true; t.arp_mode = 2;
         s.held_keys.fill(-1); s.held_keys[0] = 50; s.held_keys[4] = 53; s.held_keys[9] = 57;
     }},
    {"stagechord", [](Shell&, Snapshot& s) { on_stage(s, 0, 1, 1); s.playing = true; auto& t = s.project.tracks[0]; t.chord = true; t.chord_type = 1; s.ui.chord_pad = 1; }},
    {"stagefx", [](Shell&, Snapshot& s) { on_stage(s, 2, StageKind::Fx); s.playing = true; s.ui.fx_cursor[2] = 1; }},
    {"stagefxempty", [](Shell&, Snapshot& s) { on_stage(s, 0, StageKind::Fx); s.playing = true; }},
    {"stagekitlayers", [](Shell&, Snapshot& s) { on_stage(s, 3, 0, 1); s.sample = break_sample(); s.playing = true; layered_pad(s, LayerPlay::Velocity); }},
    {"stagekitlayersshift", [](Shell&, Snapshot& s) { on_stage(s, 3, 0, 1); s.sample = break_sample(); layered_pad(s, LayerPlay::Random); s.ui.shift = true; }},
    {"stagekitmix", [](Shell&, Snapshot& s) { on_stage(s, 3, StageKind::PadMix); s.sample = break_sample(); s.playing = true; s.project.tracks[3].pads[8].pan = 12; }},
    {"stagekitsends", [](Shell&, Snapshot& s) { // a kit's SENDS: the last pad's own, 30 % into bus A
         on_stage(s, 3, StageKind::Sends); s.sample = break_sample(); s.playing = true;
         s.project.tracks[3].pads[8].sends = {30, 0, 10, 0}; s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"stagekitsendsdry", [](Shell&, Snapshot& s) { on_stage(s, 3, StageKind::Sends); s.sample = break_sample(); s.playing = true; }}, // a dry pad
    {"stagekitsendsshift", [](Shell&, Snapshot& s) { // Shift held: the whole kit's (KIT SEND)
         on_stage(s, 3, StageKind::Sends); s.sample = break_sample(); s.playing = true;
         s.project.tracks[3].pads[8].sends = {30, 0, 0, 0}; s.project.tracks[3].mix.sends = {20, 0, 0, 0}; s.ui.shift = true;
     }},
    {"stagekitampenv", [](Shell&, Snapshot& s) {
         on_stage(s, 3, StageKind::AmpEnv); s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.attack = 20; pad.decay = 70; pad.velocity_sense = 60;
     }},
    {"stagekittune", [](Shell&, Snapshot& s) { on_stage(s, 3, StageKind::Tune); s.sample = break_sample(); s.playing = true; s.project.tracks[3].pads[8].pitch = -2; }},
    {"stagekitplay", [](Shell&, Snapshot& s) { on_stage(s, 3, StageKind::Play); s.sample = break_sample(); s.playing = true; s.project.tracks[3].pads[8].group = 2; }},
    {"stagekitcolour", [](Shell&, Snapshot& s) { on_stage(s, 3, StageKind::Colour); s.sample = break_sample(); s.playing = true; coloured_kit(s); }},
    {"stagekitfollow", [](Shell&, Snapshot& s) { on_stage(s, 3, StageKind::Follow); s.sample = break_sample(); s.playing = true; s.project.tracks[3].params[sampler::Follow] = 1; }},
    {"stagekitcloud", [](Shell&, Snapshot& s) { on_stage(s, 3, StageKind::Cloud); s.sample = break_sample(); s.playing = true; auto& t = s.project.tracks[3]; t.pads[t.last_pad % sampler_pads].cloud = true; }},
    {"stagekitcloudoff", [](Shell&, Snapshot& s) { on_stage(s, 3, StageKind::Cloud); s.sample = break_sample(); s.playing = true; }},
    {"stagecloudplaying", [](Shell&, Snapshot& s) { // CLOUD while A09 is held: its grains, the bars they cover lit, where they are
         on_stage(s, 3, StageKind::Cloud); s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3];
         auto& pad = t.pads[8]; pad.start = 0; pad.end = 0; pad.cloud = true; pad.cloud_position = 38; // the whole break
         uint32_t end = pad.end ? pad.end : s.sample.frames;
         cloud_heard(s, pad.start + (end - pad.start) * 0.38);
     }},
    {"stagecloudscan", [](Shell&, Snapshot& s) { // Scan moving: the cloud has travelled on from K1's line, the label with it
         on_stage(s, 3, StageKind::Cloud); s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3];
         auto& pad = t.pads[8]; pad.start = 0; pad.end = 0; pad.cloud = true; pad.cloud_position = 20; pad.cloud_scan = 75; // the whole break
         uint32_t end = pad.end ? pad.end : s.sample.frames;
         cloud_heard(s, pad.start + (end - pad.start) * 0.52);
     }},
    {"stagekitfilt", [](Shell&, Snapshot& s) { on_stage(s, 3, StageKind::KitFilter); s.sample = break_sample(); s.playing = true; s.project.tracks[3].params[sampler::Filter] = 55; }},
    {"stagekitfilter", [](Shell&, Snapshot& s) {
         on_stage(s, 3, StageKind::Filter); s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.filter = PadFilter::Lpf4; pad.cutoff = 45; pad.resonance = 40;
     }},
    {"stagekitfiltenv", [](Shell&, Snapshot& s) {
         on_stage(s, 3, StageKind::FiltEnv); s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.filter = PadFilter::Classic; pad.filter_attack = 30; pad.filter_decay = 70; pad.filter_amount = 60;
     }},
    {"stagekitempty", [](Shell&, Snapshot& s) { on_stage(s, 3, 0, 0); s.project.tracks[3].last_pad = 20; s.playing = true; }}, // B05: no sample
    {"stagemidi", [](Shell&, Snapshot& s) { stage_midi(s, 0, 0); s.ui.track_out_at[11] = s.now; sent_notes(s, 11); }},
    {"stagemidiprogram", [](Shell&, Snapshot& s) { stage_midi(s, 0, 1); s.ui.turning = 2; s.ui.turned_at = s.now; }},
    {"stagemidicc", [](Shell&, Snapshot& s) { stage_midi(s, 0, 2); }},
    {"stagemidiccshift", [](Shell&, Snapshot& s) { stage_midi(s, 0, 2); s.ui.shift = true; }},
    {"stagemidiarp", [](Shell&, Snapshot& s) { stage_midi(s, 1, 0); }},
    {"stageemptysynth", [](Shell&, Snapshot& s) { on_stage(s, 12, 0, 0); s.ui.new_pick = 0; }},
    {"stageemptykit", [](Shell&, Snapshot& s) { on_stage(s, 12, 1, 0); s.ui.new_pick = 2; s.ui.new_for = 12; }}, // picked from Tracks: B1 BACK
    {"stageemptymidi", [](Shell&, Snapshot& s) { on_stage(s, 12, 2, 0); s.ui.new_pick = 5; }},
    {"stageemptyslide", [](Shell&, Snapshot& s) { // the wheel just moved: Card on its way to the middle
         on_stage(s, 12, 1, 0); s.ui.new_pick = 2; s.ui.new_pick_from = 1; s.now = 1000; s.ui.new_pick_at = 960;
     }},
    {"tracks", [](Shell&, Snapshot& s) { // Pad Bank tapped twice; each track's share of a block (roadmap P2), one heavy
        s.ui.page = PageId::Tracks; s.ui.track = 2; s.playing = true;
        const float loads[] {0.04f, 0.11f, 0.07f, 0.02f, 0.66f, 0.09f, 0.03f, 0.05f};
        for (unsigned t = 0; t < 8; ++t) s.machine.track_load[t] = loads[t];
    }},
    // The Looper (docs/LOOPER.md): four loops in every state, the loop tracks' MIX, the loop's sheet, off, a loop's chain.
    {"looper", [](Shell&, Snapshot& s) { looper_scene(s); s.ui.turning = 1; s.ui.turned_at = s.now; }},
    {"loopermix", [](Shell&, Snapshot& s) { looper_scene(s); s.ui.looper_view = 1; s.ui.loop = 2; }},
    // B3 MORE, the loop's sheet: L3 stopped with the kit in hand (PLAY on the bar, COMMIT to B01, K1 its source);
    // Shift held (CLEAR, + NOTE, PLAY at once); L1 overdubbing with a synth in hand (STOP, COMMIT dim); L3's play
    // queued (CANCEL); an empty loop (only its cards).
    {"loopersheet", [](Shell&, Snapshot& s) { looper_sheet(s, 2); }},
    {"loopersheetshift", [](Shell&, Snapshot& s) { looper_sheet(s, 2); s.ui.shift = true; }},
    {"loopersheetdub", [](Shell&, Snapshot& s) { looper_sheet(s, 0); s.ui.track = 0; }},
    {"loopersheetqueued", [](Shell&, Snapshot& s) { looper_sheet(s, 2); s.loops[2].view.pending = Looper::Pending::Play; }},
    {"loopersheetempty", [](Shell&, Snapshot& s) {
         looper_sheet(s, 1);
         s.loops[1].view = Looper::View {}; s.loops[1].view.ready = true;
     }},
    {"loopershift", [](Shell&, Snapshot& s) { looper_scene(s); s.ui.shift = true; }},
    {"looperoff", [](Shell&, Snapshot& s) { s.ui.page = PageId::Looper; }},
    {"looperempty", [](Shell&, Snapshot& s) { // just turned on: one loop playing, three empty
         looper_scene(s);
         for (unsigned n = 1; n < loop_tracks; ++n) { s.loops[n].view = Looper::View {}; s.loops[n].view.ready = true; }
     }},
    {"looperfx", [](Shell&, Snapshot& s) {
         looper_scene(s); s.ui.page = PageId::Rack; s.ui.fx_loop = 1;
         s.project.loop_fx[1].slots[0].kind = uint8_t(EffectKind::Echo);
     }},
    {"tracks2", [](Shell&, Snapshot& s) { s.ui.page = PageId::Tracks; s.ui.track = 9; s.playing = true; s.project.tracks[2].mix.mute = true; }},
    {"keys", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Keys; s.ui.held_pads = 1u << 4 | 1u << 9; s.playing = true;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"keysdrums", [](Shell&, Snapshot& s) { // Keys on Drums pads: a drum plug-in's eleven voices (9W9's), Low Tom last, Snare held
         s.ui.page = PageId::Keys; s.playing = true;
         auto& t = s.project.tracks[0];
         use_drum_pads(t); t.last_note = 38;
         const char* voices[] {"Bass Drum", "Snare", "Low Tom", "Med Tom", "Hi Tom", "Rim Shot", "Hand Clap", "Closed Hat", "Open Hat", "Crash", "Ride"};
         s.ui.drum_voices.entry = t.plugin;
         for (unsigned k = 0; k < 11; ++k) std::snprintf(s.ui.drum_voices.names[k].data(), s.ui.drum_voices.names[k].size(), "%s", voices[k]);
         s.ui.held_pads = 1u << 1;
     }},
    {"transpose", [](Shell&, Snapshot& s) { // Keys, Shift + K3 turning: Transpose on the Shift card
         s.ui.page = PageId::Keys; s.ui.shift = true; s.ui.shift_used = true; s.project.tracks[0].transpose = -3; s.playing = true;
         s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    {"sounds", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Sounds; s.ui.track = 2; s.ui.sounds_cursor = 2; s.playing = true;
         auto engine = unsigned(s.project.tracks[2].engine);
         s.ui.settings.toggle_favourite(engine, 1); s.ui.settings.toggle_favourite(engine, 4);
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"edit", [](Shell&, Snapshot& s) { s.ui.page = PageId::Edit; s.playing = true; }}, // a synth plug-in's own values
    {"stepeditsteps", [](Shell&, Snapshot& s) { // Step Edit's pads on the steps (Shift + B1 PADS), step 7 held: its locks
         s.ui.page = PageId::StepEdit; s.ui.step_pads = true; s.ui.step_cursor = 6; s.ui.held_step = 6; s.playing = true; s.position = 9 * sixteenth_ticks;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"motion", [](Shell&, Snapshot& s) { // the kit's FILTER and RESO over its pattern 2
         s.ui.page = PageId::Motion; s.ui.track = 3; s.project.tracks[3].pattern = 1; s.playing = true;
         s.position = 40 * sixteenth_ticks; s.ui.recording = true; s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"patterns", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Patterns; s.ui.track = 2; s.playing = true; // the pads play track 3; B1-B3 the editors
         const uint8_t live[] {1, 0, 0, 2, 1, 0, 0, 0};
         for (unsigned t = 0; t < 8; ++t) { s.playing_patterns[t] = live[t]; s.project.tracks[t].pattern = live[t]; s.positions[t] = int((4 + t) * sixteenth_ticks); }
         s.project.tracks[2].pattern = 1;
     }},
    {"plugins", [](Shell&, Snapshot& s) { // the picker for track 3's instrument: folders, plug-ins, one for Windows only
         s.ui.page = PageId::Plugins; s.ui.track = 2; s.ui.plugin_target = UiState::PluginTarget::Instrument;
         plugin_listing(s); s.ui.plugins_cursor = 3;
     }},
    {"pluginsbundle", [](Shell&, Snapshot& s) { // a bundle opened for track 1's third slot: a row a plug-in
         s.ui.page = PageId::Plugins; s.ui.plugin_target = UiState::PluginTarget::Effect; s.ui.plugin_chain = 0; s.ui.plugin_slot = 2;
         auto& v = s.plugins;
         v.root = "/media/card/Melodee/Plugins"; v.folder = v.root + "/Effects/Effect Pack.vst3"; v.top = false; v.home = false;
         const char* names[] {"Compressor Mono", "Compressor Stereo", "Delay Stereo", "Equalizer x16 Stereo",
                              "Gate Stereo", "Limiter Stereo", "Reverb Stereo", "Trigger Stereo"};
         int32_t id = 1;
         for (const char* n : names) {
             PluginFile f; f.name = n; f.path = v.folder; f.format = PluginFormat::Vst3; f.instrument = 0; f.plugins = 214; f.id = id++;
             v.files.push_back(f);
         }
         s.ui.plugins_cursor = 2;
     }},
    {"pluginsfx", [](Shell&, Snapshot& s) { // the picker for the master's second slot
         s.ui.page = PageId::Plugins; s.ui.plugin_target = UiState::PluginTarget::Effect; s.ui.plugin_chain = uint8_t(master_chain); s.ui.plugin_slot = 1;
         plugin_listing(s); s.ui.plugins_cursor = 5;
     }},
    {"pluginedit", [](Shell&, Snapshot& s) { // Edit on a PLUGIN track: row 2 of 8 on K1-K3, K2 turning
         plugin_track(s, PluginStatus::Ready); s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"pluginopening", [](Shell&, Snapshot& s) { plugin_track(s, PluginStatus::Loading); }},
    {"pluginfailed", [](Shell&, Snapshot& s) { plugin_track(s, PluginStatus::Failed); }},
    {"rackplugin", [](Shell&, Snapshot& s) { // a plug-in effect on track 1's chain, after an echo
         s.ui.page = PageId::Rack; s.ui.track = 0;
         s.project.tracks[0].fx.slots[0] = {uint8_t(EffectKind::Echo), false, {60, 40, 30, 60, 0, 0}};
         s.project.tracks[0].fx.slots[1] = {plugin_effect_kind, false, {4, 0, 0, 0, 0, 0}};
         std::snprintf(s.project.plugins[4].path, plugin_path_length, "/media/card/Melodee/Plugins/Plate Reverb.vst3");
         s.ui.fx_cursor[0] = 1;
         plugin_panel(s, 4, "Plate Reverb", "VST3", false, PluginStatus::Ready);
     }},
    {"rackpluginsheet", [](Shell&, Snapshot& s) { // Push on that plug-in: its sheet, with Presets
         s.ui.page = PageId::Rack; s.ui.track = 0;
         s.project.tracks[0].fx.slots[0] = {uint8_t(EffectKind::Echo), false, {60, 40, 30, 60, 0, 0}};
         s.project.tracks[0].fx.slots[1] = {plugin_effect_kind, false, {4, 0, 0, 0, 0, 0}};
         std::snprintf(s.project.plugins[4].path, plugin_path_length, "/media/card/Melodee/Plugins/Amp Sim.vst3");
         s.ui.fx_cursor[0] = 1;
         plugin_panel(s, 4, "Amp Sim", "VST3", false, PluginStatus::Ready);
         s.ui.plugin_panel.programs = 8;
         s.ui.sheet_open = true; s.ui.sheet_row = 1;
     }},
    {"pluginpresets", [](Shell&, Snapshot& s) { // PRESETS for an amp sim on track 1's chain: the eight factory ones, one loaded
         s.ui.page = PageId::PluginPresets; s.ui.track = 0; s.ui.presets_back = PageId::Rack; s.ui.presets_entry = 4;
         std::snprintf(s.project.plugins[4].path, plugin_path_length, "/media/card/Melodee/Plugins/Amp Sim.vst3");
         plugin_panel(s, 4, "Amp Sim", "VST3", false, PluginStatus::Ready);
         auto& v = s.presets;
         v.entry = 4;
         const char* names[] {"Bass Drive", "Clean", "Clean Chorus", "Crunch",
                              "Crunch Verb", "Fuzz", "High Gain", "Lead"};
         std::vector<PluginProgram> list;
         for (int i = 0; i < 8; ++i) list.push_back({i, names[i], {}});
         v.list = PluginProgramList(std::move(list));
         v.current = 3; s.ui.presets_cursor = 4;
     }},
    {"pluginpresetfolders", [](Shell&, Snapshot& s) { // PRESETS for Surge XT (CLAP): its two locations as folders
         surge_presets(s);
     }},
    {"pluginpresetfolder", [](Shell&, Snapshot& s) { // inside Factory Presets/Leads, Banjo Remains loaded
         surge_presets(s);
         std::snprintf(s.ui.presets_folder.data(), s.ui.presets_folder.size(), "Factory Presets/Leads");
         s.ui.presets_cursor = 3;
     }},
    {"pickerplugins", [](Shell&, Snapshot& s) { // the effect picker on the Plug-ins family
         s.ui.page = PageId::Rack; s.ui.track = 0; s.ui.picker_open = true; s.ui.picker_family = uint8_t(Family::Plugins); s.ui.picker_items = true;
     }},
    {"recquantize", [](Shell&, Snapshot& s) { // Shift + Pad 10: on, a grid of its own, 1/32T, K1 turning
         s.ui.page = PageId::RecQuantize; s.ui.track = 0; s.playing = true;
         s.ui.settings.rec_quantize = true; s.ui.settings.rec_grid = 1 + unsigned(Division::ThirtySecondTriplet);
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"recquantizeoff", [](Shell&, Snapshot& s) { // off, following the pattern's Q
         s.ui.page = PageId::RecQuantize; s.ui.track = 3; s.ui.settings.rec_quantize = false; s.ui.settings.rec_grid = 0;
     }},
    {"seqsheet", [](Shell&, Snapshot& s) { // Seq held over Stage: track 3 plays P2, P4 waits for the bar
         s.ui.page = PageId::Stage; s.ui.track = 2; s.playing = true;
         const uint8_t live[] {1, 0, 0, 2, 1, 0, 0, 0};
         for (unsigned t = 0; t < 8; ++t) { s.playing_patterns[t] = live[t]; s.project.tracks[t].pattern = live[t]; s.positions[t] = int((4 + t) * sixteenth_ticks); }
         s.project.tracks[2].pattern = 3;
         s.ui.seq_held = true; s.ui.seq_down = s.now - seq_sheet_ms; // held a moment: the sheet is up
     }},
    // Freeze plan F2c: the PRINT sheet on track 3 (a plug-in), its prints being made, a MIDI track's sheet, Seq held +
    // Sample Record, Tracks and Stage with track 3 frozen, and the question an edit of it asks.
    {"print", [](Shell&, Snapshot& s) { print_scene(s); }},
    {"printing", [](Shell&, Snapshot& s) {
         print_scene(s);
         auto& sheet = s.ui.print_sheet;
         sheet.printing = true;
         auto& job = s.print;
         job.busy = true; job.freeze = true; job.track = 2; job.count = 5; job.through = 3; job.pattern = 3; job.fraction = 0.62f;
         job.times_real = 5.8f; job.seconds_left = 9;
         for (unsigned n : {0u, 1u, 2u}) job.done[0] |= uint64_t(1) << n;
     }},
    {"printmidi", [](Shell&, Snapshot& s) {
         stage_midi(s, 0, 0);
         for (unsigned n : {0u, 1u, 2u, 3u}) { s.sketches[11][n].notes = true; s.sketches[11][n].length = 64 * sixteenth_ticks; }
         s.ui.page = PageId::Print;
         auto& sheet = s.ui.print_sheet;
         sheet.track = 11; sheet.tail = PrintTail::Ring;
         for (unsigned n : {0u, 1u, 2u, 3u}) sheet.pick(n, true);
     }},
    {"seqprint", [](Shell&, Snapshot& s) { // Seq held + Sample Record: P1 and P3 frozen, P2 playing, P4 and P9 live
         frozen_scene(s);
         s.ui.page = PageId::Stage;
         s.ui.seq_held = true; s.ui.seq_down = s.now - seq_sheet_ms; s.ui.seq_print = true;
         s.ui.seq_frozen = {};
         for (unsigned n : {0u, 2u}) s.ui.seq_frozen[0] |= uint64_t(1) << n;
     }},
    {"seqfrozen", [](Shell&, Snapshot& s) { // Seq held on a frozen track: its frozen patterns marked
         frozen_scene(s);
         s.ui.page = PageId::Stage;
         s.ui.seq_held = true; s.ui.seq_down = s.now - seq_sheet_ms;
     }},
    {"tracksfrozen", [](Shell&, Snapshot& s) {
         frozen_scene(s);
         s.ui.page = PageId::Tracks;
         const float loads[] {0.04f, 0.11f, 0.004f, 0.02f, 0.16f, 0.09f, 0.03f, 0.05f};
         for (unsigned t = 0; t < 8; ++t) s.machine.track_load[t] = loads[t];
         s.machine.render_load = 0.41f;
     }},
    {"stagefrozen", [](Shell&, Snapshot& s) { frozen_scene(s); on_stage(s, 2, 1, 0); }}, // the print of P2 playing; the sound's cards dim
    {"stagefrozenshift", [](Shell&, Snapshot& s) { frozen_scene(s); on_stage(s, 2, 1, 0); s.ui.shift = true; }}, // Shift + B3: DEEP FREEZE
    {"askunfreeze", [](Shell&, Snapshot& s) { // a sound card turned on a frozen track
         frozen_scene(s); on_stage(s, 2, 1, 0);
         s.ui.ask = UiState::Ask::Unfreeze; s.ui.ask_target = 2;
     }},
    // Freeze plan F2d: deep freeze. Shift on the PRINT sheet, a deep-frozen track's Stage, Tracks and Seq sheet, its
    // plug-ins loading again (THAWING), and the question an edit of it asks.
    {"printdeep", [](Shell&, Snapshot& s) { print_scene(s); s.ui.shift = true; }},
    {"deepfreeze", [](Shell&, Snapshot& s) { deep_scene(s); on_stage(s, 2, 1, 0); }},
    {"thawing", [](Shell&, Snapshot& s) { deep_scene(s); on_stage(s, 2, 1, 0); s.thawing = 1u << 2; }},
    {"tracksdeep", [](Shell&, Snapshot& s) { // track 3 deep frozen, track 5 frozen
         deep_scene(s);
         s.ui.page = PageId::Tracks;
         auto& five = s.freeze[4];
         five.state = FreezeState::Frozen; five.set(0, true);
         const float loads[] {0.04f, 0.11f, 0.002f, 0.02f, 0.006f, 0.09f, 0.03f, 0.05f};
         for (unsigned t = 0; t < 8; ++t) s.machine.track_load[t] = loads[t];
         s.machine.render_load = 0.33f;
     }},
    {"seqdeep", [](Shell&, Snapshot& s) { deep_scene(s); s.ui.page = PageId::Stage; s.ui.seq_held = true; s.ui.seq_down = s.now - seq_sheet_ms; }},
    {"askthaw", [](Shell&, Snapshot& s) {
         deep_scene(s); on_stage(s, 2, 1, 0);
         s.ui.ask = UiState::Ask::Unfreeze; s.ui.ask_target = 2;
     }},
    {"seqbank", [](Shell&, Snapshot& s) { // Seq held + Pad Bank: bank B, track 3 plays P18, P20 waits for the bar
         s.ui.page = PageId::Stage; s.ui.track = 2; s.playing = true; s.ui.pattern_bank = 1;
         for (unsigned n : {16u, 17u, 19u, 22u}) { s.sketches[2][n].notes = true; s.sketches[2][n].length = 32 * sixteenth_ticks; }
         s.playing_patterns[2] = 17; s.project.tracks[2].pattern = 19; s.positions[2] = int(9 * sixteenth_ticks);
         s.ui.seq_held = true; s.ui.seq_down = s.now - seq_sheet_ms;
     }},
    {"patternsbank", [](Shell&, Snapshot& s) { // Patterns on bank B: P17-P32 across
         s.ui.page = PageId::Patterns; s.ui.track = 2; s.playing = true; s.ui.pattern_bank = 1;
         const uint8_t live[] {16, 17, 16, 0, 18, 16, 0, 0};
         for (unsigned t = 0; t < 8; ++t) {
             s.playing_patterns[t] = live[t]; s.project.tracks[t].pattern = live[t]; s.positions[t] = int((4 + t) * sixteenth_ticks);
             for (unsigned n = 16; n < 16 + t % 4 + 1; ++n) s.sketches[t][n].notes = s.project.tracks[t].used();
         }
     }},
    {"patternsbankh", [](Shell&, Snapshot& s) { // Patterns on bank H: three-digit columns, P113-P128
         s.ui.page = PageId::Patterns; s.ui.track = 0; s.ui.pattern_bank = 7;
         s.project.tracks[0].pattern = 127;
         for (unsigned n = 112; n < 128; n += 3) s.sketches[0][n].notes = true;
     }},
    {"seqchain", [](Shell&, Snapshot& s) { // seq-sheet2 B: track 3 chained P1 > P4 > P2 > P5 > P3, P4 playing
         s.ui.page = PageId::Stage; s.ui.track = 2; s.playing = true;
         auto& t = s.project.tracks[2];
         const uint8_t chain[] {0, 3, 1, 4, 2};
         for (unsigned n = 0; n < 5; ++n) t.chain[n] = chain[n];
         t.chain_count = 5; t.chain_at = 1; t.pattern = 3;
         s.playing_patterns[2] = 3; s.positions[2] = int(6 * sixteenth_ticks);
         s.ui.seq_held = true; s.ui.seq_down = s.now - seq_sheet_ms;
     }},
    {"seqbpm", [](Shell&, Snapshot& s) { // Seq held, the wheel by tenths, BPM per pattern: pattern 2's own tempo
         s.ui.page = PageId::Patterns; s.ui.track = 0; s.project.seq_tempo = true;
         s.project.pattern_tempo[1] = 985; s.ui.lead_pattern = 1; s.tempo = 985; s.ui.bpm_tenths = true;
         s.ui.seq_held = true; s.ui.seq_down = s.now - seq_sheet_ms;
     }},
    {"seqclear", [](Shell&, Snapshot& s) { // Seq held + Erase + pad 2: B1 Cancel, B3 Do it
         s.ui.page = PageId::Tracks; s.ui.track = 2; s.ui.erase = true;
         s.ui.seq_held = true; s.ui.seq_down = s.now - seq_sheet_ms;
         s.ui.ask = UiState::Ask::ClearPattern; s.ui.ask_target = 1;
     }},
    {"countin", [](Shell&, Snapshot& s) { // Seq Record with Count-In: the third beat of the bar before the take
         s.ui.page = PageId::Patterns; s.ui.track = 0; s.playing = true; s.ui.recording = true; s.count_in = 3;
         s.project.signature = 2; // 3/4
         s.ui.settings.rec_quantize = false;
     }},
    {"recarmed", [](Shell&, Snapshot& s) { // Seq Record pressed while stopped: Play starts the take
         s.ui.page = PageId::Patterns; s.ui.track = 0; s.ui.rec_armed = true; s.ui.locate = 5 * ticks_per_quarter + 12;
     }},
    {"timecorrect", [](Shell& shell, Snapshot& s) { // Shift + Pad 14 (time-correct C): 70 % toward Q, swung 62 %, as it plays
         s.ui.page = PageId::TimeCorrect; s.ui.track = 0;
         loose_hand(shell.patterns().at[0][s.project.tracks[0].pattern % pattern_count]);
         auto& feel = shell.patterns().at[0][s.project.tracks[0].pattern % pattern_count].feel;
         feel.strength = 70; feel.swing = 62; feel.groove_from = 1 + 2;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"timecorrectoff", [](Shell& shell, Snapshot& s) { // QUANTIZE off: the notes as written, nothing slid
         s.ui.page = PageId::TimeCorrect; s.ui.track = 0;
         loose_hand(shell.patterns().at[0][s.project.tracks[0].pattern % pattern_count]);
     }},
    {"patterncopy", [](Shell&, Snapshot& s) { // Shift + Erase on Patterns, then a pattern: where does it go
         s.ui.page = PageId::Patterns; s.ui.track = 0; s.ui.copying = UiState::Copying::Patterns; s.ui.copy_from = 0;
     }},
    {"song", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 24; s.ui.song_view = 16; s.ui.song_zoom = 2;
         s.ui.song_pattern = 2; s.ui.song_length = 4;
         s.playing = s.song_playing = true; s.song_bar = 20; s.ticks = 48 * 4;
         // Punches written with Seq Record: a stutter on the master, a low pass on track 3.
         s.project.song.add_punch({23, 12, 4, 4, 255, 100});
         s.project.song.add_punch({27, 0, 16, 8, 2, 80});
         s.project.song.add_punch({31, 8, 8, 1, 255, 100});
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"songzoom", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 26; s.ui.song_view = 24; s.ui.song_zoom = 0;
         s.playing = s.song_playing = true; s.song_bar = 25; s.ticks = 40 * 4;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"songclip", [](Shell&, Snapshot& s) { // the cursor on a clip: B2 and B3 are its jobs
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 26; s.ui.song_view = 16;
         s.playing = true;
     }},
    {"songshift", [](Shell&, Snapshot& s) { // and with Shift held
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 26; s.ui.song_view = 16; s.ui.shift = true;
     }},
    // Roadmap P5: the song's tempo lane, a step up on bar 9, a ramp to 140 on bar 17, a step down on bar 25; the
    // cursor on the ramp's point, Shift held: its tempo and shape on K1 and K2.
    {"songtempo", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 16; s.ui.song_view = 4; s.ui.song_zoom = 2; s.ui.shift = true;
         s.project.song.tempo.set(8, 1280, false); s.project.song.tempo.set(16, 1400, true); s.project.song.tempo.set(24, 1000, false);
         s.playing = s.song_playing = true; s.song_bar = 13; s.ticks = 48 * 4;
     }},
    {"songtempoadd", [](Shell&, Snapshot& s) { // the cursor between points: Shift + K1 adds one at the lane's tempo there
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 12; s.ui.song_view = 4; s.ui.song_zoom = 2; s.ui.shift = true;
         s.project.song.tempo.set(8, 1280, false); s.project.song.tempo.set(16, 1400, true);
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
    {"sendspads", [](Shell&, Snapshot& s) { // a kit's SENDS, B2 again: its pads' own sends into bus A
         s.ui.page = PageId::Sends; s.ui.track = 3; s.ui.sends_pads = true; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3];
         const uint8_t sends[16] {0, 30, 15, 15, 20, 30, 10, 10, 0, 0, 25, 25, 0, 0, 40, 60};
         for (unsigned n = 0; n < 16; ++n) t.pads[n].sends[0] = sends[n];
         t.pads[14].sends[1] = 35;
     }},
    {"sendspadsb", [](Shell&, Snapshot& s) { // bus B: the pads that send there
         s.ui.page = PageId::Sends; s.ui.track = 3; s.ui.sends_pads = true; s.ui.sends_bus = 1; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3];
         t.pads[5].sends[1] = 20; t.pads[11].sends[1] = 10; t.pads[14].sends[1] = 35; t.last_pad = 14;
     }},
    {"bus", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Bus; s.ui.track = 2; s.playing = true;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    // Roadmap P4, MOD (MOTION again on Motion): an LFO on two lanes, a follower ducking a track, a macro on three.
    {"modlfo", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::ModPage; s.ui.track = 2; s.playing = true; s.ui.mod_index = 0; s.ui.mod_target = 1;
         auto& m = s.project.modulators[0];
         m.kind = ModKind::Lfo; m.shape = LfoShape::Triangle; m.sync = true; m.division = 2;
         m.targets[0] = ModTarget {2, uint8_t(motion_fx_param(0, 0)), 0, 40, 0, 100, 0};
         m.targets[1] = ModTarget {4, uint8_t(motion_pan), 0, -70, -50, 50, 0};
         s.mod_values[0] = 0.4f; s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    {"modfollow", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::ModPage; s.ui.track = 1; s.playing = true; s.ui.mod_index = 3; s.ui.shift = true;
         auto& m = s.project.modulators[3];
         m.kind = ModKind::Follower; m.source = 0; m.attack = 10; m.release = 45;
         m.targets[0] = ModTarget {1, uint8_t(motion_level), 0, -80, 0, 127, 0};
         m.targets[1] = ModTarget {5, uint8_t(motion_send(1)), 0, 50, 0, 100, 0};
         s.mod_values[3] = 0.7f;
     }},
    {"macro", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::ModPage; s.ui.mod_view = 1; s.ui.track = 2; s.playing = true; s.ui.macro_index = 1; s.ui.macro_target = 0;
         auto& m = s.project.macros[1];
         m.value = 88;
         m.targets[0] = MacroTarget {2, uint8_t(motion_fx_param(0, 2)), 0, 10, 90};
         m.targets[1] = MacroTarget {3, uint8_t(motion_level), 0, 100, 40};
         m.targets[2] = MacroTarget {0, uint8_t(motion_return(0)), 0, 20, 80};
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"rackkey", [](Shell&, Snapshot& s) { // roadmap P4: a Compressor on track 3's chain keyed by track 1 (the kick), its KEY row
         s.ui.page = PageId::Rack; s.ui.track = 2; s.playing = true;
         auto& slot = s.project.tracks[2].fx.slots[0];
         slot = EffectSlot {uint8_t(EffectKind::Compressor), false, {-30, 8, 4, 5, 30, 100}, fx_key_track(0)};
         s.ui.fx_cursor[2] = 0; s.ui.fx_row[2] = 2; s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"buskey", [](Shell&, Snapshot& s) { // roadmap P4: a Gate on bus A keyed by track 2, KEY on its second row
         s.ui.page = PageId::Bus; s.ui.track = 2; s.ui.sends_bus = 0; s.playing = true;
         auto& chain = s.project.buses[0].fx;
         chain.slots[1] = EffectSlot {uint8_t(EffectKind::Gate), false, {-36, 20, 0, 0, 0, 0}, fx_key_track(1)};
         s.ui.fx_cursor[UiState::cursor_of(bus_chain(0))] = 1; s.ui.fx_row[UiState::cursor_of(bus_chain(0))] = 1;
     }},
    {"fxpick", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Rack; s.ui.track = 2; s.ui.fx_cursor[2] = 3; s.playing = true;
         s.ui.picker_open = true; s.ui.picker_items = true; s.ui.picker_family = 2; s.ui.picker_item = 2;
     }},
    {"pads", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true; s.sample = break_sample(); s.ui.turning = 1; s.ui.turned_at = s.now;
         pad_waves(s);
     }},
    {"padscolour", [](Shell&, Snapshot& s) { // a kit in colours of its own (Stage's COLOUR, an MPC kit's)
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         coloured_kit(s);
         pad_waves(s);
     }},
    {"padsplaying", [](Shell&, Snapshot& s) { // the pad in hand as it plays: a head on its small waveform
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         playing_head(s, 9000, 0.8f);
         pad_waves(s);
     }},
    {"padslines", [](Shell&, Snapshot& s) { // a kit playing: a line along the foot of each sounding pad, the one in hand too
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         pad_voice(s, 0, 8, 40, 0.62f, 0.9f);  // A09, in hand: a line in the background colour
         pad_voice(s, 1, 0, 39, 0.3f, 1.0f);   // A01 kick
         pad_voice(s, 2, 2, 38, 0.85f, 0.35f); // A03 hat, fading
         pad_voice(s, 3, 5, 37, 0.18f, 0.8f);  // A06
         pad_voice(s, 4, 0, 30, 0.95f, 0.1f);  // an older kick: the newest one's line shows
         pad_waves(s);
     }},
    {"padcopy", [](Shell&, Snapshot& s) { // Shift + Erase, then a pad: where does it go
         s.ui.page = PageId::Pads; s.ui.track = 3; s.sample = break_sample(); s.ui.copying = UiState::Copying::Pads; s.ui.copy_from = 2;
     }},
    {"mutepads", [](Shell&, Snapshot& s) { // mute mode latched on a kit page: two pads muted
         s.ui.page = PageId::Pads; s.ui.track = 3; s.sample = break_sample(); s.ui.mute_latch = true; s.playing = true;
         s.project.tracks[3].set_pad_muted(1, true); s.project.tracks[3].set_pad_muted(6, true);
     }},
    {"copytrack", [](Shell&, Snapshot& s) { // Pad Bank held, Shift + Erase, track 2 picked: where does it go
         s.ui.page = PageId::Stage; s.ui.track = 1; s.ui.pad_bank = true;
         s.ui.copying = UiState::Copying::Tracks; s.ui.copy_from = 1;
     }},
    {"movetrack", [](Shell&, Snapshot& s) { // Pad Bank held, B2 MOVE, track 2 picked: where does it go (B1 back)
         s.ui.page = PageId::Stage; s.ui.track = 1; s.ui.pad_bank = true;
         s.ui.copying = UiState::Copying::Tracks; s.ui.moving = true; s.ui.copy_from = 1;
     }},
    {"tracksshift", [](Shell&, Snapshot& s) { // Tracks with Shift held: MOVE and COPY on B2 and B3
         s.ui.page = PageId::Tracks; s.ui.track = 1; s.ui.shift = true;
     }},
    {"cleartrack", [](Shell&, Snapshot& s) { // Pad Bank held + Erase + pad 1: B1 Cancel, B3 Clear
         s.ui.page = PageId::Stage; s.ui.track = 0; s.ui.pad_bank = true; s.ui.erase = true;
         s.ui.ask = UiState::Ask::ClearTrack; s.ui.ask_target = 0;
     }},
    {"repeat", [](Shell&, Snapshot& s) { // Note Repeat on, on Keys: 1/8, 1/16, 1/32 on B1-B3
         s.ui.page = PageId::Keys; s.playing = true; s.project.tracks[0].arp = true; s.project.tracks[0].arp_rate = 4;
         s.ui.held_pads = 1u << 4;
     }},
    {"repeattriplets", [](Shell&, Snapshot& s) { // and with Shift held: the triplets
         s.ui.page = PageId::Keys; s.playing = true; s.project.tracks[0].arp = true; s.project.tracks[0].arp_rate = 5;
         s.ui.shift = true; s.ui.shift_used = true;
     }},
    {"undo", [](Shell&, Snapshot& s) { // Shift + Minus: what came back
         s.ui.page = PageId::Pads; s.ui.track = 3; s.sample = break_sample();
         std::snprintf(s.ui.notice, sizeof(s.ui.notice), "Undo Delete A05"); s.ui.notice_at = s.now;
     }},
    {"samples", [](Shell&, Snapshot& s) { // the browser on the card: folders first, then files
         s.ui.page = PageId::Samples; s.ui.track = 3; s.playing = true; s.ui.files_cursor = 3; s.sample_root = "/media/card/Samples";
         s.files_folder = s.sample_root; s.files_top = false;
         for (auto name : {"EDITS", "LOOPER", "RECORDED"}) s.files.push_back({std::string("/media/card/Samples/") + name, name, 0, true});
         const char* files[] {"BASS LOOP", "BREAK", "DRUM LOOP", "KEYS", "PERC", "VOCAL"};
         for (auto name : files) s.files.push_back({std::string("/media/card/Samples/") + name + ".wav", name, 0, false});
         s.files[4].path = s.project.samples[4].path;
         s.ui.files_count = uint16_t(s.files.size()); s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"samplesinstruments", [](Shell&, Snapshot& s) { // a folder of instruments: each tagged by its kind, a kit, an expansion
         s.ui.page = PageId::Samples; s.ui.track = 3; s.playing = true; s.ui.files_cursor = 0; s.sample_root = "/media/card/Samples";
         s.files_folder = s.sample_root + "/Instruments"; s.files_top = false;
         struct Row { const char* name; const char* file; bool instrument, kit, expansion; };
         const Row rows[] {{"CHOIR", "Choir.exs", true, false, false},
                           {"PIANO", "Piano.sfz", true, false, false},
                           {"ELECTRIC PIANO", "Electric Piano.xpm", true, false, false},
                           {"ORGAN", "Organ.xty", true, false, false},
                           {"DRUM KIT", "Drum Kit.xpm", false, true, false},
                           {"EXPANSION", "Expansion.xpn", false, false, true}};
         for (const auto& r : rows) {
             SampleFile f;
             f.path = s.files_folder + "/" + r.file; f.name = r.name;
             f.instrument = r.instrument; f.kit = r.kit; f.expansion = r.expansion;
             s.files.push_back(f);
         }
         s.ui.files_count = uint16_t(s.files.size()); s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"samplespreview", [](Shell&, Snapshot& s) { // B3 on: the file under the cursor fills its row as it plays, 1.2 of 3.1 s
         s.ui.page = PageId::Samples; s.ui.track = 3; s.ui.preview = true; s.ui.files_cursor = 5; s.sample_root = "/media/card/Samples";
         s.files_folder = s.sample_root; s.files_top = false;
         for (auto name : {"EDITS", "LOOPER", "RECORDED"}) s.files.push_back({std::string("/media/card/Samples/") + name, name, 0, true});
         const char* files[] {"BASS LOOP", "BREAK", "DRUM LOOP", "KEYS", "PERC", "VOCAL"};
         for (auto name : files) s.files.push_back({std::string("/media/card/Samples/") + name + ".wav", name, 0, false});
         s.files[4].path = s.project.samples[4].path;
         s.ui.files_count = uint16_t(s.files.size());
         PlayHead h; // the audition voice on the preview slot
         h.serial = 50; h.pad = int16_t(sampler_pads); h.slot = uint16_t(preview_slot);
         h.from = 0; h.to = 137592; h.loop_at = 0; h.at = 52920; h.step = 1; h.level = 0.85f; h.wake = 1;
         s.heads[0] = h;
     }},
    {"banks", [](Shell&, Snapshot& s) { // the card's Samples/Banks: bank files, Push loads one into the bank in view
         s.ui.page = PageId::Samples; s.ui.track = 3; s.ui.files_cursor = 1; s.sample_root = "/media/card/Samples";
         s.files_folder = s.sample_root + "/Banks"; s.files_top = false; s.project.tracks[3].bank = 2;
         for (auto name : {"BREAKS", "DRUMS A", "VOCAL CHOPS"}) s.files.push_back({s.files_folder + "/" + name + ".mldb", name, 0, false, true});
         s.ui.files_count = uint16_t(s.files.size());
         std::snprintf(s.ui.notice, sizeof(s.ui.notice), "Bank C: DRUMS A"); s.ui.notice_at = s.now;
     }},
    {"savebank", [](Shell&, Snapshot& s) { // Shift + Sample Select on Samples: Save Bank's Name sheet
         s.ui.page = PageId::Samples; s.ui.track = 3; s.sample_root = "/media/card/Samples"; s.files_folder = s.sample_root;
         s.project.tracks[3].bank = 1; s.sample = break_sample();
         s.ui.name.start(NameTarget::Bank, "KIT B", name_length - 1, true);
     }},
    {"browserinternal", [](Shell&, Snapshot& s) { // B2: the internal drive's categories, preview on
         s.ui.page = PageId::Samples; s.ui.track = 3; s.ui.files_drive = 1; s.ui.preview = true; s.ui.files_cursor = 7;
         s.sample_root = "/media/card/Samples"; s.files_folder = ""; s.files_top = true;
         for (auto name : {"808", "BASS", "BREAK", "CLAPS", "CYMBALS", "FX", "HI-HATS", "KICKS", "LOOPS", "MELODIC", "PERC", "PHRASES", "SNARES", "TOMS", "USER", "VOCAL"})
             s.files.push_back({std::string("/content/Samples/") + name, name, 0, true});
         s.ui.files_count = uint16_t(s.files.size());
     }},
    {"trim", [](Shell&, Snapshot& s) { // Stage on a kit: B1 TRIM, the last pad's region
         on_stage(s, 3, 0, 0); s.playing = true; s.sample = break_sample();
         auto& pad = s.project.tracks[3].pads[8]; pad.start = 18000; pad.end = 150000;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"triminstrument", [](Shell&, Snapshot& s) { // ZONES (TRIM's place) on a pad holding an .exs: a tile a zone over its keys, the one in hand named
         on_stage(s, 3, 0, 0); s.playing = true; choir_pad(s, 8);
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"zonestrimmed", [](Shell&, Snapshot& s) { // ZONES: the zone in hand tuned 12 cents up and 1.5 dB down (round 3 "instrument C")
         on_stage(s, 3, 0, 0); s.playing = true; choir_pad(s, 8);
         auto& t = s.project.tracks[3];
         set_trim(t.zone_trims, 8, 9, t.pads[8].sample, 12, -15);
         s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    {"zoneslayered", [](Shell&, Snapshot& s) { // ZONES on a layered SFZ: three velocity rows, round robin, C4 at velocity 100's zone in hand
         on_stage(s, 3, 0, 0); s.playing = true; piano_pad(s, 8);
     }},
    {"zonesreading", [](Shell&, Snapshot& s) { // ZONES while its files are first read: outlines until each lands, the ring
         on_stage(s, 3, 0, 0); choir_pad(s, 8);
         for (unsigned n = 12; n < 22; ++n) s.sample.part_state[n] = PartState::Waiting;
         s.sample.parts_ready = 12;
     }},
    {"zonesproblems", [](Shell&, Snapshot& s) { // ZONES with three files missing, regions left out and opcodes it does not play
         on_stage(s, 3, 0, 0); choir_pad(s, 8);
         for (unsigned n : {5u, 6u, 15u}) s.sample.part_state[n] = PartState::Failed;
         s.sample.parts_ready = 19; s.sample.parts_failed = 3; s.sample.error = "Files missing";
         s.sample.left = 4; s.sample.ignored = "cutoff2, eg06_time1, lfo01_freq";
     }},
    {"instrumentampenv", [](Shell&, Snapshot& s) { // AMP ENV on an instrument: the key strip, the envelope under it
         on_stage(s, 3, 0, 3); s.playing = true; choir_pad(s, 8);
         auto& pad = s.project.tracks[3].pads[8]; pad.attack = 30; pad.decay = 100; pad.release = 55; pad.velocity_sense = 60;
     }},
    {"instrumenttune", [](Shell&, Snapshot& s) { // TUNE on an instrument, Semi -5: two octaves round the key a hit plays, C4 outlined
         on_stage(s, 3, 1, 0); s.playing = true; choir_pad(s, 8);
         s.project.tracks[3].pads[8].pitch = -5;
     }},
    {"instrumentfilter", [](Shell&, Snapshot& s) { // FILTER on an instrument: the strip, the pad's filter under it
         on_stage(s, 3, 2, 0); s.playing = true; choir_pad(s, 8);
         auto& pad = s.project.tracks[3].pads[8]; pad.filter = PadFilter::Lpf2; pad.cutoff = 70; pad.resonance = 40;
     }},
    {"instrumentmix", [](Shell&, Snapshot& s) { // MIX on an instrument: the map, the zone a hit plays lit
         on_stage(s, 3, 0, 2); s.playing = true; choir_pad(s, 8);
     }},
    {"trimplaying", [](Shell&, Snapshot& s) { // TRIM while the pad plays: the head, its wake, the time heard
         on_stage(s, 3, 0, 0); s.playing = true; s.sample = break_sample();
         auto& pad = s.project.tracks[3].pads[8]; pad.start = 18000; pad.end = 150000;
         playing_head(s, 53100); // 1.204 s in
     }},
    {"stagevoices", [](Shell&, Snapshot& s) { // PLAY, Poly: a roll of three voices, the newest strong with its wake
         on_stage(s, 3, 1, 1); s.playing = true; s.sample = break_sample();
         auto& pad = s.project.tracks[3].pads[8]; pad.start = 18000; pad.end = 150000;
         stage_voice(s, 0, 12, 38000, 1.0f);
         stage_voice(s, 1, 11, 75000, 0.7f);
         stage_voice(s, 2, 10, 112000, 0.35f);
     }},
    {"stagemono", [](Shell&, Snapshot& s) { // PLAY, Mono: the new hit cut the last one, whose head is fading out
         on_stage(s, 3, 1, 1); s.playing = true; s.sample = break_sample();
         auto& pad = s.project.tracks[3].pads[8]; pad.start = 18000; pad.end = 150000; pad.mono = true;
         stage_voice(s, 0, 12, 26000, 1.0f);
         stage_voice(s, 1, 11, 81000, 0.4f).cut = true;
     }},
    {"trimlooping", [](Shell&, Snapshot& s) { // TRIM, Loop on: the head just went round, the ghost at the end it left
         on_stage(s, 3, 0, 0); s.playing = true; s.sample = break_sample();
         auto& pad = s.project.tracks[3].pads[8]; pad.start = 40000; pad.end = 140000;
         pad.loop = true; pad.loop_lock = false; pad.loop_start = 82000; pad.play = PadPlay::Gate;
         PlayHead& h = stage_voice(s, 0, 7, 84300, 1.0f);
         h.pass = 2; h.wrap = 0.8f;
     }},
    {"trimreverse", [](Shell&, Snapshot& s) { // TRIM, reversed: the head runs right to left, its wake to its right
         on_stage(s, 3, 0, 0); s.playing = true; s.sample = break_sample();
         auto& pad = s.project.tracks[3].pads[8]; pad.start = 18000; pad.end = 150000; pad.reverse = true;
         stage_voice(s, 0, 7, 96000, 0.9f);
     }},
    {"chop", [](Shell&, Snapshot& s) { // Threshold on the pad's region, slice 5 in hand
         s.ui.page = PageId::Chop; s.ui.track = 3; s.playing = true; s.sample = break_sample(); s.ui.chop_sample = 4;
         s.ui.slices = chop(ChopMode::Regions, 12, 0, 0, s.sample.frames, s.sample.hits, s.sample.hit_count, 920);
         s.ui.chop_mode = ChopMode::Regions;
         s.ui.chop_slice = 4; s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"chopmanual", [](Shell&, Snapshot& s) { // Manual: pad 1 is playing the region, three cuts tapped so far
         s.ui.page = PageId::Chop; s.ui.track = 3; s.sample = break_sample(); s.ui.chop_sample = 4;
         s.ui.chop_mode = ChopMode::Manual;
         s.ui.slices = chop(ChopMode::Manual, 0, 0, 0, s.sample.frames, s.sample.hits, s.sample.hit_count, 920);
         for (uint32_t at : {28000u, 57000u, 86000u}) s.ui.slices.add(at, s.sample.frames, manual_slices);
         s.ui.chop_slice = 3; s.ui.chop_take_at = s.now > 2400 ? s.now - 2400 : 1;
         chop_voice(s, s.ui.slices.start[0], s.ui.slices.end(s.ui.slices.count - 1, s.sample.frames), 0.48f, 0.9f);
     }},
    {"chopplaying", [](Shell&, Snapshot& s) { // a pad playing slice 8 while slice 5 is in hand: it lights, its head runs through it
         s.ui.page = PageId::Chop; s.ui.track = 3; s.playing = true; s.sample = break_sample(); s.ui.chop_sample = 4;
         s.ui.slices = chop(ChopMode::Regions, 12, 0, 0, s.sample.frames, s.sample.hits, s.sample.hit_count, 920);
         s.ui.chop_mode = ChopMode::Regions; s.ui.chop_slice = 4;
         chop_voice(s, s.ui.slices.start[7], s.ui.slices.end(7, s.sample.frames), 0.55f, 0.85f);
     }},
    {"chopdense", [](Shell&, Snapshot& s) { // 37 slices: numbers that would collide step aside, slice 12 in hand and 20 sounding keep theirs
         s.ui.page = PageId::Chop; s.ui.track = 3; s.playing = true; s.sample = break_sample(); s.ui.chop_sample = 4;
         s.ui.slices = chop(ChopMode::Regions, 37, 0, 0, s.sample.frames, s.sample.hits, s.sample.hit_count, 920);
         s.ui.chop_mode = ChopMode::Regions; s.ui.chop_slice = 11;
         chop_voice(s, s.ui.slices.start[19], s.ui.slices.end(19, s.sample.frames), 0.5f, 0.85f);
     }},
    {"chopplayingsel", [](Shell&, Snapshot& s) { // the slice in hand playing: its raised ground and handles stay, the head over them
         s.ui.page = PageId::Chop; s.ui.track = 3; s.playing = true; s.sample = break_sample(); s.ui.chop_sample = 4;
         s.ui.slices = chop(ChopMode::Regions, 12, 0, 0, s.sample.frames, s.sample.hits, s.sample.hit_count, 920);
         s.ui.chop_mode = ChopMode::Regions; s.ui.chop_slice = 4;
         chop_voice(s, s.ui.slices.start[4], s.ui.slices.end(4, s.sample.frames), 0.4f, 1.0f);
     }},
    {"chopbeats", [](Shell&, Snapshot& s) { // Beats on the break's own tempo (two bars at 92 BPM): its tempo by the file's length
         s.ui.page = PageId::Chop; s.ui.track = 3; s.sample = break_sample(); s.ui.chop_sample = 4;
         s.sample.beats.tempo = 920;
         for (unsigned n = 0; n < 8; ++n) s.sample.beats.at.push_back(230087u * n / 8);
         s.ui.chop_mode = ChopMode::Beats;
         s.ui.slices = chop(ChopMode::Beats, 1, 0, 0, s.sample.frames, s.sample.hits, s.sample.hit_count, 1240, &s.sample.beats);
         s.ui.chop_slice = 2; s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    {"chopshift", [](Shell&, Snapshot& s) { // Shift: Extract / Split / Merge, zoomed in at the slice's end
         s.ui.page = PageId::Chop; s.ui.track = 3; s.sample = break_sample(); s.ui.chop_sample = 4;
         s.ui.slices = chop(ChopMode::Threshold, 40, 20000, 200000, s.sample.frames, s.sample.hits, s.sample.hit_count, 920);
         s.ui.chop_slice = 2; s.ui.chop_zoom = 45; s.ui.chop_focus = 1; s.ui.chop_fine = true;
         s.project.samples[4].frames = s.sample.frames;
         s.ui.shift = true; s.ui.shift_used = true;
     }},
    {"chopfine", [](Shell&, Snapshot& s) { // K1 touched, zoomed in at the slice's start: the wheel's handle drawn wider
         s.ui.page = PageId::Chop; s.ui.track = 3; s.sample = break_sample(); s.ui.chop_sample = 4;
         s.project.samples[4].frames = s.sample.frames;
         s.ui.slices = chop(ChopMode::Threshold, 40, 20000, 200000, s.sample.frames, s.sample.hits, s.sample.hit_count, 920);
         s.ui.chop_slice = 2; s.ui.chop_zoom = 30; s.ui.chop_focus = 0; s.ui.chop_fine = true; s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"levels", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Pads; s.ui.levels_held = true; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3]; t.levels = true; t.levels_pad = 1; t.last_pad = 1;
         t.levels_kind = uint8_t(LevelsKind::Velocity); t.levels_ends[unsigned(LevelsKind::Velocity)] = {40, 127}; // sixteen levels from 40
         s.sample.frames = 26460;
         s.ui.levels_hit = 10; s.ui.levels_value = level_velocity(10);
     }},
    {"inputconfig", [](Shell&, Snapshot& s) { // the mic, monitored: the speaker goes quiet
         s.ui.page = PageId::InputConfig; s.ui.track = 3; s.take.level = 0.12f;
         auto& in = s.ui.settings.input;
         in.source = InputSource::Mic; in.threshold = -24; in.monitor = Monitor::On; in.length = rec_seq;
         s.monitoring = true; s.speaker_quieted = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"inputusb", [](Shell&, Snapshot& s) { // USB-C L, waiting for the computer; Rec Input FX on
         s.ui.page = PageId::InputConfig; s.ui.track = 3; s.take.level = 0.0f;
         auto& in = s.ui.settings.input;
         in.source = InputSource::UsbL; in.threshold = InputConfig::threshold_min; in.monitor = Monitor::Off; in.fx = true; in.gain = 6;
         s.usb_state = uint8_t(UsbStreamState::Waiting);
         s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    {"record", [](Shell&, Snapshot& s) { // a take onto B01 with three pads cut from it
         s.ui.page = PageId::Record; s.ui.track = 3; s.playing = true;
         auto& in = s.ui.settings.input;
         in.source = InputSource::Rear; in.length = 2 + 3; in.gain = 6; // 4 bars
         s.ui.rec_pad = 16; s.ui.rec_track = 3; s.ui.rec_slot = 5;
         s.take.state = uint8_t(TakeRecorder::State::Recording); s.take.length = 4 * 88200; s.take.frames = 88200 + 50000;
         s.take.level = 0.5f;
         s.ui.rec_chops = 3;
         s.ui.rec_chop_pad = {17, 18, 19}; s.ui.rec_chop_at = {22050, 66150, 110250};
         for (unsigned c = 0; c < 128 * s.take.frames / s.take.length; ++c)
             s.take.peaks[c] = uint8_t(std::clamp((c % 22 < 3 ? 0.9 : 0.55) * (0.8 + 0.2 * std::sin(c * 0.9)) * (c < 4 ? 0.1 : 1.0), 0.0, 1.0) * 255);
         s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    {"recordtrack", [](Shell&, Snapshot& s) { // sampling track 1's PROPHET after its effects: the pads play it, the take ends on its own
         s.ui.page = PageId::Record; s.ui.track = 3;
         auto& in = s.ui.settings.input;
         in.source = InputSource::Track; in.track = 0; in.post = true; in.length = rec_free; in.threshold = -48; in.monitor = Monitor::Auto;
         s.monitoring = true;
         s.ui.rec_pad = 4; s.ui.rec_track = 3; s.ui.rec_slot = 5;
         s.take.state = uint8_t(TakeRecorder::State::Recording); s.take.length = TakeRecorder::max_frames; s.take.frames = 3 * 44100;
         s.take.level = 0.42f;
         for (unsigned c = 0; c < 128 * s.take.frames / s.take.length; ++c)
             s.take.peaks[c] = uint8_t(std::clamp((0.35 + 0.5 * std::fabs(std::sin(c * 0.7))) * (c < 1 ? 0.2 : 1.0), 0.0, 1.0) * 255);
         s.ui.held_pads = 1u << 5 | 1u << 9;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"inputtrack", [](Shell&, Snapshot& s) { // Input Config with a track as the source, before its effects
         s.ui.page = PageId::InputConfig; s.ui.track = 3; s.take.level = 0.2f;
         auto& in = s.ui.settings.input;
         in.source = InputSource::Track; in.track = 0; in.post = false; in.monitor = Monitor::On;
         s.monitoring = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"recordseq", [](Shell&, Snapshot& s) { // Rec length Seq while playing: the take waits for the pattern to come round
         s.ui.page = PageId::Record; s.ui.track = 3; s.playing = true;
         auto& in = s.ui.settings.input;
         in.source = InputSource::Resample; in.length = rec_seq; in.threshold = InputConfig::threshold_min;
         s.ui.rec_pad = 2; s.ui.rec_track = 3; s.ui.rec_slot = 5;
         s.take.state = uint8_t(TakeRecorder::State::Cued); s.take.level = 0.3f;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"resample", [](Shell&, Snapshot& s) { // Shift + Pad 11: the pad to render onto
         s.ui.page = PageId::Pads; s.ui.track = 3; s.ui.resample_pick = true;
     }},
    {"resampling", [](Shell&, Snapshot& s) { // the render playing, on Patterns
         s.ui.page = PageId::Patterns; s.ui.track = 3; s.playing = true; s.ui.rendering = 1; s.render_frames = 44100 * 3 + 2000;
         s.render_state = uint8_t(Bounce::State::Running);
     }},
    {"songexport", [](Shell&, Snapshot& s) { // Song, B1: Export
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 0; s.ui.song_export = true;
         Clip clip; clip.track = 2; clip.pattern = 0; clip.bar = 0; clip.bars = 16;
         s.project.song.place(clip);
     }},
    {"songstems", [](Shell&, Snapshot& s) { // roadmap P4: Export with the wheel on STEMS
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 0; s.ui.song_export = true; s.ui.export_kind = 3;
         Clip clip; clip.track = 2; clip.pattern = 0; clip.bar = 0; clip.bars = 16;
         s.project.song.place(clip);
     }},
    {"songmidifile", [](Shell&, Snapshot& s) { // roadmap P4: Export with the wheel on MIDI FILE, no song: the patterns
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 0; s.ui.song_export = true; s.ui.export_kind = 4;
     }},
    {"mixdownname", [](Shell&, Snapshot& s) { // Export, B2: the mixdown's name
         s.ui.page = PageId::Song; s.ui.track = 2;
         s.ui.name.start(NameTarget::Mixdown, "Project 1", 31, false);
     }},
    {"project", [](Shell&, Snapshot& s) { // Shift + Pad 16: SAVE, OPEN, NEW
         s.ui.page = PageId::Project; s.playing = true;
         preview_projects(s);
     }},
    {"projectlist", [](Shell&, Snapshot& s) { // OPEN: the list, the open project in hand
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_list = true; s.ui.project_cursor = 3;
         preview_projects(s);
     }},
    {"filesheet", [](Shell&, Snapshot& s) { // Push on the open project: its jobs as cards, SAVE lit
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_list = true; s.ui.project_cursor = 3; s.ui.sheet_open = true;
         preview_projects(s);
     }},
    {"filesheetscroll", [](Shell&, Snapshot& s) { // turned to the last card: the rows scrolled, DELETE asking again
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_list = true; s.ui.project_cursor = 3; s.ui.sheet_open = true;
         s.ui.sheet_row = 7; s.ui.delete_armed = true;
         preview_projects(s);
     }},
    // Parity phase 10: another project's sheet with Load kit, New project asking first.
    {"loadkit", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.ui.project_list = true; s.ui.project_cursor = 4; s.ui.sheet_open = true; s.ui.sheet_row = 1;
         preview_projects(s);
     }},
    // Parity phase 10b: SD card access, its row on Project, then the card on a computer, ejected, held, and none to lend.
    {"cardrow", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.ui.project_list = true; s.ui.project_cursor = 0;
         preview_projects(s);
     }},
    {"cardaccess", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::CardAccess; s.ui.card.state = 2; s.ui.card.connected = true;
     }},
    {"cardejected", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::CardAccess; s.ui.card.state = 2; s.ui.card.connected = true; s.ui.card.ejected = true;
     }},
    {"cardlocked", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::CardAccess; s.ui.card.state = 2; s.ui.card.connected = true; s.ui.card.locked = true;
         s.ui.ask = UiState::Ask::TakeCardBack;
     }},
    {"cardnocomputer", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::CardAccess; s.ui.card.state = 2; s.playing = true;
     }},
    {"cardnone", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::CardAccess; s.ui.card.note = CardNote::NoCard;
     }},
    {"cardcamera", [](Shell&, Snapshot& s) { // not lent: a computer shows Screen Mirror's camera, which the drive would unplug
         s.ui.page = PageId::CardAccess; s.ui.card.note = CardNote::Camera;
     }},
    {"newproject", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.ui.project_cursor = 0; s.ui.ask = UiState::Ask::NewProject; s.ui.ask_target = 1;
         preview_projects(s);
     }},
    {"movefolder", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_list = true; s.ui.project_cursor = 3; s.ui.move_open = true; s.ui.move_cursor = 1;
         preview_projects(s);
     }},
    {"tempo", [](Shell&, Snapshot& s) { s.ui.page = PageId::Tempo; s.playing = true; s.ticks = ticks_per_quarter; s.ui.settings.click = 1; s.project.swing = 54; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"captured", [](Shell&, Snapshot& s) { // Shift + Seq Record on Patterns (capture C): P3 took the last bars, the undo keys a moment
         s.ui.page = PageId::Patterns; s.ui.track = 0; s.playing = true;
         s.ui.captured_pattern = 2; s.ui.captured_at = s.now;
     }},
    {"capturedmany", [](Shell&, Snapshot& s) { // roadmap P5: three tracks played in the phrase, each into its next empty pattern
         s.ui.page = PageId::Patterns; s.ui.track = 0; s.playing = true;
         s.ui.captured_pattern = 2; s.ui.captured_tracks = 3; s.ui.captured_at = s.now;
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
    // Parity phase 7: Knob FX, the master compressor, the picker's new effects.
    {"knobfx", [](Shell&, Snapshot& s) { // a kit: Ensemble on three pads of bank A, the fader 40 % of the way from A to B
         s.ui.page = PageId::KnobFx; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& k = s.project.knob_fx;
         load_knob(k, 19); k.on = true;
         for (unsigned pad : {0u, 1u, 4u}) k.set_pad(3, pad, true);
         knob_settings(k, {0.2f, 0.35f, 0.6f}, {0.7f, 0.9f, 0.15f}, 51);
         s.ui.knob_cursor = 19; s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"knobfxb", [](Shell&, Snapshot& s) { // B in hand, at B; the wheel turned to another effect, Push to load it
         s.ui.page = PageId::KnobFx; s.ui.track = 3; s.sample = break_sample();
         auto& k = s.project.knob_fx;
         load_knob(k, 19); k.on = true;
         knob_settings(k, {0.2f, 0.35f, 0.6f}, {0.7f, 0.9f, 0.15f}, 127);
         k.editing = 1;
         s.ui.knob_cursor = 22;
     }},
    {"knobfxtracks", [](Shell&, Snapshot& s) { // a synth track: Delay on three tracks
         s.ui.page = PageId::KnobFx; s.ui.track = 2; s.playing = true;
         auto& k = s.project.knob_fx;
         load_knob(k, 0); k.on = true;
         for (unsigned t : {0u, 2u, 5u}) k.set_track(t, true);
         s.ui.knob_cursor = 0;
     }},
    {"knobfxbypass", [](Shell&, Snapshot& s) { // bypassed, on every track
         s.ui.page = PageId::KnobFx; s.ui.track = 0; s.playing = true;
         auto& k = s.project.knob_fx;
         load_knob(k, 7); k.on = true; k.bypass = true; k.tracks = 0xffff;
         s.ui.knob_cursor = 7;
     }},
    {"knobfxstage", [](Shell&, Snapshot& s) { // the mode on away from its page: the pots are Amp Sim's
         s.ui.page = PageId::Stage; s.ui.track = 0; s.playing = true;
         auto& k = s.project.knob_fx;
         load_knob(k, 16); k.on = true; k.set_track(0, true);
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"knobfxshift", [](Shell&, Snapshot& s) { // and Shift held: its second row (bass, mid, treble)
         s.ui.page = PageId::Stage; s.ui.track = 0; s.ui.shift = true;
         auto& k = s.project.knob_fx;
         load_knob(k, 16); k.on = true; k.set_track(0, true);
     }},
    {"compressor", [](Shell&, Snapshot& s) { // Shift + Pad 5: engaged, Color on, taking 4.5 dB off
         s.ui.page = PageId::Compressor; s.playing = true;
         auto& c = s.project.compressor;
         c.on = true; c.color = true; c.amount = 60; c.attack = 25; c.release = 40; c.boost = 3;
         s.compression = 4.5f; s.ui.turning = 2; s.ui.turned_at = s.now;
         for (unsigned n = 0; n < comp_columns; ++n) { // a busy mix: beats over a held bed
             unsigned beat = n % 15;
             s.comp_trail[n] = {-14.0f + 9.0f * std::exp(-float(beat) / 3) + 1.5f * std::sin(float(n) * 0.7f),
                                1.5f + 3.5f * std::exp(-float(beat) / 4)};
         }
         kick_bar(s, 0.4f, 5.0f);
     }},
    {"compressorside", [](Shell&, Snapshot& s) { // Sidechain: track 4 (a kit) keys it whole, the tracks on the pads, track 1 selected
         s.ui.page = PageId::Compressor; s.playing = true; s.ui.track = 0;
         auto& c = s.project.compressor;
         c.on = true; c.color = true; c.sidechain = true; c.key = 3; c.amount = 55; c.attack = 5; c.release = 60;
         s.project.tracks[3].set_name("ACOUSTIC DRUM KITS"); // a long name, as the device's own
         s.compression = 7.0f;
         kick_trail(s);
         kick_bar(s, 0.62f, 9.0f);
     }},
    {"compressoroff", [](Shell&, Snapshot& s) { // bypassed: BYPASSED beside the page name, B3 lit, the curve grey
         s.ui.page = PageId::Compressor; s.playing = true;
         s.project.compressor.amount = 60;
     }},
    {"compressorsideoff", [](Shell&, Snapshot& s) { // the most the header holds: bypassed, Sidechain on a kit pad, Color
         s.ui.page = PageId::Compressor; s.playing = true; s.ui.track = 0;
         auto& c = s.project.compressor;
         c.on = false; c.color = true; c.sidechain = true; c.key = 3; c.key_pad = 1; c.amount = 60;
         s.ui.comp_pads = true; s.ui.comp_bank = 0;
     }},
    {"compressorkick", [](Shell&, Snapshot& s) { // Sidechain on the kit's A01 Kick alone: its pads, bank A
         s.ui.page = PageId::Compressor; s.playing = true; s.ui.track = 0;
         auto& c = s.project.compressor;
         c.on = true; c.sidechain = true; c.key = 3; c.key_pad = 0; c.amount = 60; c.attack = 5; c.release = 70;
         s.ui.comp_pads = true; s.ui.comp_bank = 0;
         kick_trail(s);
         kick_bar(s, 0.85f, 11.0f);
     }},
    {"pickerdirt", [](Shell&, Snapshot& s) { // the picker on Dirt: Amp sim, Tube drive, Soft clipper, Vintage, Vinyl, Tape emulator
         s.ui.page = PageId::Rack; s.ui.track = 2; s.ui.fx_cursor[2] = 2; s.playing = true;
         s.ui.picker_open = true; s.ui.picker_items = true; s.ui.picker_family = uint8_t(Family::Dirt); s.ui.picker_item = 8;
     }},
    {"rackspring", [](Shell&, Snapshot& s) { // Spring and Pumper in a track's chain; Shift + K turns the second row
         s.ui.page = PageId::Rack; s.ui.track = 2; s.ui.fx_cursor[2] = 0; s.playing = true;
         auto& chain = s.project.tracks[2].fx;
         const EffectKind kinds[] {EffectKind::Spring, EffectKind::Pumper};
         for (unsigned k = 0; k < 2; ++k) {
             chain.slots[k].kind = uint8_t(kinds[k]);
             const auto& info = effect_info(kinds[k]);
             for (unsigned q = 0; q < effect_params; ++q) chain.slots[k].params[q] = info.params[q].label ? info.params[q].initial : 0;
         }
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"settings", [](Shell&, Snapshot& s) { s.ui.page = PageId::SettingsPage; s.playing = true; s.ui.settings_door = 4; }}, // the doors, AUDIO in hand
    {"settingspads", [](Shell&, Snapshot& s) { s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 0; s.ui.settings_cursor = Settings::PadResponse; }},
    {"settingslights", [](Shell&, Snapshot& s) { s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 1; s.ui.settings_cursor = Settings::PadFade; }},
    {"settingsscreen", [](Shell&, Snapshot& s) { s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 3; s.ui.settings_cursor = Settings::Palette; }},
    {"settingsaudio", [](Shell&, Snapshot& s) { // AUDIO's rows, 48 kHz with a dropout
        s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 4; s.ui.settings_cursor = Settings::AudioBuffer; s.ui.settings_editing = true;
        s.ui.settings.audio_rate = 48000; s.ui.settings.audio_periods = 4;
        s.audio_rate = 48000; s.audio_periods = 4; s.audio_xruns = 1;
        s.machine.render_load = 0.47f; // the blocks' share of their time, beside the dropout (roadmap P2)
        s.machine.pdc_frames = 152;    // a 44.1 kHz plug-in effect's converter held back (roadmap P4)
    }},
    {"settingssampling", [](Shell&, Snapshot& s) { // the sampling rows: how Audio Recall cuts, when a take ends on its own
        s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 5; s.ui.settings_cursor = Settings::RecallGrabRow; s.ui.settings_editing = true;
    }},
    {"settingsmirror", [](Shell&, Snapshot& s) { // SCREEN's rows with Screen Mirror on at 1280 x 960, 60 fps, a computer showing it
        s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 3; s.ui.settings_cursor = Settings::MirrorSize;
        s.ui.settings.mirror = true; s.ui.settings.mirror_size = 3; s.ui.settings.mirror_fps = 60;
        s.mirror_state = uint8_t(MirrorState::Streaming); s.mirror_width = 1280; s.mirror_height = 960;
    }},
    {"settingsmirrorheld", [](Shell&, Snapshot& s) { // turned off while a computer shows it: it waits for the computer to close the camera
        s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 3; s.ui.settings_cursor = Settings::Mirror;
        s.ui.settings.mirror = false; s.ui.settings.mirror_size = 3;
        s.mirror_state = uint8_t(MirrorState::Streaming); s.mirror_width = 1280; s.mirror_height = 960; s.mirror_held = true;
    }},
    {"about", [](Shell&, Snapshot& s) { // About C: how the machine is doing, all well
         s.ui.page = PageId::About; s.playing = true;
         s.audio_rate = 48000; s.audio_periods = 3;
         auto& m = s.machine;
         m.cores = 4; m.core_load = {{38, 52, 30, 44}}; m.render_load = 0.31f;
         m.card = true; m.card_total = 31'900'000'000ull; m.card_free = 21'400'000'000ull;
         m.plugins = 3; m.plugins_load = 0.22f;
     }},
    {"abouttrouble", [](Shell&, Snapshot& s) { // a core near full, a dropout, the card late twice, no plug-ins
         s.ui.page = PageId::About; s.playing = true;
         s.audio_rate = 44100; s.audio_periods = 2; s.audio_xruns = 1;
         auto& m = s.machine;
         m.cores = 4; m.core_load = {{91, 64, 20, 12}}; m.render_load = 0.88f;
         m.card = true; m.card_total = 31'900'000'000ull; m.card_free = 640'000'000ull; m.stream_late = 2;
     }},
    {"aboutcredits", [](Shell&, Snapshot& s) { s.ui.page = PageId::About; s.ui.about_view = 1; }},
    // Round 2, pad-fx C: Low latched from the bar's start and Stutter held from its third beat, heard; an Echo kept
    // with the pattern on the fourth (quieter); the playhead between.
    {"punch", [](Shell& shell, Snapshot& s) {
         s.ui.page = PageId::Punch; s.ui.track = 2; s.playing = true; s.project.punch_target = 1;
         s.project.punch_bank = 0; // Melodee's first bank: Low latched, Stutter held
         s.ui.engaged[0] = {8, -1, true, 100}; s.ui.engaged[1] = {4, 4, false, 100}; s.ui.engaged_count = 2;
         s.ui.punch_slot = {{8, 4, -1, -1}}; s.ui.punch_selected = 4;
         s.ui.turning = 1; s.ui.turned_at = s.now;
         auto& pattern = shell.patterns().at[2][s.project.tracks[2].pattern];
         uint32_t bar = s.project.bar_ticks(), pass = std::max<uint32_t>(pattern.length, bar);
         pattern.punches = {{bar * 13 / 16, bar / 8, 14, 2, 100, 2, 1}};
         s.positions[2] = int(bar * 11 / 16); s.ticks = 4 * pass + uint32_t(s.positions[2]);
         s.ui.spans[0] = {4ull * pass, 0, 8, 2, 100, 2, 1, false};
         s.ui.spans[1] = {4ull * pass + bar * 10 / 16, 0, 4, 2, 100, 2, 1, false};
         s.ui.span_count = 2;
     }},
    {"punchkept", [](Shell& shell, Snapshot& s) { // stopped: what the pattern keeps, no playhead
         s.ui.page = PageId::Punch; s.ui.track = 2; s.project.punch_target = 1; s.project.punch_bank = 0;
         auto& pattern = shell.patterns().at[2][s.project.tracks[2].pattern];
         uint32_t bar = s.project.bar_ticks();
         pattern.punches = {{0, bar / 4, 1, 2, 100, 2, 1}, {bar / 2, bar / 8, 5, 2, 100, 2, 1}, {bar * 13 / 16, bar / 8, 14, 2, 100, 2, 1}};
     }},
    {"punchquantize", [](Shell&, Snapshot& s) { // Shift held: B3 QUANTIZE, its LED dark (punches start at once)
         s.ui.page = PageId::Punch; s.ui.track = 0; s.playing = true; s.project.punch_bank = 2; s.ui.shift = true;
         s.project.punch_quantize = false;
     }},
    {"punchfree", [](Shell&, Snapshot& s) { // quantize off on a track: the header's magnet says OFF beside its tag
         s.ui.page = PageId::Punch; s.ui.track = 2; s.playing = true; s.project.punch_target = 1; s.project.punch_bank = 0;
         s.project.punch_quantize = false;
     }},
    {"puncherase", [](Shell& shell, Snapshot& s) { // Erase held: the kept punches red, as Erase + pad takes them out
         s.ui.page = PageId::Punch; s.ui.track = 2; s.project.punch_target = 1; s.project.punch_bank = 0; s.ui.erase = true;
         auto& pattern = shell.patterns().at[2][s.project.tracks[2].pattern];
         uint32_t bar = s.project.bar_ticks();
         pattern.punches = {{0, bar / 4, 1, 2, 100, 2, 1}, {bar / 2, bar / 8, 5, 2, 100, 2, 1}, {bar * 13 / 16, bar / 8, 14, 2, 100, 2, 1}};
     }},
    // Parity phase 7b: the MPC's Pad FX, five engaged (the first bypassed), and Flex Beat.
    {"padfxmpc", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Punch; s.ui.track = 0; s.playing = true; s.project.punch_bank = 2; s.project.punch_target = 0;
         const int8_t kinds[] {int8_t(punch_mpc_first + 5), int8_t(punch_mpc_first + 1), int8_t(punch_mpc_first + 12),
                               int8_t(punch_mpc_first + 14), int8_t(punch_mpc_first + 9)};
         for (unsigned n = 0; n < 5; ++n) s.ui.engaged[n] = {kinds[n], int8_t(n == 4 ? 9 : -1), n == 1, 100};
         s.ui.engaged_count = 5;
         s.ui.punch_slot = {{kinds[1], kinds[2], kinds[3], kinds[4]}}; s.ui.punch_selected = kinds[4];
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"flexbeat", [](Shell&, Snapshot& s) { // Loop mode, Backspin playing, quantize on
         s.ui.page = PageId::FlexBeat; s.playing = true; s.project.flex.mode = 1; s.project.flex.mix = 80;
         s.flex_playing = 2; s.ui.flex_chosen = 2; s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    // Parity phase 8: the fader.
    {"fader", [](Shell&, Snapshot& s) { // Shift + Pad 9 on the kit (fader C): it follows K2's Filter, still to reach it (Pickup)
         s.ui.page = PageId::FaderPage; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3];
         t.params[sampler::Filter] = 62;
         t.fader_learned = {FaderLearn::Lane, uint16_t(sampler::Filter), uint8_t(t.engine), 1};
         s.ui.fader = {100, false, 1};
     }},
    {"faderstock", [](Shell&, Snapshot& s) { // the wheel walked the kit's eight to Pad Pan on A09
         s.ui.page = PageId::FaderPage; s.ui.track = 3; s.sample = break_sample();
         auto& t = s.project.tracks[3];
         t.fader_target = uint8_t(FaderTarget::PadPan); t.pads[8].pan = -18;
     }},
    {"faderpin", [](Shell&, Snapshot& s) { // a synth's fader pinned to the chorus's mix, learned from Rack's K3
         s.ui.page = PageId::FaderPage; s.ui.track = 2;
         auto& t = s.project.tracks[2];
         t.fader_learned = {FaderLearn::Lane, uint16_t(motion_fx_param(0, 2)), uint8_t(t.engine), 2};
         t.fader_pin = true;
     }},
    {"fadersend", [](Shell&, Snapshot& s) { // the wheel walked a synth's own mix to Send B: its seven dots
         s.ui.page = PageId::FaderPage; s.ui.track = 2;
         auto& t = s.project.tracks[2];
         t.fader_target = uint8_t(FaderTarget::SendB); t.mix.sends[1] = 35;
     }},
    {"faderbend", [](Shell&, Snapshot& s) { // a plug-in track's fader on Pitch Bend, bent a little up
         s.ui.page = PageId::FaderPage; s.ui.track = 2;
         auto& t = s.project.tracks[2];
         t.fader_target = uint8_t(FaderTarget::PitchBend); t.bend = 20;
     }},
    {"fadersynth", [](Shell&, Snapshot& s) { // a synth track with its fader off
         s.ui.page = PageId::FaderPage; s.ui.track = 2;
         auto& t = s.project.tracks[2];
         t.fader_on = false; t.wheel = 70;
     }},
    {"fadercard", [](Shell&, Snapshot& s) { // the fader moved on Tracks: the selected track's level, on K2's card
         s.ui.page = PageId::Tracks; s.ui.track = 2; s.playing = true;
         s.ui.fader = {90, true, 1}; s.ui.fader_moved_at = s.now;
         s.project.tracks[2].mix.level = 90;
         const float meters[] {0.5f, 0.42f, 0.2f, 0, 0.6f, 0.25f, 0.3f, 0.05f, 0.18f, 0.08f};
         for (unsigned t = 0; t < 10; ++t) s.meters[t] = meters[t];
     }},
    {"faderpickup", [](Shell&, Snapshot& s) { // the fader moved on the kit's Pads page: A09's volume on K2, not reached yet
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         s.project.tracks[3].pads[8].level = 100;
         s.ui.fader = {40, false, 1}; s.ui.fader_moved_at = s.now;
     }},
    {"faderheader", [](Shell&, Snapshot& s) { // the fader on Kit Volume, which no card on Pads shows: the header says it
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         s.project.tracks[3].fader_target = uint8_t(FaderTarget::KitVolume);
         s.ui.fader = {100, true, 1}; s.ui.fader_moved_at = s.now;
     }},
    {"faderheaderwait", [](Shell&, Snapshot& s) { // a synth's mod wheel on its Keys page, the fader not there yet
         s.ui.page = PageId::Keys; s.ui.track = 2; s.playing = true;
         s.project.tracks[2].wheel = 70;
         s.ui.fader = {20, false, 1}; s.ui.fader_moved_at = s.now;
     }},
    {"fadersettings", [](Shell&, Snapshot& s) { // Settings: the three takeover rows, the fader's open
         s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 2; s.ui.settings_cursor = Settings::FaderTakeover; s.ui.settings_editing = true;
     }},
    // Parity phase 9: MIDI and sync.
    {"midiconfig", [](Shell&, Snapshot& s) { // Shift + Pad 8: Sync In open on MIDI Clock, MIDI just came in
         s.ui.page = PageId::MidiConfig; s.ui.track = 2; s.playing = true;
         auto& m = s.ui.settings.midi;
         m.sync_in = MidiSync::Clock; m.pad_out = PadMidiOut::Empty; m.in_channel = 1;
         s.project.tracks[2].midi_channel = 3;
         s.ui.midi_cursor = SyncInRow; s.ui.midi_editing = true; s.ui.midi_in_at = s.now;
     }},
    {"midiconfigcv", [](Shell&, Snapshot& s) { // further down: CV/Sync Out on, 24 PPQN, the takeover rows
         s.ui.page = PageId::MidiConfig; s.ui.track = 0;
         auto& m = s.ui.settings.midi;
         m.cv_out = true; m.cv_base = 1; m.cv_division = 24; m.sync_out = MidiSync::Clock;
         s.ui.midi_cursor = CvDivisionRow; s.ui.midi_out_at = s.now;
     }},
    {"midireset", [](Shell&, Snapshot& s) { // Reset Factory Settings asks first: B1 NO, B3 YES
         s.ui.page = PageId::MidiConfig; s.ui.midi_cursor = ResetRow; s.ui.ask = UiState::Ask::ResetSettings;
     }},
    // MIDI tracks (docs/MIDI_TRACKS.md, phase M1).
    {"miditrack", [](Shell&, Snapshot& s) { // Edit on a MIDI track: DEVICE, USB-C on channel 10, the channel just turned
         s.ui.page = PageId::Edit; s.ui.track = 4; s.ui.edit_page[4] = 0; s.playing = true;
         auto& t = s.project.tracks[4];
         load_sound(t, EngineKind::Midi, 0);
         t.params[midi_track::Port] = midi_track::UsbC; t.params[midi_track::Channel] = 10;
         s.ui.midi_outputs = 1 | 2;
         s.ui.turning = 1; s.ui.turned_at = s.now; s.ui.midi_out_at = s.now;
     }},
    {"midiroute", [](Shell&, Snapshot& s) { // the DEVICE picture on All USB with a device plugged in, a note just sent
         s.ui.page = PageId::Edit; s.ui.track = 4; s.ui.edit_page[4] = 0; s.playing = true;
         auto& t = s.project.tracks[4];
         load_sound(t, EngineKind::Midi, 0);
         t.params[midi_track::Port] = midi_track::AllUsb; t.params[midi_track::Channel] = 2;
         s.ui.midi_outputs = 1 | 2 | 1 << 8; s.ui.track_out_at[4] = s.now;
         sent_notes(s, 4);
     }},
    // Roadmap P4 (docs/MIDI_TRACKS.md M2-M5): a MIDI track's device by name gone offline, its slots, its Drum map, its
    // input taken elsewhere, its SysEx going out.
    {"mididevice", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.track = 4; s.ui.edit_page[4] = 0; s.playing = true;
         auto& t = s.project.tracks[4];
         load_sound(t, EngineKind::Midi, 0);
         t.params[midi_track::Port] = midi_track::UsbDevice; t.params[midi_track::Channel] = 1; t.params[midi_track::Delay] = -8;
         std::snprintf(t.midi.device, sizeof(t.midi.device), "%s", "USB Synth");
         std::snprintf(s.ui.usb_devices[0].data(), 32, "%s", "USB Drums");
         s.ui.usb_device_count = 1; s.ui.midi_outputs = 1 | 2 | 1 << 8;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"midislots", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.track = 4; s.ui.edit_page[4] = 3; s.playing = true;
         auto& t = s.project.tracks[4];
         load_sound(t, EngineKind::Midi, 0);
         t.params[midi_track::slot_address(3)] = int16_t(midi_track::address_cc14 + 5);
         t.params[midi_track::slot_address(4)] = int16_t(midi_track::address_nrpn + 300);
         t.params[midi_track::slot_address(5)] = int16_t(midi_track::address_bend);
         t.params[midi_track::slot_value(3)] = 90; t.params[midi_track::slot_value(4)] = 40; t.params[midi_track::slot_value(5)] = 64;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"mididrum", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.track = 4; s.playing = true;
         auto& t = s.project.tracks[4];
         load_sound(t, EngineKind::Midi, 0);
         use_drum_pads(t);
         t.midi.drum_note[2] = 39; t.last_pad = 2;
         s.ui.edit_page[4] = uint8_t(2 + midi_track::slot_pages);
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"midiaudio", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.track = 4; s.playing = true;
         auto& t = s.project.tracks[4];
         load_sound(t, EngineKind::Midi, 0);
         t.params[midi_track::AudioIn] = midi_track::AudioMic;
         s.ui.midi_in_use = 1 << 4;
         s.ui.edit_page[4] = uint8_t(4 + midi_track::slot_pages);
     }},
    {"midisysex", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.track = 4; s.playing = true;
         auto& t = s.project.tracks[4];
         load_sound(t, EngineKind::Midi, 0);
         std::snprintf(t.midi.sysex, sizeof(t.midi.sysex), "%s", "synth-bank.syx");
         t.params[midi_track::SysexOnLoad] = 1;
         s.ui.sysex_percent[4] = 37;
         s.ui.edit_page[4] = uint8_t(5 + midi_track::slot_pages);
     }},
    {"midipanic", [](Shell&, Snapshot& s) { // MIDI Config's MIDI Panic under the cursor
         s.ui.page = PageId::MidiConfig; s.ui.midi_cursor = PanicRow; s.ui.midi_out_at = s.now;
     }},
    {"extclock", [](Shell&, Snapshot& s) { // Patterns while MIDI clock in leads: EXT CLOCK, its tempo
         s.ui.page = PageId::Patterns; s.ui.track = 0; s.playing = true; s.following = true; s.tempo = 1284;
         s.ui.settings.midi.sync_in = MidiSync::Clock;
     }},
    {"extmtc", [](Shell&, Snapshot& s) { // the song while MTC in leads (parity phase 9b): EXT MTC, the song's own tempo
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 25; s.ui.song_view = 16; s.ui.song_zoom = 2;
         s.playing = s.song_playing = true; s.song_bar = 25; s.ticks = 48 * 4;
         s.following = true; s.following_mtc = true;
         s.ui.settings.midi.sync_in = MidiSync::Mtc;
     }},
    {"pad", [](Shell&, Snapshot& s) { s.ui.page = PageId::PadPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.pitch = 3; pad.fine = -8; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"layers", [](Shell&, Snapshot& s) { s.ui.page = PageId::LayersPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         layered_pad(s, LayerPlay::Velocity); s.ui.turning = 1; s.ui.turned_at = s.now; }},
    {"layerscycle", [](Shell&, Snapshot& s) { s.ui.page = PageId::LayersPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         layered_pad(s, LayerPlay::Cycle); s.ui.pad_layer = 2; }},
    {"play", [](Shell&, Snapshot& s) { s.ui.page = PageId::PlayPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.last_pad = 3; t.pads[2].group = 1; t.pads[3].group = 1; t.pads[3].link = 7; t.pads[3].offset = 25; t.pads[3].loop = true;
         s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"envelope", [](Shell&, Snapshot& s) { s.ui.page = PageId::EnvelopePage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.attack = 30; pad.decay = 60; pad.sustain = 55; pad.release = 40; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"filter", [](Shell&, Snapshot& s) { s.ui.page = PageId::FilterPage; s.ui.track = 3; s.playing = true;
         s.project.tracks[3].params[sampler::Filter] = 81; s.project.tracks[3].params[sampler::Resonance] = 34; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"follow", [](Shell&, Snapshot& s) { s.ui.page = PageId::FollowPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.params[sampler::Follow] = 1; t.last_pad = 4; t.pads[4].start = 0; t.pads[4].end = 230087; t.pads[4].own_tempo = 920;
         s.ui.turning = 1; s.ui.turned_at = s.now; }},
    {"cloudplaying", [](Shell&, Snapshot& s) { s.ui.page = PageId::CloudPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.last_pad = 8;
         auto& pad = t.pads[8]; pad.start = 0; pad.end = 230087; pad.cloud = true; pad.cloud_position = 38;
         cloud_heard(s, 230087 * 0.38);
     }},
    {"cloud", [](Shell&, Snapshot& s) { s.ui.page = PageId::CloudPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.last_pad = 4; t.pads[4].cloud = true; t.pads[4].start = 0; t.pads[4].end = 230087; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    // Parity phase 10: the sampler's page map (Push) over Play.
    {"samplermap", [](Shell&, Snapshot& s) { s.ui.page = PageId::PlayPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         s.ui.map_open = true; s.ui.map_cursor = 7; }},
    // Parity phase 4: Sample Mode.
    {"padmix", [](Shell&, Snapshot& s) { s.ui.page = PageId::PadMix; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.pads[8].level = 88; t.pads[8].pan = -18; t.params[sampler::Level] = 110; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"padfilter", [](Shell&, Snapshot& s) { s.ui.page = PageId::PadFilterPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.filter = PadFilter::Lpf4; pad.cutoff = 62; pad.resonance = 55; pad.filter_amount = 40;
         s.ui.turning = 2; s.ui.turned_at = s.now; }},
    {"filterenv", [](Shell&, Snapshot& s) { s.ui.page = PageId::FilterEnvPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.filter = PadFilter::Classic; pad.cutoff = 35; pad.filter_amount = 60;
         pad.filter_attack = 30; pad.filter_decay = 70; pad.filter_sustain = 30; pad.decay_from_start = true; s.ui.turning = 2; s.ui.turned_at = s.now; }},
    {"trimloop", [](Shell&, Snapshot& s) { // a loop with its own point, normalized, Note On and reversed (the icons)
         on_stage(s, 3, 0, 0); s.playing = true; s.sample = break_sample();
         auto& pad = s.project.tracks[3].pads[8]; pad.start = 18000; pad.end = 150000; pad.loop = true; pad.loop_lock = false; pad.loop_start = 60000;
         pad.gain = 420; pad.play = PadPlay::Gate; pad.reverse = true; s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    {"trimshift", [](Shell&, Snapshot& s) { // Shift on Trim: Loop Lock and Normalize on B1 / B2, zoom on K1-K3
         on_stage(s, 3, 0, 0); s.playing = true; s.sample = break_sample(); s.ui.shift = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.start = 18000; pad.end = 150000; pad.loop = true;
     }},
    {"trimzoom", [](Shell&, Snapshot& s) { // Shift + K1 turned: closer in round the start, smoothly (0-100%)
         on_stage(s, 3, 0, 0); s.sample = break_sample(); s.ui.shift = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.start = 18000; pad.end = 150000;
         s.project.samples[pad.sample % sample_slots].frames = s.sample.frames;
         s.ui.trim_zoom = 35; s.ui.trim_focus = 0; s.ui.last_pot = 0; s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"ampenv", [](Shell&, Snapshot& s) { s.ui.page = PageId::EnvelopePage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.attack = 20; pad.decay = 70; pad.velocity_sense = 60; s.ui.turning = 1; s.ui.turned_at = s.now; }},
    {"tune", [](Shell&, Snapshot& s) { s.ui.page = PageId::PadPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.pitch = -2; pad.warp = warp_seq; pad.beats = 8; s.ui.turning = 2; s.ui.turned_at = s.now; }},
    {"levelsfilter", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Pads; s.ui.levels_held = true; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3]; t.levels = true; t.levels_pad = 1; t.last_pad = 1; t.levels_kind = uint8_t(LevelsKind::Filter);
         t.pads[1].cutoff = 70; s.sample.frames = 26460; s.ui.levels_hit = 5;
     }},
    {"levelssynth", [](Shell&, Snapshot& s) { // 16 Levels on a synth: its last note, by Tune
         s.ui.page = PageId::Keys; s.ui.levels_held = true; s.ui.track = 0; s.playing = true;
         auto& t = s.project.tracks[0]; t.levels = true; t.levels_kind = uint8_t(LevelsKind::Tune); s.ui.levels_hit = 3;
     }},
    {"shift", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.shift = true; s.playing = true; }},
    {"shiftkeys", [](Shell&, Snapshot& s) { // Shift held on Keys: the Transpose card; Full Level and Rec Quantize on
         s.ui.page = PageId::Keys; s.ui.shift = true; s.project.tracks[0].transpose = 2;
         s.ui.settings.toggle_full_level(); s.ui.settings.rec_quantize = true;
     }},
    {"shiftkit", [](Shell&, Snapshot& s) { // a kit track on Pads: Save sample, Reverse, Copy pad; metronome on Rec
         s.ui.page = PageId::Pads; s.ui.track = 3; s.ui.shift = true; s.ui.settings.click = 2;
     }},
    {"shifttempo", [](Shell&, Snapshot& s) { // Shift held on Tempo: the project's key
         s.ui.page = PageId::Tempo; s.ui.shift = true; s.project.root = 0; s.project.scale = chromatic_scale;
     }},
    {"levelsoctave", [](Shell&, Snapshot& s) { // 16 Levels by Tune on an instrument's pad, + pressed once: an octave up
         s.ui.page = PageId::Pads; s.ui.levels_held = true; s.ui.track = 3; s.playing = true;
         auto& t = s.project.tracks[3]; t.levels = true; t.levels_pad = 8; t.last_pad = 8; t.levels_kind = uint8_t(LevelsKind::Tune);
         t.octave = int8_t(kit_level_octave + 1);
         choir_pad(s, 8);
         s.ui.levels_hit = 3; s.ui.levels_value = 12;
         std::snprintf(s.ui.notice, sizeof(s.ui.notice), "Octave +1"); s.ui.notice_at = s.now;
     }},
    {"levelsscale", [](Shell&, Snapshot& s) { // 16 Levels up the scale on a kit: K2 the root, K3 the scale, pad 1 the root
         s.ui.page = PageId::Pads; s.ui.levels_held = true; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3]; t.levels = true; t.levels_pad = 1; t.last_pad = 1; t.levels_kind = uint8_t(LevelsKind::Scale);
         s.project.root = 0; s.project.scale = 7; s.ui.levels_hit = 5;
     }},
    {"levelszones", [](Shell&, Snapshot& s) { // Zones on a percussion instrument's pad: its 23 keys, page 1 of 2, pad 3 hit
         s.ui.page = PageId::Pads; s.ui.levels_held = true; s.ui.track = 3; s.playing = true;
         auto& t = s.project.tracks[3]; t.levels = true; t.levels_pad = 8; t.last_pad = 8; t.levels_kind = uint8_t(LevelsKind::Zones);
         t.octave = kit_level_octave;
         auto& ref = s.project.samples[t.pads[8].sample % sample_slots];
         std::snprintf(ref.path, sizeof(ref.path), "/media/card/Samples/Percussion/Percussion.dspreset");
         std::snprintf(ref.name, sizeof(ref.name), "PERCUSSION");
         ref.frames = 0; ref.zones = 67; ref.keys = {};
         for (unsigned key : {48u, 50u, 52u, 53u, 55u, 57u, 59u, 60u, 61u, 62u, 63u, 64u, 65u, 66u, 67u, 68u, 69u, 70u, 71u, 72u, 74u, 76u, 77u})
             ref.keys[key / 64] |= uint64_t(1) << (key % 64);
         s.ui.levels_hit = 2; s.ui.levels_value = 52;
     }},
    {"levelsscalesynth", [](Shell&, Snapshot& s) { // and on a synth: D Dorian from D3 in the track's octave, pad 10 hit
         s.ui.page = PageId::Keys; s.ui.levels_held = true; s.ui.track = 0; s.playing = true;
         auto& t = s.project.tracks[0]; t.levels = true; t.levels_kind = uint8_t(LevelsKind::Scale); t.octave = 3;
         s.project.root = 2; s.project.scale = 2; s.ui.levels_hit = 9;
     }},
    {"exit", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.exit_open = true; }},
    // Updates from the card (shell/update.h): question, install in progress, failed install.
    {"updateask", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.ui.project_cursor = 0; preview_projects(s);
         s.ui.ask = UiState::Ask::InstallUpdate; std::snprintf(s.ui.update.version, sizeof s.ui.update.version, "v0.2.0");
     }},
    {"updating", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; preview_projects(s);
         s.ui.update.phase = uint8_t(UpdatePhase::Writing); std::snprintf(s.ui.update.version, sizeof s.ui.update.version, "v0.2.0");
     }},
    {"updatefailed", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; preview_projects(s);
         s.ui.update.phase = uint8_t(UpdatePhase::Failed); s.ui.update.error = uint8_t(UpdateError::CardFull);
         std::snprintf(s.ui.update.version, sizeof s.ui.update.version, "v0.2.0");
     }},
    {"loading", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.loading = true; s.ui.loading_total = s.ui.loading_left = 40; std::strncpy(s.project.name, "PROJECT 1", sizeof s.project.name - 1); }},
    {"loadingsamples", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.loading = true; s.ui.loading_total = 40; s.ui.loading_left = 12; std::strncpy(s.project.name, "PROJECT 1", sizeof s.project.name - 1); }},
    {"exitcard", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.exit_open = true; s.projects.root = "/media/card/Melodee/Projects"; s.project_path = "/media/card/Melodee/Projects/Song.mldp"; }},
    {"exitoff", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.exit_open = s.ui.exit_off = true; s.projects.root = "/media/card/Melodee/Projects"; }},
    {"exitsaved", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.exit_open = s.ui.exit_off = s.ui.exit_saved = true; s.projects.root = "/media/card/Melodee/Projects"; }},
    {"name", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.song_cursor = 16; s.playing = true;
         s.ui.name.start(NameTarget::Marker, "CHORUS", 11, true);
         s.ui.name.pad(1, s.now); // C: the letter just typed, its pad lit
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"namesound", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Sounds; s.ui.track = 2; s.playing = true;
         s.ui.name.start(NameTarget::Sound, "PLUCK", name_length - 1, true);
         s.ui.name.digits = true; s.ui.name.insert(' '); s.ui.name.pad(1, s.now - 2000);
     }},
    {"savesample", [](Shell&, Snapshot& s) { // Shift + Sample Select on a kit: Save Sample's Name sheet
         on_stage(s, 3, 0, 0); s.sample = break_sample();
         s.ui.name.start(NameTarget::Sample, "BREAK", name_length - 1, false);
     }},
    {"nameproject", [](Shell&, Snapshot& s) { // Save on a project with no file yet: a name first (QUICKSTART.md)
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_cursor = 4;
         preview_projects(s);
         s.ui.name.start(NameTarget::ProjectSave, "Project 1", sizeof(s.project.name) - 1, false);
     }},
    // Parity phase 3. These come last: they write into the shell's patterns, which the previews before them share.
    {"rollkit", [](Shell& shell, Snapshot& s) { // Roll on a kit: a lane per pad, pad 3's hit at 1.2.1 in focus
         step_edit_kit(shell, s);
         s.ui.page = PageId::Roll; s.ui.roll_key = uint8_t(sampler_first_note + 2); s.ui.roll_top = uint8_t(sampler_first_note + 11);
     }},
    {"stepedit", [](Shell& shell, Snapshot& s) { // Shift + Seq (step entry): the caret on the kit's step 5, what it holds
         step_edit_kit(shell, s);
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"stepentrychord", [](Shell& shell, Snapshot& s) { // a chord going in at the caret: A01 and A03 held, a fixed velocity
         step_edit_kit(shell, s);
         s.ui.step_cursor = 6; s.ui.entry_velocity = 100; s.ui.entry_length = 2;
         s.ui.entry_held = 0b101; s.held_keys[0] = int16_t(sampler_first_note); s.held_keys[2] = int16_t(sampler_first_note + 2);
     }},
    {"rollnudge", [](Shell& shell, Snapshot& s) { // Roll, Shift held: NUDGE on K1, the note in focus a little late
         step_edit_kit(shell, s);
         s.ui.page = PageId::Roll; s.ui.step_cursor = 4; s.ui.roll_key = uint8_t(sampler_first_note + 1);
         s.ui.roll_top = uint8_t(sampler_first_note + 11); s.ui.shift = true;
     }},
    {"erasepad", [](Shell& shell, Snapshot& s) { // Erase + pad 1 on Step Edit: B1 Cancel, B3 Do it
         step_edit_kit(shell, s);
         s.ui.ask = UiState::Ask::EraseKeyEvents; s.ui.ask_target = int(sampler_first_note);
     }},
    {"padmotion", [](Shell& shell, Snapshot& s) { // Motion on a kit: the last pad's level and tune, recorded per pad
         pad_motion_kit(shell, s);
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"eraselane", [](Shell& shell, Snapshot& s) { // Erase + K1 on Motion: the pad lane goes once B3 says so
         pad_motion_kit(shell, s);
         s.ui.ask = UiState::Ask::EraseLane; s.ui.ask_target = motion_lane(motion_pad_param(PadParam::Level), 8);
     }},
    // Roll R1 (shell/roll.h): track 1's pattern rewritten, so last of all.
    {"roll", [](Shell& shell, Snapshot& s) { // the chord at 2.2.3, C4 in focus, K2 turning
         roll_bass(shell, s, 22, 60);
         s.playing = true; s.position = 26 * sixteenth_ticks; s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"notesheet", [](Shell& shell, Snapshot& s) { // Push on the note in focus: Accent, Slide, Tie, Delete as cards
         roll_bass(shell, s, 24, 57);
         s.ui.sheet_open = true;
     }},
    {"rolledges", [](Shell& shell, Snapshot& s) { // the rows lower: notes above and below are dots on the edges
         roll_bass(shell, s, 30, 55);
         s.ui.roll_top = 63;
     }},
    // Roadmap P1 (drums and sequencer): these rewrite the shell's patterns too.
    {"rollratchet", [](Shell& shell, Snapshot& s) { // Roll's TRIG row (B2 again): the note in focus a ratchet of four, ramped down
         roll_bass(shell, s, 24, 57);
         auto& p = shell.patterns().at[0][s.project.tracks[0].pattern % pattern_count];
         for (auto& e : p.events) if (e.tick == 24 * sixteenth_ticks && e.key == 57) { e.ratchet = 4; e.ramp = -50; }
         for (auto& e : p.events) if (e.tick == 34 * sixteenth_ticks) { e.ratchet = 3; e.ramp = 60; }
         s.ui.trig_cards = true; s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"rollmarks", [](Shell& shell, Snapshot& s) { // Roll's selection: four notes marked by Shift + wheel, the last two muted
         roll_bass(shell, s, 24, 57);
         auto& p = shell.patterns().at[0][s.project.tracks[0].pattern % pattern_count];
         s.ui.roll_marked = 0; s.ui.roll_mark_track = 0; s.ui.roll_mark_pattern = s.project.tracks[0].pattern;
         for (auto& e : p.events) {
             if (e.tick < 24 * sixteenth_ticks || s.ui.roll_marked >= 4) continue;
             if (s.ui.roll_marked >= 2) e.flags |= step_mute;
             s.ui.roll_marks[s.ui.roll_marked++] = {e.tick, e.key, e.pitch};
         }
     }},
    {"stepsratchet", [](Shell& shell, Snapshot& s) { // Steps' TRIG row on the kit, the hat's lane: step 5 a ratchet of three rising, Shift held
         step_edit_kit(shell, s);
         s.ui.page = PageId::StepEdit; s.ui.step_pads = true; s.project.tracks[3].last_pad = 2;
         for (auto& e : shell.patterns().at[3][0].events) if (e.tick == 96 && e.key == sampler_first_note + 2) { e.ratchet = 3; e.ramp = 40; e.pitch_ramp = 5; }
         for (auto& e : shell.patterns().at[3][0].events) if (e.tick == 144) { e.ratchet = 2; }
         s.ui.step_cards = 2; s.ui.shift = true;
     }},
    {"stepstrig", [](Shell& shell, Snapshot& s) { // Steps' TRIG row: K3 turning a trig condition, the hat at step 5 on 3:4, step 3 FILL
         step_edit_kit(shell, s);
         s.ui.page = PageId::StepEdit; s.ui.step_pads = true; s.project.tracks[3].last_pad = 2;
         for (auto& e : shell.patterns().at[3][0].events) {
             if (e.tick == 96 && e.key == sampler_first_note + 2) { e.ratchet = 3; e.ramp = 40; e.condition = uint8_t(trig_of(3, 4)); }
             if (e.tick == 48) e.condition = uint8_t(trig_fill);
         }
         s.ui.step_cards = 2; s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    {"stepslock", [](Shell& shell, Snapshot& s) { // step 5 held on the kit's hat lane: the cards lock the hat's level, tune and pan there
         step_edit_kit(shell, s);
         s.ui.page = PageId::StepEdit; s.ui.step_pads = true; s.project.tracks[3].last_pad = 2; s.ui.held_step = 4;
         auto& p = shell.patterns().at[3][0];
         auto lock = [&](uint32_t tick, PadParam which, int value) {
             MotionPoint m;
             m.tick = tick; m.param = uint8_t(motion_pad_param(which)); m.pad = 2; m.value = int16_t(value); m.hold = 24;
             p.write_motion(m);
         };
         lock(96, PadParam::Pitch, 5); lock(96, PadParam::Level, 70); lock(144, PadParam::Pitch, -3);
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"motionlocks", [](Shell& shell, Snapshot& s) { // Motion with locks: the hat's tune recorded, two steps locked over it
         pad_motion_kit(shell, s);
         auto& p = shell.patterns().at[3][0];
         for (auto [tick, value] : {std::pair<uint32_t, int>{3 * sixteenth_ticks, 12}, {11 * sixteenth_ticks, -9}}) {
             MotionPoint m;
             m.tick = tick; m.param = uint8_t(motion_pad_param(PadParam::Pitch)); m.pad = 8; m.value = int16_t(value); m.hold = sixteenth_ticks;
             p.write_motion(m);
         }
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"stepseuclid", [](Shell& shell, Snapshot& s) { // STEPS a third time on the kit: the hat's lane as E(5,8), K1 turning its hits
         step_edit_kit(shell, s);
         s.ui.page = PageId::StepEdit; s.ui.step_pads = true; s.project.tracks[3].last_pad = 2;
         lay_euclid(shell.patterns().at[3][0], uint8_t(sampler_first_note + 2), 5, 8, 0, 90);
         s.ui.step_cards = 3; s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"patternsvary", [](Shell& shell, Snapshot& s) { // Shift + B3 on Patterns: VARY, Busier picked, amount 30 %
         step_edit_kit(shell, s);
         s.ui.page = PageId::Patterns; s.ui.sheet_open = true; s.ui.sheet_row = 1;
     }},
    {"tracksspeed", [](Shell&, Snapshot& s) { // Shift on Tracks: track 3 plays its patterns at x1 1/2
         s.ui.page = PageId::Tracks; s.ui.track = 2; s.playing = true; s.project.tracks[2].speed = 3;
         s.ui.shift = true;
     }},
    {"repeatrates", [](Shell&, Snapshot& s) { // Note Repeat on the kit's Pads at 1/64: B3 pressed again (dots under B1 and B3)
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true;
         s.project.tracks[3].arp = true; s.project.tracks[3].arp_rate = 8;
     }},
    {"stepslane", [](Shell& shell, Snapshot& s) { // Shift + K1 on the kit's Steps: the hat's lane loops over six steps of the bar's sixteen
         step_edit_kit(shell, s);
         s.ui.page = PageId::StepEdit; s.ui.step_pads = true; s.project.tracks[3].last_pad = 2;
         auto& p = shell.patterns().at[3][0];
         p.set_lane_length(sampler_first_note + 2, 6 * sixteenth_ticks);
         s.position = 9 * sixteenth_ticks; s.pass = 0; // the hat's lane at its step 4 while the pattern is at 10
         s.ui.shift = true; s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    // Roadmap P5: scenes on Patterns. Shift + B1 SCENE: the cursor's scene, its tempo on K1 (Push names it).
    {"patternsscene", [](Shell& shell, Snapshot& s) {
         s.ui.page = PageId::Patterns; s.ui.track = 2; s.ui.scene = 1; s.playing = true; s.ui.patterns_cards = 1;
         s.project.scenes[0].set_name("INTRO"); s.project.scenes[1].set_name("VERSE"); s.project.scenes[1].tempo = 1240;
         s.project.scenes[2].set_name("DROP"); s.project.scenes[2].tempo = 1280; s.project.scenes[5].set_name("BREAKDOWN");
         s.project.tracks[2].pattern = 1;
         for (unsigned t = 0; t < track_count; ++t) { s.playing_patterns[t] = t == 2 ? 1 : 0; s.positions[t] = t == 5 ? -1 : int(t * sixteenth_ticks); }
         auto& f = shell.patterns().at[2][1].follow;
         f.action = uint8_t(FollowAction::Next); f.other = uint8_t(FollowAction::Any); f.chance = 75; f.count = 4;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"patternsfollow", [](Shell& shell, Snapshot& s) { // Shift + B2 FOLLOW: the pattern in hand's follow action on K1-K3
         s.ui.page = PageId::Patterns; s.ui.track = 2; s.ui.scene = 1; s.playing = true; s.ui.patterns_cards = 2;
         s.project.tracks[2].pattern = 1;
         auto& f = shell.patterns().at[2][1].follow;
         f.action = uint8_t(FollowAction::Next); f.other = uint8_t(FollowAction::Any); f.chance = 75; f.count = 4;
         s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"patternsshift", [](Shell& shell, Snapshot& s) { // Shift held on Patterns: SCENE, FOLLOW, VARY; FOLLOW's Shift + K1 the second action
         s.ui.page = PageId::Patterns; s.ui.track = 2; s.ui.scene = 1; s.ui.shift = true; s.ui.patterns_cards = 2; s.project.tracks[2].pattern = 1;
         auto& f = shell.patterns().at[2][1].follow;
         f.action = uint8_t(FollowAction::Again); f.other = uint8_t(FollowAction::Stop); f.chance = 50; f.count = 2; f.plays = true;
     }},
    // Roadmap P5: SCENES (B1 again on Patterns), the bank's scenes and the selected track's pattern's follow action.
    {"scenes", [](Shell& shell, Snapshot& s) {
         s.ui.page = PageId::Scenes; s.ui.track = 2; s.ui.scene = 1; s.playing = true;
         s.project.scenes[0].set_name("INTRO"); s.project.scenes[1].set_name("VERSE"); s.project.scenes[1].tempo = 1240;
         s.project.scenes[2].set_name("DROP"); s.project.scenes[2].tempo = 1280; s.project.scenes[5].set_name("BREAK");
         s.project.tracks[2].pattern = 1;
         for (unsigned t = 0; t < track_count; ++t) { s.playing_patterns[t] = t == 2 ? 1 : 0; s.positions[t] = t == 5 ? -1 : int(t * sixteenth_ticks); }
         auto& f = shell.patterns().at[2][1].follow;
         f.action = uint8_t(FollowAction::Next); f.other = uint8_t(FollowAction::Any); f.chance = 75; f.count = 4;
     }},
    {"stepeditshift", [](Shell& shell, Snapshot& s) { // Shift held on Step Edit: PADS, TIE, INSERT; the pattern's length, Q, time signature
         step_edit_kit(shell, s);
         s.ui.shift = true;
     }},
};

void draw(juce::Graphics& g, const Snapshot& s) {
    ui::Paint p(g);
    g.fillAll(ui::colour::bg);
    page_spec(s.ui.page).draw(p, s);
    draw_overlays(p, s);
}

int preview(const std::string& folder, const std::vector<std::string>& names) {
    EngineHost host;
    Shell shell(host);
    board_project(shell);
    unsigned written = 0, text_issues = 0;
    if (names.empty()) { // every page: first the PNGs of pages that are gone, so no removed screen lingers among them
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
            auto stem = entry.path().stem().string();
            if (entry.path().extension() != ".png") continue;
            if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, "@2x") == 0) stem.resize(stem.size() - 3);
            bool known = std::any_of(std::begin(previews), std::end(previews), [&](const auto& p) { return stem == p.name; });
            if (!known) std::filesystem::remove(entry.path(), error);
        }
    }
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
            std::vector<ui::TextIssue> issues;
            ui::audit_text(scale == 1 ? &issues : nullptr);
            {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(float(scale)));
                draw(g, *snapshot);
            }
            ui::audit_text(nullptr);
            for (const auto& i : issues) {
                std::printf("MELODEE_TEXT %s \"%s\" size %.1f width %.0f in %.0f\n", preview.name,
                            i.text.toRawUTF8(), double(i.size), double(i.needed), double(i.box));
                ++text_issues;
            }
            save_png(image, folder + "/" + preview.name + (scale == 2 ? "@2x" : "") + ".png");
        }
        ++written;
    }
    require(written > 0, "Unknown Melodee page; use stage stagemix stagemidi stageemptysynth tracks keys sounds edit mix steps roll notesheet motion patterns recquantize recquantizeoff seqsheet seqbpm seqclear rollkit song songzoom songclip songshift length rack sends bus fxpick pads samples trim chop chopfine levels record project filesheet tempo arp chord capture pad play envelope filter follow cloud padmix padfilter filterenv trimloop trimshift trimzoom ampenv tune levelsfilter levelssynth shift exit exitcard exitoff exitsaved");
    std::cout << "MELODEE_PREVIEW pages=" << written << " text_issues=" << text_issues << '\n';
    // Text under the floor or cut by its box fails the build check.
    return text_issues ? 1 : 0;
}

// The boot animation at 30 frames a second, fading out over the starting
// project's first page, as DIR/boot-NNN.png.
int boot_frames(const std::string& folder) {
    EngineHost host;
    Shell shell(host);
    board_project(shell);
    auto snapshot = std::make_unique<Snapshot>();
    shell.snapshot(*snapshot, 100000);
    snapshot->ui = UiState {};
    snapshot->now = 100000;
    const auto& track = snapshot->project.tracks[snapshot->ui.track];
    snapshot->pattern = shell.patterns().at[snapshot->ui.track][track.pattern];
    ui::BootAnimation boot;
    unsigned written = 0;
    for (uint32_t ms = 0;; ms += 1000 / 30) {
        juce::Image image(juce::Image::ARGB, screen_width, screen_height, true);
        {
            juce::Graphics g(image);
            if (boot.covers(ms)) boot.draw(g, ms);
            else { draw(g, *snapshot); boot.draw(g, ms); }
        }
        char name[32];
        std::snprintf(name, sizeof name, "/boot-%03u.png", written++);
        save_png(image, folder + name);
        if (boot.finished(ms)) break;
    }
    std::cout << "MELODEE_BOOT frames=" << written << '\n';
    return written > 1 ? 0 : 1;
}

// Draws every preview page `rounds` times at 320 x 240 and prints the mean
// time per draw, for comparing drawing cost before and after a change.
int bench(unsigned rounds, const std::vector<std::string>& names) {
    EngineHost host;
    Shell shell(host);
    board_project(shell);
    juce::Image image(juce::Image::ARGB, screen_width, screen_height, true);
    double total = 0;
    unsigned pages = 0;
    for (const auto& preview : previews) {
        if (!names.empty() && std::find(names.begin(), names.end(), preview.name) == names.end()) continue;
        auto snapshot = std::make_unique<Snapshot>();
        shell.snapshot(*snapshot, 100000);
        snapshot->ui = UiState {};
        snapshot->now = 100000;
        preview.set(shell, *snapshot);
        const auto& track = snapshot->project.tracks[snapshot->ui.track];
        snapshot->pattern = shell.patterns().at[snapshot->ui.track][track.pattern];
        { juce::Graphics g(image); draw(g, *snapshot); } // warm caches
        auto start = std::chrono::steady_clock::now();
        for (unsigned n = 0; n < rounds; ++n) { juce::Graphics g(image); draw(g, *snapshot); }
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / rounds;
        std::printf("MELODEE_BENCH %-10s %8.0f us\n", preview.name, us);
        total += us; ++pages;
    }
    require(pages > 0, "Unknown Melodee page");
    std::printf("MELODEE_BENCH mean       %8.0f us over %u pages\n", total / pages, pages);
    return 0;
}

void little(std::ofstream& file, uint32_t value, unsigned bytes) {
    for (unsigned n = 0; n < bytes; ++n) file.put(char((value >> (n * 8)) & 255));
}

// Two bars of the starter project at 120 BPM (4 s), then two seconds of
// tail: its beat must sound, stay below full scale and end in silence. The
// kit plays three drums made here (its synth tracks are plug-ins, not loaded
// in the self-test): a falling sine kick, a noise snare and a short hat.
int sound(const std::string& path) {
    EngineHost host;
    Shell shell(host);
    starter_project(shell.project(), shell.patterns());
    host.load(shell.project(), shell.patterns());
    std::array<SampleData, 3> drums;
    uint32_t seed = 1;
    auto noise = [&] { seed = seed * 1664525u + 1013904223u; return int32_t(seed) * (1.0f / 2147483648.0f); };
    for (unsigned d = 0; d < drums.size(); ++d) {
        auto& data = drums[d];
        data.frames = 22050;
        data.left.resize(data.frames); data.right.resize(data.frames);
        double phase = 0;
        for (unsigned n = 0; n < data.frames; ++n) {
            double t = n / 44100.0;
            float x = 0;
            if (d == 0) { phase += 2 * 3.14159265358979 * (50 + 120 * std::exp(-t * 30)) / 44100.0; x = float(std::sin(phase) * std::exp(-t * 9)); }
            else if (d == 1) x = float(noise() * 0.6 * std::exp(-t * 18) + std::sin(n * 0.04) * 0.3 * std::exp(-t * 25));
            else x = float(noise() * 0.4 * std::exp(-t * 60));
            data.left[n] = data.right[n] = x * 0.8f;
        }
        require(host.offer_sample(d, &data), "Offer the sound check's drums");
    }
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

// A multisampled instrument as the sample library reads it, its samples found on the card or disk.
static int instrument_read(const char* path) {
    std::string error;
    auto read = read_instrument(path, &error);
    if (!error.empty()) { std::printf("error=%s\n", error.c_str()); return 1; }
    std::string ignored, left_by;
    for (const auto& i : read.ignored) ignored += i + ",";
    for (const auto& i : read.left_by) left_by += i + ",";
    const auto& e = read.envelope;
    std::printf("zones=%zu left=%u left_by=%s velocity=%.0f ignored=%s attack=%.3f decay=%.3f sustain=%.1f release=%.3f\n", read.regions.size(), read.left,
                left_by.c_str(), read.velocity, ignored.c_str(), e.attack.value_or(-1), e.decay.value_or(-1), e.sustain.value_or(-1), e.release.value_or(-1));
    for (const auto& r : read.regions) {
        const auto& z = r.zone;
        std::printf("  keys %u-%u root %u vel %u-%u tune %d track %d gain %.1f pan %.2f from %lld to %lld loop %d %u-%u one_shot %d seq %u/%u choke %u %s\n", z.low_key,
                    z.high_key, z.root, z.low_velocity, z.high_velocity, z.tune, z.keytrack, z.gain, z.pan, (long long)r.offset, (long long)r.end, int(z.loop),
                    z.loop_start, z.loop_end, z.one_shot, z.sequence, z.sequence_length, z.off_by, r.sample.c_str());
    }
    return 0;
}

// What an MPC Sample project or an MPC drum program becomes, for the build's checks and a look at a real file.
static int mpc_import(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    if (is_mpc_program(path) && !is_drum_program(path)) { // a keygroup program: one pad's instrument
        KeygroupProgram program;
        auto error = read_program(path, std::string(bytes.begin(), bytes.end()), program);
        if (!error.empty()) { std::printf("error=%s\n", error.c_str()); return 1; }
        auto read = read_keygroup(program);
        std::string ignored;
        for (const auto& i : read.ignored) ignored += i + " ";
        std::printf("keygroup name=%s zones=%zu left=%u velocity=%.0f ignored=%s preview=%s\n", program.tags["ProgramName"].c_str(), read.regions.size(),
                    read.left, read.velocity, ignored.c_str(), program_preview(path).c_str());
        unsigned missing = 0;
        for (const auto& r : read.regions) {
            bool there = std::filesystem::exists(std::filesystem::path(path).parent_path() / r.sample);
            missing += !there;
            std::printf("  keys %u-%u root %u vel %u-%u tune %d gain %.1f end %lld %s%s\n", r.zone.low_key, r.zone.high_key, r.zone.root, r.zone.low_velocity,
                        r.zone.high_velocity, r.zone.tune, r.zone.gain, (long long)r.end, r.sample.c_str(), there ? "" : " MISSING");
        }
        std::printf("missing=%u\n", missing);
        return 0;
    }
    auto project = std::make_unique<Project>();
    auto patterns = std::make_unique<PatternBank>();
    MpcImport report;
    auto began = std::chrono::steady_clock::now();
    const std::string kind = std::filesystem::path(path).extension().string();
    auto error = kind == ".xpm" || kind == ".XPM" ? import_mpc_program(bytes, path, *project, report)
                                                  : import_mpc_project(bytes, path, *project, *patterns, report);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    if (!error.empty()) { std::printf("error=%s\n", error.c_str()); return 1; }
    std::printf("name=%s tracks=%u sequences=%u pads=%u notes=%u over_limit=%u missing=%u tempo=%u seq_tempo=%d signature=%u/%u ms=%lld\n", project->name, report.tracks,
                report.sequences, report.pads, report.notes, report.over_limit, report.missing, project->tempo, project->seq_tempo,
                project->time_signature().beats, project->time_signature().unit, (long long)ms);
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& track = project->tracks[t];
        if (!track.used()) continue;
        std::printf("track %u name=%s level=%u pan=%d mute=%d pattern=%u\n", t, track.name, track.mix.level, track.mix.pan, track.mix.mute, track.pattern);
        for (unsigned n = 0; n < sampler_pads; ++n) {
            const auto& pad = track.pads[n];
            if (!pad.used()) continue;
            char label[4];
            pad_label(n, label);
            std::printf("pad %s file=%s start=%u end=%u level=%u pan=%d pitch=%d play=%d mono=%d group=%u filter=%s cutoff=%u warp=%u attack=%u decay=%u loop=%d colour=%06x\n", label, project->samples[pad.sample].name,
                        pad.start, pad.end, pad.level, pad.pan, pad.pitch, int(pad.play), pad.mono, pad.group, pad_filter_name(pad.filter), pad.cutoff, pad.warp, pad.attack, pad.decay, pad.loop, unsigned(pad.colour));
            const auto& ref = project->samples[pad.sample];
            std::string file, which;
            if (ref.instrument() && split_program_pad(ref.path, file, which)) { // its layers, as the sample library will read them
                std::string why;
                auto pads = program_pads(file, why);
                auto found = pads.find(which);
                if (found == pads.end()) { std::printf("  layers: not found %s\n", why.c_str()); continue; }
                auto read = read_drum_pad(found->second);
                std::printf("  layers=%zu", read.regions.size());
                for (const auto& r : read.regions)
                    std::printf(" [%s vel %u-%u seq %u/%u gain %.1f end %lld%s]", r.sample.c_str(), r.zone.low_velocity, r.zone.high_velocity,
                                r.zone.sequence, r.zone.sequence_length, r.zone.gain, (long long)r.end,
                                std::filesystem::exists(std::filesystem::path(file).parent_path() / r.sample) ? "" : " MISSING");
                std::printf("\n");
            } else std::printf("  path=%s%s\n", ref.path, std::filesystem::exists(ref.path) ? "" : " MISSING");
        }
        for (unsigned p = 0; p < pattern_count; ++p) {
            const auto& pattern = patterns->at[t][p];
            if (pattern.events.empty()) continue;
            std::printf("pattern %u %u length=%u notes=%zu first=%u:%u:%u\n", t, p, pattern.length, pattern.events.size(), pattern.events[0].tick, pattern.events[0].key, pattern.events[0].velocity);
        }
    }
    return 0;
}

std::optional<int> run_tool(int argc, char** argv) {
    if (argc >= 2 && !std::strcmp(argv[1], "--melodee-monkey")) { // [SEED COUNT MOVES], build.sh's: 1 12 3000
        auto arg = [&](int n, unsigned fallback) { return argc > n ? unsigned(std::strtoul(argv[n], nullptr, 10)) : fallback; };
        return monkey(arg(2, 1), arg(3, 12), arg(4, 3000));
    }
    if (argc >= 3 && !std::strcmp(argv[1], "--melodee-preview")) {
        std::vector<std::string> names(argv + 3, argv + argc);
        return preview(argv[2], names);
    }
    // --self-test is what device.py and the SD boot run before stopping the stock app.
    if (argc == 3 && (!std::strcmp(argv[1], "--melodee-sound") || !std::strcmp(argv[1], "--self-test"))) return sound(argv[2]);
    if (argc == 3 && !std::strcmp(argv[1], "--mpc-import")) return mpc_import(argv[2]);
    if (argc == 3 && !std::strcmp(argv[1], "--instrument-read")) return instrument_read(argv[2]);
    if (argc == 4 && !std::strcmp(argv[1], "--mpc-install")) { // an expansion into DIR/<its title>, as Push on one does
        ExpansionInstall install;
        auto error = install.open(argv[2], argv[3]);
        while (error.empty() && install.step(error)) {}
        std::printf("%s folder=%s files=%u/%u\n", error.empty() ? "installed" : ("error=" + error).c_str(), install.folder().c_str(), install.done(), install.total());
        return error.empty() ? 0 : 1;
    }
    if (argc == 3 && !std::strcmp(argv[1], "--melodee-boot")) return boot_frames(argv[2]);
    if (argc >= 2 && !std::strcmp(argv[1], "--melodee-dsp-bench")) return dsp_bench(argc, argv);
    if (argc >= 2 && !std::strcmp(argv[1], "--melodee-print-spike")) return print_spike(argc, argv);
    if (argc >= 3 && !std::strcmp(argv[1], "--melodee-bench")) {
        std::vector<std::string> names(argv + 3, argv + argc);
        return bench(unsigned(std::max(1, std::atoi(argv[2]))), names);
    }
    return std::nullopt;
}

} // namespace melodee
