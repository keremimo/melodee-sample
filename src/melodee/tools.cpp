// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/tools.h"
#include "melodee/shell/expansion_install.h"
#include "melodee/shell/mpc_program.h"
#include "melodee/shell/mpc_project.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/seq_layer.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/prophet_params.h"
#include "melodee/engine/kit909.h"
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

// A full band for the previews and the sound check: four Prophet tracks, an
// empty kit and a 909, a bass line in D and a beat.
void starter_project(Project& project, PatternBank& patterns) {
    project = Project {};
    std::snprintf(project.name, sizeof(project.name), "Starter");
    project.tempo = 1200;
    load_sound(project.tracks[0], EngineKind::Prophet, 1);  // FUNK BASS II
    load_sound(project.tracks[1], EngineKind::Prophet, 5);  // SOFT PAD
    load_sound(project.tracks[2], EngineKind::Prophet, 6);  // GLASS PLUCK
    load_sound(project.tracks[3], EngineKind::Prophet, 3);  // SYNC LEAD
    load_sound(project.tracks[4], EngineKind::Sampler, 0); // an empty kit
    load_sound(project.tracks[5], EngineKind::Kit909, 0);  // drums, with a beat
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
    // 909 keys (engine/kit909.h): 36 kick, 37 snare, 43 closed hat.
    auto& beat = patterns.at[5][0];
    for (unsigned st = 0; st < 16; st += 4) note(beat, st, 36, 120, 1);
    for (unsigned st : {4u, 12u}) note(beat, st, 37, 110, 1);
    for (unsigned st = 2; st < 16; st += 4) note(beat, st, 43, 90, 1);
}

void board_project(Shell& shell) {
    starter_project(shell.project(), shell.patterns()); // the previews show a full project, not a new one
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
    auto point = [&](unsigned sixteenth, unsigned param, int value) {
        MotionPoint m;
        m.tick = sixteenth * sixteenth_ticks; m.param = uint8_t(param); m.value = int16_t(value);
        shell.patterns().at[0][1].write_motion(m);
    };
    for (unsigned n = 0; n < 16; ++n) point(n * 4, prophet::Cutoff, cutoff[n]);
    const int reso[] {40, 52, 60, 52, 40, 35};
    for (unsigned n = 0; n < 6; ++n) point(24 + n * 4, prophet::Resonance, reso[n]);
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

// Track 3's pad `pad` holding a Logic EXS24 instrument: 22 zones over C0..C7 at
// every velocity, all its files read, Note On as a loaded instrument is.
void choir_pad(Snapshot& s, unsigned pad) {
    auto& sound = s.project.tracks[3].pads[pad % sampler_pads];
    auto& ref = s.project.samples[sound.sample % sample_slots];
    std::snprintf(ref.path, sizeof(ref.path), "/media/card/Samples/Boys Choir/Boys Choir.exs");
    std::snprintf(ref.name, sizeof(ref.name), "BOYS CHOIR");
    ref.frames = 0;
    sound.play = PadPlay::Gate; sound.start = sound.end = 0;
    LoadedSample m;
    m.name = ref.name; m.path = ref.path; m.instrument = true; m.ready = true;
    m.zones = 22; m.parts = 22; m.parts_ready = 22; m.low_key = 24; m.high_key = 108;
    for (unsigned key = 24; key <= 108; ++key) m.map[key] = 0xffff;
    s.sample = m;
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

// The Project board's card: two folders and four projects, Night drive open with changes.
void preview_projects(Snapshot& s) {
    auto& v = s.projects;
    v.root = "/media/card/Melodee/Projects"; v.free_bytes = 21400000000ull;
    using K = ProjectEntry::Kind;
    v.entries = {{K::Folder, "Live sets", v.root + "/Live sets", 0, 0, 6, 0}, {K::Folder, "Sketches", v.root + "/Sketches", 0, 0, 14, 0},
                 {K::Project, "Night drive", v.root + "/Night drive.mldp", 10, 1240, 0, 4}, {K::Project, "Tape loops", v.root + "/Tape loops.mldp", 7, 920, 0, 3},
                 {K::Project, "Sunday sketch", v.root + "/Sunday sketch.mldp", 3, 1100, 0, 2}, {K::Project, "Basement jam", v.root + "/Basement jam.mldp", 12, 1400, 0, 1}};
    v.folders = {"Live sets", "Live sets/Spring tour", "Sketches", "Sketches/2026"};
    s.project_path = v.root + "/Night drive.mldp";
    s.dirty = true;
    s.ui.project_rows = uint16_t(project_rows(v.entries.size(), s.project_folder)); // as the shell copies it in
}

// Step Edit on the kit (track 4): step 5 of pattern 1 holds a kick, a hat and
// a snare played a little late; the hat is chosen.
void step_edit_kit(Shell& shell, Snapshot& s) {
    s.ui.page = PageId::StepEdit; s.ui.track = 3; s.playing = true; s.position = 4 * sixteenth_ticks + 9;
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

void on_stage(Snapshot& s, unsigned track, unsigned group, unsigned sub) {
    s.ui.page = PageId::Stage; s.ui.track = uint8_t(track);
    s.ui.stage_group[track] = uint8_t(group); s.ui.stage_sub[track] = uint8_t(sub);
}
// Stage on track 11, a 909 (BOOM KIT), its snare and clap last hit.
void stage_909(Snapshot& s, unsigned group, unsigned sub) {
    auto& t = s.project.tracks[10];
    load_sound(t, EngineKind::Kit909, 2);
    t.last_note = 36 + kit909::Clap; t.mix.level = 96; t.mix.sends[0] = 20;
    on_stage(s, 10, group, sub); s.playing = true;
}
// Stage on track 12, a MIDI track to a synth on USB-C, channel 3.
void stage_midi(Snapshot& s, unsigned group, unsigned sub) {
    auto& t = s.project.tracks[11];
    load_sound(t, EngineKind::Midi, 0);
    t.set_name("JUNO-60");
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

// Plug-ins (docs/MELODEE.md): a Plugins folder as the picker lists it, and a PLUGIN track's panel as the rack fills it.
void plugin_listing(Snapshot& s) {
    auto& v = s.plugins;
    v.root = "/media/card/Plugins"; v.folder = v.root; v.top = true;
    auto add = [&](const char* name, bool folder, PluginFormat format, int8_t instrument, const char* why = "") {
        PluginFile f;
        f.name = name; f.folder = folder; f.format = format; f.instrument = instrument; f.why = why;
        f.path = v.root + "/" + name;
        v.files.push_back(f);
    };
    add("Open MPC", true, PluginFormat::Vst3, -1);
    add("Dragonfly", true, PluginFormat::Vst3, -1);
    add("Acid-303", false, PluginFormat::Vst2Arm32, 1);
    add("Nekobi", false, PluginFormat::Vst3, 1);
    add("8W8", false, PluginFormat::Vst2, 1);
    add("MVerb", false, PluginFormat::Vst3, 0);
    add("Serum", false, PluginFormat::Vst3, -1, "Windows and Mac builds only");
    add("Kars", false, PluginFormat::Vst3, -1);
    add("LSP Plugins", false, PluginFormat::Vst3, 0);
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
    const char* names[] {"Cut Off", "Test for mono compatibility", "Time fraction"};
    const char* texts[] {"5.0", "7.2", "3.5"};
    const int16_t values[] {500, 720, 350};
    for (unsigned k = 0; k < 3; ++k) {
        auto& c = panel.params[k];
        c.used = true; c.value = values[k];
        std::snprintf(c.name, sizeof(c.name), "%s", names[k]);
        std::snprintf(c.text, sizeof(c.text), "%s", texts[k]);
    }
    // The list's window: the row before (Resonance, Env mod, Decay), then the three in hand.
    const char* before[] {"Resonance", "Env mod", "Decay"};
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
    t.engine = EngineKind::Plugin; t.plugin = 0; t.set_name("ACID-303");
    std::snprintf(s.project.plugins[0].path, plugin_path_length, "/media/card/Plugins/sd88me - VST - Acid-303");
    plugin_panel(s, 0, "Acid-303", "ARMV7", true, status);
}

struct Preview {
    const char* name;
    void (*set)(Shell&, Snapshot&);
};

const Preview previews[] {
    {"stage", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.playing = true; }}, // track 1: B1 PRESET
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
    {"stagepreset", [](Shell&, Snapshot& s) { // a sound turned to, waiting to load; starred
         on_stage(s, 0, 0, 0); s.playing = true; s.ui.preset_due = s.now + 100; s.ui.preset_cursor = 4;
         s.ui.settings.toggle_favourite(unsigned(EngineKind::Prophet), 4); s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"stagetone", [](Shell&, Snapshot& s) { on_stage(s, 0, 0, 1); s.playing = true; s.ui.last_pot = 1; }},
    {"stagemix", [](Shell&, Snapshot& s) { on_stage(s, 0, 0, 2); s.playing = true; }},
    {"stagesends", [](Shell&, Snapshot& s) { on_stage(s, 0, 0, 3); s.playing = true; s.ui.turning = 1; s.ui.turned_at = s.now; }}, // the four sends
    {"stagefilterenv", [](Shell&, Snapshot& s) { on_stage(s, 0, 1, 0); s.playing = true; }},
    {"stageampenv", [](Shell&, Snapshot& s) { on_stage(s, 0, 1, 1); s.playing = true; s.ui.turning = 2; s.ui.turned_at = s.now; }},
    {"stagelfo", [](Shell&, Snapshot& s) { on_stage(s, 0, 1, 2); s.playing = true; s.project.tracks[0].params[prophet::LfoAmount] = 40; }},
    {"stagearp", [](Shell&, Snapshot& s) { on_stage(s, 0, 2, 0); s.playing = true; s.project.tracks[0].arp_octaves = 2; }},
    {"stagearpheld", [](Shell&, Snapshot& s) { // keys held: the notes it plays from them, up and down
         on_stage(s, 0, 2, 0); s.playing = true; auto& t = s.project.tracks[0]; t.arp = true; t.arp_mode = 2;
         s.held_keys.fill(-1); s.held_keys[0] = 50; s.held_keys[4] = 53; s.held_keys[9] = 57;
     }},
    {"stagechord", [](Shell&, Snapshot& s) { on_stage(s, 0, 2, 1); s.playing = true; auto& t = s.project.tracks[0]; t.chord = true; t.chord_type = 1; s.ui.chord_pad = 1; }},
    {"stageperform", [](Shell&, Snapshot& s) { on_stage(s, 0, 2, 2); s.playing = true; }},
    {"stagefx", [](Shell&, Snapshot& s) { on_stage(s, 2, 2, 3); s.playing = true; s.ui.fx_cursor[2] = 1; }},
    {"stagefxempty", [](Shell&, Snapshot& s) { on_stage(s, 0, 2, 3); s.playing = true; }},
    {"stageshift", [](Shell&, Snapshot& s) { on_stage(s, 0, 1, 1); s.ui.shift = true; }}, // AMP ENV's Shift row
    {"stagefm6", [](Shell&, Snapshot& s) { load_sound(s.project.tracks[5], EngineKind::Fm6, 1); on_stage(s, 5, 1, 0); s.playing = true; }},
    {"stagecz", [](Shell&, Snapshot& s) { load_sound(s.project.tracks[6], EngineKind::Cz, 2); on_stage(s, 6, 1, 0); s.playing = true; }},
    {"stagesid", [](Shell&, Snapshot& s) { load_sound(s.project.tracks[7], EngineKind::Sid, 1); on_stage(s, 7, 0, 1); s.playing = true; }},
    {"stagecztone", [](Shell&, Snapshot& s) { load_sound(s.project.tracks[6], EngineKind::Cz, 2); on_stage(s, 6, 2, 2); s.playing = true; }},
    {"stagekitmix", [](Shell&, Snapshot& s) { on_stage(s, 3, 0, 1); s.sample = break_sample(); s.playing = true; s.project.tracks[3].pads[8].pan = 12; }},
    {"stagekitampenv", [](Shell&, Snapshot& s) {
         on_stage(s, 3, 0, 2); s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.attack = 20; pad.decay = 70; pad.velocity_sense = 60;
     }},
    {"stagekittune", [](Shell&, Snapshot& s) { on_stage(s, 3, 1, 0); s.sample = break_sample(); s.playing = true; s.project.tracks[3].pads[8].pitch = -2; }},
    {"stagekitplay", [](Shell&, Snapshot& s) { on_stage(s, 3, 1, 1); s.sample = break_sample(); s.playing = true; s.project.tracks[3].pads[8].group = 2; }},
    {"stagekitfollow", [](Shell&, Snapshot& s) { on_stage(s, 3, 1, 2); s.sample = break_sample(); s.playing = true; s.project.tracks[3].params[sampler::Follow] = 1; }},
    {"stagekitcloud", [](Shell&, Snapshot& s) { on_stage(s, 3, 1, 3); s.sample = break_sample(); s.playing = true; s.project.tracks[3].params[sampler::Cloud] = 1; }},
    {"stagekitcloudoff", [](Shell&, Snapshot& s) { on_stage(s, 3, 1, 3); s.sample = break_sample(); s.playing = true; }},
    {"stagecloudplaying", [](Shell&, Snapshot& s) { // CLOUD while A09 is held: its grains, the bars they cover lit, where they are
         on_stage(s, 3, 1, 3); s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.params[sampler::Cloud] = 1; t.params[sampler::Position] = 38;
         auto& pad = t.pads[8]; pad.start = 0; pad.end = 0; // the whole break
         uint32_t end = pad.end ? pad.end : s.sample.frames;
         cloud_heard(s, pad.start + (end - pad.start) * 0.38);
     }},
    {"stagecloudscan", [](Shell&, Snapshot& s) { // Scan moving: the cloud has travelled on from K1's line, the label with it
         on_stage(s, 3, 1, 3); s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.params[sampler::Cloud] = 1; t.params[sampler::Position] = 20; t.params[sampler::Scan] = 75;
         auto& pad = t.pads[8]; pad.start = 0; pad.end = 0; // the whole break
         uint32_t end = pad.end ? pad.end : s.sample.frames;
         cloud_heard(s, pad.start + (end - pad.start) * 0.52);
     }},
    {"stagekitfilt", [](Shell&, Snapshot& s) { on_stage(s, 3, 2, 2); s.sample = break_sample(); s.playing = true; s.project.tracks[3].params[sampler::Filter] = 55; }},
    {"stagekitfilter", [](Shell&, Snapshot& s) {
         on_stage(s, 3, 2, 0); s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.filter = PadFilter::Lpf4; pad.cutoff = 45; pad.resonance = 40;
     }},
    {"stagekitfiltenv", [](Shell&, Snapshot& s) {
         on_stage(s, 3, 2, 1); s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.filter = PadFilter::Classic; pad.filter_attack = 30; pad.filter_decay = 70; pad.filter_amount = 60;
     }},
    {"stagekitempty", [](Shell&, Snapshot& s) { on_stage(s, 3, 0, 0); s.project.tracks[3].last_pad = 20; s.playing = true; }}, // B05: no sample
    {"stage909", [](Shell&, Snapshot& s) { stage_909(s, 0, 0); }},
    {"stage909voice", [](Shell&, Snapshot& s) { stage_909(s, 0, 1); s.ui.turning = 0; s.ui.turned_at = s.now; }},
    {"stage909levels", [](Shell&, Snapshot& s) { stage_909(s, 0, 2); }},
    {"stage909mix", [](Shell&, Snapshot& s) { stage_909(s, 1, 0); }},
    {"stage909fx", [](Shell&, Snapshot& s) { stage_909(s, 2, 0); }},
    {"stagemidi", [](Shell&, Snapshot& s) { stage_midi(s, 0, 0); s.ui.track_out_at[11] = s.now; }},
    {"stagemidiprogram", [](Shell&, Snapshot& s) { stage_midi(s, 0, 1); s.ui.turning = 2; s.ui.turned_at = s.now; }},
    {"stagemidicc", [](Shell&, Snapshot& s) { stage_midi(s, 1, 0); }},
    {"stagemidiccshift", [](Shell&, Snapshot& s) { stage_midi(s, 1, 0); s.ui.shift = true; }},
    {"stagemidiarp", [](Shell&, Snapshot& s) { stage_midi(s, 2, 0); }},
    {"stageemptysynth", [](Shell&, Snapshot& s) { on_stage(s, 12, 0, 0); s.ui.new_sound = 3; }},
    {"stageemptykit", [](Shell&, Snapshot& s) { on_stage(s, 12, 1, 0); s.ui.last_pot = 1; s.ui.new_source = 2; }},
    {"stageemptymidi", [](Shell&, Snapshot& s) { on_stage(s, 12, 2, 0); s.ui.new_channel = 3; s.ui.last_pot = 2; s.ui.new_port = 1; }},
    {"tracks", [](Shell&, Snapshot& s) { s.ui.page = PageId::Tracks; s.ui.track = 2; s.playing = true; }}, // Pad Bank tapped twice
    // The Looper (docs/LOOPER.md): four loops in every state, the loop tracks' MIX, the loop's sheet, off, a loop's chain.
    {"looper", [](Shell&, Snapshot& s) { looper_scene(s); s.ui.turning = 1; s.ui.turned_at = s.now; }},
    {"loopermix", [](Shell&, Snapshot& s) { looper_scene(s); s.ui.looper_view = 1; s.ui.loop = 2; }},
    {"loopersheet", [](Shell&, Snapshot& s) { looper_scene(s); s.ui.loop_sheet = true; s.ui.sheet_more = true; s.ui.loop_row = 3; }},
    {"loopersheetlikely", [](Shell&, Snapshot& s) { looper_scene(s); s.ui.loop_sheet = true; }}, // B3 MORE as it opens: the likely job
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
    {"edit", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.edit_page[0] = 3; s.playing = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
         s.ui.pots[1] = {81, false, Binding {Binding::Kind::Param, prophet::Resonance, 0, 100, 0}.key(0)};
     }},
    {"soundmap", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.edit_page[0] = 3; s.ui.map_open = true; s.ui.map_cursor = 3; s.playing = true;
     }},
    {"mod", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Edit; s.ui.edit_page[0] = uint8_t(prophet::info().page_count); s.playing = true;
     }},
    {"touch", [](Shell&, Snapshot& s) { // velocity and pad pressure (phase 1d)
         s.ui.page = PageId::Edit; s.ui.edit_page[0] = uint8_t(prophet::info().page_count - 1); s.ui.edit_row[0] = 1; s.playing = true;
         s.ui.turning = 1; s.ui.turned_at = s.now;
         s.ui.pots[1] = {50, false, Binding {Binding::Kind::Param, prophet::AtBright, 0, 100, 0}.key(1)};
     }},
    {"steps", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Steps; s.ui.step_cursor = 6; s.ui.held_step = 6; s.playing = true; s.position = 9 * sixteenth_ticks;
         s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"motion", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Motion; s.playing = true; s.position = 40 * sixteenth_ticks; s.ui.recording = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
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
    {"pluginsbundle", [](Shell&, Snapshot& s) { // LSP's bundle opened for track 1's third slot: a row a plug-in
         s.ui.page = PageId::Plugins; s.ui.plugin_target = UiState::PluginTarget::Effect; s.ui.plugin_chain = 0; s.ui.plugin_slot = 2;
         auto& v = s.plugins;
         v.root = "/media/card/Plugins"; v.folder = v.root + "/LSP/lsp-plugins.vst3"; v.top = false;
         const char* names[] {"A/B Tester x2 Mono", "Beat Breather Stereo", "Compressor x2 Stereo", "Dynamic Processor Stereo",
                              "Graphic Equalizer x16 Stereo", "Impulse Reverb Stereo", "Multiband Compressor x8 Stereo", "Trigger Stereo"};
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
         std::snprintf(s.project.plugins[4].path, plugin_path_length, "/media/card/Plugins/DragonflyHallReverb.vst3");
         s.ui.fx_cursor[0] = 1;
         plugin_panel(s, 4, "Dragonfly Hall Reverb", "VST3", false, PluginStatus::Ready);
     }},
    {"rackpluginsheet", [](Shell&, Snapshot& s) { // Push on that plug-in: its sheet, with Presets
         s.ui.page = PageId::Rack; s.ui.track = 0;
         s.project.tracks[0].fx.slots[0] = {uint8_t(EffectKind::Echo), false, {60, 40, 30, 60, 0, 0}};
         s.project.tracks[0].fx.slots[1] = {plugin_effect_kind, false, {4, 0, 0, 0, 0, 0}};
         std::snprintf(s.project.plugins[4].path, plugin_path_length, "/media/card/Plugins/TONE3000.vst3");
         s.ui.fx_cursor[0] = 1;
         plugin_panel(s, 4, "TONE3000", "VST3", false, PluginStatus::Ready);
         s.ui.plugin_panel.programs = 8;
         s.ui.sheet_open = true; s.ui.sheet_row = 1;
     }},
    {"pluginpresets", [](Shell&, Snapshot& s) { // PRESETS for TONE3000 on track 1's chain: the eight factory ones, one loaded
         s.ui.page = PageId::PluginPresets; s.ui.track = 0; s.ui.presets_back = PageId::Rack; s.ui.presets_entry = 4;
         std::snprintf(s.project.plugins[4].path, plugin_path_length, "/media/card/Plugins/TONE3000.vst3");
         plugin_panel(s, 4, "TONE3000", "VST3", false, PluginStatus::Ready);
         auto& v = s.presets;
         v.entry = 4;
         const char* names[] {"Ampeg Bass Drive", "Bogner Fullstack", "Dumble Spread", "Marshall Bluesbreaker",
                              "Matchless Crunch", "Mesa Pitch Shift", "Stereo Crunch Verb", "Stereo Fuzz"};
         for (int i = 0; i < 8; ++i) v.list.push_back({i, names[i]});
         v.current = 3; s.ui.presets_cursor = 4;
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
    {"timecorrect", [](Shell&, Snapshot& s) { // Shift + Pad 14: the chosen pads' notes onto Q
         s.ui.page = PageId::TimeCorrect; s.ui.track = 0; s.ui.tc_all = false; s.ui.tc_swing = 62; s.ui.tc_shift = -6;
         for (unsigned pad : {0u, 2u, 4u, 7u}) s.ui.tc_toggle(unsigned(pad_note(s.project, s.track(), pad).key));
         s.ui.turning = 2; s.ui.turned_at = s.now;
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
    {"padsplaying", [](Shell&, Snapshot& s) { // the pad in hand as it plays: a head on its small waveform
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         playing_head(s, 9000, 0.8f);
     }},
    {"padslines", [](Shell&, Snapshot& s) { // a kit playing: a line along the foot of each sounding pad, the one in hand too
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         pad_voice(s, 0, 8, 40, 0.62f, 0.9f);  // A09, in hand: a line in the background colour
         pad_voice(s, 1, 0, 39, 0.3f, 1.0f);   // A01 kick
         pad_voice(s, 2, 2, 38, 0.85f, 0.35f); // A03 hat, fading
         pad_voice(s, 3, 5, 37, 0.18f, 0.8f);  // A06
         pad_voice(s, 4, 0, 30, 0.95f, 0.1f);  // an older kick: the newest one's line shows
     }},
    {"padcopy", [](Shell&, Snapshot& s) { // Shift + Erase, then a pad: where does it go
         s.ui.page = PageId::Pads; s.ui.track = 3; s.sample = break_sample(); s.ui.copying = UiState::Copying::Pads; s.ui.copy_from = 2;
     }},
    {"mutepads", [](Shell&, Snapshot& s) { // mute mode latched on a kit page: two pads muted
         s.ui.page = PageId::Pads; s.ui.track = 3; s.sample = break_sample(); s.ui.mute_latch = true; s.playing = true;
         s.project.tracks[3].set_pad_muted(1, true); s.project.tracks[3].set_pad_muted(6, true);
     }},
    {"padbank", [](Shell&, Snapshot& s) { // Pad Bank held on a kit in bank F: the sixteen tracks
         s.ui.page = PageId::Pads; s.ui.track = 3; s.sample = break_sample(); s.ui.pad_bank = true; s.playing = true;
         s.ui.pad_bank_down = s.now - pad_bank_overlay_ms; // held a second: the overlay is up
         s.project.tracks[3].bank = 5;
     }},
    {"copytrack", [](Shell&, Snapshot& s) { // Pad Bank held, Shift + Erase, track 2 picked: where does it go
         s.ui.page = PageId::Stage; s.ui.track = 1; s.ui.pad_bank = true;
         s.ui.copying = UiState::Copying::Tracks; s.ui.copy_from = 1;
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
         const char* files[] {"AMEN TYPE", "DUST BREAK", "FUNK LOOP 04", "RHODES STAB", "TAPE HIT", "VINYL CHORD"};
         for (auto name : files) s.files.push_back({std::string("/media/card/Samples/") + name + ".wav", name, 0, false});
         s.files[4].path = s.project.samples[4].path;
         s.ui.files_count = uint16_t(s.files.size()); s.ui.turning = 1; s.ui.turned_at = s.now;
     }},
    {"samplesinstruments", [](Shell&, Snapshot& s) { // a folder of instruments: each tagged by its kind, a kit, an expansion
         s.ui.page = PageId::Samples; s.ui.track = 3; s.playing = true; s.ui.files_cursor = 0; s.sample_root = "/media/card/Samples";
         s.files_folder = s.sample_root + "/Instruments"; s.files_top = false;
         struct Row { const char* name; const char* file; bool instrument, kit, expansion; };
         const Row rows[] {{"BOYS CHOIR AH-EH-EE-OO", "Boys Choir Ah-Eh-Ee-Oo.exs", true, false, false},
                           {"GRAND PIANO", "Grand Piano.sfz", true, false, false},
                           {"RHODES MK1", "Rhodes MK1.xpm", true, false, false},
                           {"ORGAN-FARFISA", "Organ-Farfisa.xty", true, false, false},
                           {"KIT-LOFI PATCH", "Kit-Lofi Patch.xpm", false, true, false},
                           {"VINTAGE ORGANS", "Vintage Organs.xpn", false, false, true}};
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
         const char* files[] {"AMEN TYPE", "DUST BREAK", "FUNK LOOP 04", "RHODES STAB", "TAPE HIT", "VINYL CHORD"};
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
         for (auto name : {"BREAKS C", "DUSTY KIT A", "VOX CHOPS"}) s.files.push_back({s.files_folder + "/" + name + ".mldb", name, 0, false, true});
         s.ui.files_count = uint16_t(s.files.size());
         std::snprintf(s.ui.notice, sizeof(s.ui.notice), "Bank C: DUSTY KIT A"); s.ui.notice_at = s.now;
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
    {"triminstrument", [](Shell&, Snapshot& s) { // TRIM on a pad holding an .exs: its keys and velocities in place of a waveform
         on_stage(s, 3, 0, 0); s.playing = true; choir_pad(s, 8);
         s.ui.turning = 0; s.ui.turned_at = s.now;
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
    {"chopshift", [](Shell&, Snapshot& s) { // Shift: Extract / Split / Merge, zoomed in at the slice's end
         s.ui.page = PageId::Chop; s.ui.track = 3; s.sample = break_sample(); s.ui.chop_sample = 4;
         s.ui.slices = chop(ChopMode::Threshold, 40, 20000, 200000, s.sample.frames, s.sample.hits, s.sample.hit_count, 920);
         s.ui.chop_slice = 2; s.ui.chop_zoom = 2; s.ui.chop_focus = 1;
         s.ui.shift = true; s.ui.shift_used = true;
     }},
    {"levels", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Levels; s.ui.track = 3; s.playing = true; s.sample = break_sample();
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
    {"mixdownname", [](Shell&, Snapshot& s) { // Export, B2: the mixdown's name
         s.ui.page = PageId::Song; s.ui.track = 2;
         s.ui.name.start(NameTarget::Mixdown, "Night drive", 31, false);
     }},
    {"project", [](Shell&, Snapshot& s) { // Shift + Pad 16: SAVE, OPEN, NEW
         s.ui.page = PageId::Project; s.playing = true;
         preview_projects(s);
     }},
    {"projectlist", [](Shell&, Snapshot& s) { // OPEN: the list, the open project in hand
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_list = true; s.ui.project_cursor = 3;
         preview_projects(s);
     }},
    {"filesheet", [](Shell&, Snapshot& s) { // Push on the open project: SAVE, and MORE for the rest
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_list = true; s.ui.project_cursor = 3; s.ui.sheet_open = true;
         preview_projects(s);
     }},
    {"filesheetmore", [](Shell&, Snapshot& s) { // MORE: all of them
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_list = true; s.ui.project_cursor = 3; s.ui.sheet_open = true;
         s.ui.sheet_more = true; s.ui.sheet_row = 0;
         preview_projects(s);
     }},
    // Parity phase 10: another project's sheet with Load kit, New project asking first.
    {"loadkit", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.ui.project_list = true; s.ui.project_cursor = 4; s.ui.sheet_open = true; s.ui.sheet_more = true; s.ui.sheet_row = 1;
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
    {"newproject", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.ui.project_cursor = 0; s.ui.ask = UiState::Ask::NewProject; s.ui.ask_target = 1;
         preview_projects(s);
     }},
    {"movefolder", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_list = true; s.ui.project_cursor = 3; s.ui.move_open = true; s.ui.move_cursor = 1;
         preview_projects(s);
     }},
    {"tempo", [](Shell&, Snapshot& s) { s.ui.page = PageId::Tempo; s.playing = true; s.ticks = ticks_per_quarter; s.ui.settings.click = 1; s.project.swing = 54; s.ui.turning = 0; s.ui.turned_at = s.now; }},
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
     }},
    {"compressorside", [](Shell&, Snapshot& s) { // Sidechain: track 4 (a kit) keys it whole, the tracks on the pads, track 1 selected
         s.ui.page = PageId::Compressor; s.playing = true; s.ui.track = 0;
         auto& c = s.project.compressor;
         c.on = true; c.color = true; c.sidechain = true; c.key = 3; c.amount = 55; c.attack = 5; c.release = 60;
         s.project.tracks[3].set_name("CLASSIC 909 KITS"); // a long name, as the device's own
         s.compression = 7.0f;
         kick_trail(s);
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
    {"settings", [](Shell&, Snapshot& s) { s.ui.page = PageId::SettingsPage; s.playing = true; s.ui.settings_door = 3; }}, // the doors, AUDIO in hand
    {"settingspads", [](Shell&, Snapshot& s) { s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 0; s.ui.settings_cursor = Settings::PadResponse; }},
    {"settingsscreen", [](Shell&, Snapshot& s) { s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 2; s.ui.settings_cursor = Settings::Palette; }},
    {"settingsaudio", [](Shell&, Snapshot& s) { // AUDIO's rows, 48 kHz with a dropout
        s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 3; s.ui.settings_cursor = Settings::AudioBuffer; s.ui.settings_editing = true;
        s.ui.settings.audio_rate = 48000; s.ui.settings.audio_periods = 4;
        s.audio_rate = 48000; s.audio_periods = 4; s.audio_xruns = 1;
    }},
    {"settingssampling", [](Shell&, Snapshot& s) { // the sampling rows: how Audio Recall cuts, when a take ends on its own
        s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 4; s.ui.settings_cursor = Settings::RecallGrabRow; s.ui.settings_editing = true;
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
    {"punch", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Punch; s.ui.track = 2; s.playing = true; s.project.punch_target = 1;
         s.project.punch_bank = 0; // Melodee's first bank: Loop 1/4 latched, Low held
         s.ui.engaged[0] = {8, -1, true, 100}; s.ui.engaged[1] = {4, 4, false, 100}; s.ui.engaged_count = 2;
         s.ui.punch_slot = {{8, 4, -1, -1}}; s.ui.punch_selected = 4;
         s.ui.turning = 1; s.ui.turned_at = s.now;
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
    {"fadersynth", [](Shell&, Snapshot& s) { // a synth track with its fader off
         s.ui.page = PageId::FaderPage; s.ui.track = 2;
         auto& t = s.project.tracks[2];
         t.fader_on = false; t.wheel = 70;
     }},
    {"fadercard", [](Shell&, Snapshot& s) { // the fader moved on Tracks: the selected track's level
         s.ui.page = PageId::Tracks; s.ui.track = 2; s.playing = true;
         s.ui.fader = {90, true, 1}; s.ui.fader_moved_at = s.now;
         s.project.tracks[2].mix.level = 90;
         const float meters[] {0.5f, 0.42f, 0.2f, 0, 0.6f, 0.25f, 0.3f, 0.05f, 0.18f, 0.08f};
         for (unsigned t = 0; t < 10; ++t) s.meters[t] = meters[t];
     }},
    {"faderpickup", [](Shell&, Snapshot& s) { // the fader moved on the kit's Pads page: A09's volume, not reached yet
         s.ui.page = PageId::Pads; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         s.project.tracks[3].pads[8].level = 100;
         s.ui.fader = {40, false, 1}; s.ui.fader_moved_at = s.now;
     }},
    {"fadersettings", [](Shell&, Snapshot& s) { // Settings: the three takeover rows, the fader's open
         s.ui.page = PageId::SettingsPage; s.ui.settings_in = true; s.ui.settings_door = 1; s.ui.settings_cursor = Settings::FaderTakeover; s.ui.settings_editing = true;
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
         auto& t = s.project.tracks[3]; t.params[sampler::Cloud] = 1; t.params[sampler::Position] = 38; t.last_pad = 8;
         auto& pad = t.pads[8]; pad.start = 0; pad.end = 230087;
         cloud_heard(s, 230087 * 0.38);
     }},
    {"cloud", [](Shell&, Snapshot& s) { s.ui.page = PageId::CloudPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.params[sampler::Cloud] = 1; t.last_pad = 4; t.pads[4].start = 0; t.pads[4].end = 230087; s.ui.turning = 0; s.ui.turned_at = s.now; }},
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
    {"ampenv", [](Shell&, Snapshot& s) { s.ui.page = PageId::EnvelopePage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.attack = 20; pad.decay = 70; pad.velocity_sense = 60; s.ui.turning = 1; s.ui.turned_at = s.now; }},
    {"tune", [](Shell&, Snapshot& s) { s.ui.page = PageId::PadPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.pitch = -2; pad.warp = warp_seq; pad.beats = 8; s.ui.turning = 2; s.ui.turned_at = s.now; }},
    {"levelsfilter", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Levels; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3]; t.levels = true; t.levels_pad = 1; t.last_pad = 1; t.levels_kind = uint8_t(LevelsKind::Filter);
         t.pads[1].cutoff = 70; s.sample.frames = 26460; s.ui.levels_hit = 5;
     }},
    {"levelssynth", [](Shell&, Snapshot& s) { // 16 Levels on a synth: its last note, by Tune
         s.ui.page = PageId::Levels; s.ui.track = 0; s.playing = true;
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
         s.ui.page = PageId::Levels; s.ui.track = 3; s.playing = true;
         auto& t = s.project.tracks[3]; t.levels = true; t.levels_pad = 8; t.last_pad = 8; t.levels_kind = uint8_t(LevelsKind::Tune);
         t.octave = int8_t(kit_level_octave + 1);
         choir_pad(s, 8);
         s.ui.levels_hit = 3; s.ui.levels_value = 12;
         std::snprintf(s.ui.notice, sizeof(s.ui.notice), "Octave +1"); s.ui.notice_at = s.now;
     }},
    {"levelsscale", [](Shell&, Snapshot& s) { // 16 Levels up the scale on a kit: K1 and K2 are the key
         s.ui.page = PageId::Levels; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3]; t.levels = true; t.levels_pad = 1; t.last_pad = 1; t.levels_kind = uint8_t(LevelsKind::Scale);
         s.project.root = 0; s.project.scale = 7; s.ui.levels_hit = 5;
     }},
    {"exit", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.exit_open = true; }},
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
         s.ui.name.start(NameTarget::Sound, "GLASS PLUCK", name_length - 1, true);
         s.ui.name.digits = true; s.ui.name.insert(' '); s.ui.name.pad(1, s.now - 2000);
     }},
    {"savesample", [](Shell&, Snapshot& s) { // Shift + Sample Select on a kit: Save Sample's Name sheet
         on_stage(s, 3, 0, 0); s.sample = break_sample();
         s.ui.name.start(NameTarget::Sample, "DUST BREAK", name_length - 1, false);
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
    {"stepedit", [](Shell& shell, Snapshot& s) { // Shift + Seq: the kit's step 5, three events, the second chosen
         step_edit_kit(shell, s);
         s.ui.turning = 2; s.ui.turned_at = s.now;
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
    {"notesheet", [](Shell& shell, Snapshot& s) { // Push on the note in focus: ACCENT, MORE for the rest
         roll_bass(shell, s, 24, 57);
         s.ui.sheet_open = true;
     }},
    {"notesheetmore", [](Shell& shell, Snapshot& s) { // MORE: Accent, Slide, Tie, Delete
         roll_bass(shell, s, 24, 57);
         s.ui.sheet_open = true; s.ui.sheet_more = true; s.ui.sheet_row = 1;
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
    {"stepsratchet", [](Shell& shell, Snapshot& s) { // Steps' TRIG row on the kit, the hat's lane: step 5 a ratchet of three rising, Shift held
         step_edit_kit(shell, s);
         s.ui.page = PageId::Steps; s.project.tracks[3].last_pad = 2;
         for (auto& e : shell.patterns().at[3][0].events) if (e.tick == 96 && e.key == sampler_first_note + 2) { e.ratchet = 3; e.ramp = 40; e.pitch_ramp = 5; }
         for (auto& e : shell.patterns().at[3][0].events) if (e.tick == 144) { e.ratchet = 2; }
         s.ui.trig_cards = true; s.ui.shift = true;
     }},
    {"stepstrig", [](Shell& shell, Snapshot& s) { // Steps' TRIG row: K3 turning a trig condition, the hat at step 5 on 3:4, step 3 FILL
         step_edit_kit(shell, s);
         s.ui.page = PageId::Steps; s.project.tracks[3].last_pad = 2;
         for (auto& e : shell.patterns().at[3][0].events) {
             if (e.tick == 96 && e.key == sampler_first_note + 2) { e.ratchet = 3; e.ramp = 40; e.condition = uint8_t(trig_of(3, 4)); }
             if (e.tick == 48) e.condition = uint8_t(trig_fill);
         }
         s.ui.trig_cards = true; s.ui.turning = 2; s.ui.turned_at = s.now;
     }},
    {"stepslock", [](Shell& shell, Snapshot& s) { // step 5 held on the kit's hat lane: the cards lock the hat's level, tune and pan there
         step_edit_kit(shell, s);
         s.ui.page = PageId::Steps; s.project.tracks[3].last_pad = 2; s.ui.held_step = 4;
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
    require(written > 0, "Unknown Melodee page; use stage stagepreset stage909 stagemidi stageemptysynth tracks keys sounds edit soundmap mod mix steps roll notesheet motion patterns recquantize recquantizeoff seqsheet seqbpm seqclear rollkit song songzoom songclip songshift length rack sends bus fxpick pads samples trim chop levels record project filesheet tempo arp chord capture pad play envelope filter follow cloud padmix padfilter filterenv trimloop trimshift ampenv tune levelsfilter levelssynth shift exit exitcard exitoff exitsaved");
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
// tail: the bass line must sound, stay below full scale and end in silence.
int sound(const std::string& path) {
    EngineHost host;
    Shell shell(host);
    starter_project(shell.project(), shell.patterns());
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
            std::printf("pad %s file=%s start=%u end=%u level=%u pan=%d pitch=%d play=%d mono=%d group=%u filter=%s cutoff=%u warp=%u attack=%u decay=%u loop=%d\n", label, project->samples[pad.sample].name,
                        pad.start, pad.end, pad.level, pad.pan, pad.pitch, int(pad.play), pad.mono, pad.group, pad_filter_name(pad.filter), pad.cutoff, pad.warp, pad.attack, pad.decay, pad.loop);
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
    if (argc >= 3 && !std::strcmp(argv[1], "--melodee-bench")) {
        std::vector<std::string> names(argv + 3, argv + argc);
        return bench(unsigned(std::max(1, std::atoi(argv[2]))), names);
    }
    return std::nullopt;
}

} // namespace melodee
