// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/tools.h"
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
#include "drivers/usb_audio.h"
#include "runtime/screen.h"
#include "runtime/util.h"
#include <algorithm>
#include <chrono>
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
// One note on a step of a pattern's Q, in place of what the step held.
void put(Pattern& p, unsigned step, uint8_t key, uint8_t velocity, uint8_t length = 1, uint8_t flags = 0) {
    Step s;
    s.notes[0] = {key, velocity};
    s.length = length; s.flags = flags;
    p.set_step(step, s);
}

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
    v.folders = {"Live sets", "Live sets/Spring tour", "Sketches", "Sketches/2026"};
    s.project_path = v.root + "/Night drive.mldp";
    s.dirty = true;
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
void pad_motion_kit(Shell&, Snapshot& s) {
    s.ui.page = PageId::Motion; s.ui.track = 3; s.playing = true; s.position = 9 * sixteenth_ticks; s.ui.recording = true;
    auto& t = s.project.tracks[3];
    t.pattern = 0; t.last_pad = 8;
    const int level[] {100, 84, 62, 50, 70, 96, 110, 120};
    for (unsigned n = 0; n < 8; ++n) s.project.motion.write({3, 0, uint16_t(n * 2), uint8_t(motion_pad_param(PadParam::Level)), int16_t(level[n]), 8});
    const int tune[] {0, 0, 5, 7, -5, 0};
    for (unsigned n = 0; n < 6; ++n) s.project.motion.write({3, 0, uint16_t(2 + n * 2), uint8_t(motion_pad_param(PadParam::Pitch)), int16_t(tune[n]), 8});
    s.project.motion.write({3, 0, 4, uint8_t(motion_pad_param(PadParam::Level)), 40, 0});
}

// Knob FX with list entry `entry` loaded, at its initial values (as Context::knob_fx_load does).
void load_knob(KnobFx& k, unsigned entry) {
    const auto& e = knob_fx_entry(entry);
    k.choice = uint8_t(entry); k.kind = uint8_t(e.kind);
    const auto& info = effect_info(e.kind);
    for (unsigned q = 0; q < effect_params; ++q) k.params[q] = info.params[q].label ? info.params[q].initial : 0;
    if (e.param >= 0) k.params[size_t(e.param)] = e.value;
}

// Stage on `track`, on button `group`'s page `sub` (shell/stage_map.h).
void on_stage(Snapshot& s, unsigned track, unsigned group, unsigned sub) {
    s.ui.page = PageId::Stage; s.ui.track = uint8_t(track);
    s.ui.stage_group[track] = uint8_t(group); s.ui.stage_sub[track] = uint8_t(sub);
}
// Stage on track 11, a 909 (BOOM KIT), its snare and clap last hit.
void stage_909(Snapshot& s, unsigned group, unsigned sub) {
    auto& t = s.project.tracks[10];
    load_sound(t, EngineKind::Kit909, 2);
    t.last_note = 36 + kit909::Clap; t.mix.level = 96; t.mix.send = 20;
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

struct Preview {
    const char* name;
    void (*set)(Shell&, Snapshot&);
};

const Preview previews[] {
    {"stage", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.playing = true; }}, // track 1: B1 PRESET
    // Stage, context aware: each track type's pages (pages/stage_page.cpp).
    {"stagepreset", [](Shell&, Snapshot& s) { // a sound turned to, waiting to load; starred
         on_stage(s, 0, 0, 0); s.playing = true; s.ui.preset_due = s.now + 100; s.ui.preset_cursor = 4;
         s.ui.settings.toggle_favourite(unsigned(EngineKind::Prophet), 4); s.ui.turning = 0; s.ui.turned_at = s.now;
     }},
    {"stagetone", [](Shell&, Snapshot& s) { on_stage(s, 0, 0, 1); s.playing = true; s.ui.last_pot = 1; }},
    {"stagemix", [](Shell&, Snapshot& s) { on_stage(s, 0, 0, 2); s.playing = true; }},
    {"stagefilterenv", [](Shell&, Snapshot& s) { on_stage(s, 0, 1, 0); s.playing = true; }},
    {"stageampenv", [](Shell&, Snapshot& s) { on_stage(s, 0, 1, 1); s.playing = true; s.ui.turning = 2; s.ui.turned_at = s.now; }},
    {"stagelfo", [](Shell&, Snapshot& s) { on_stage(s, 0, 1, 2); s.playing = true; s.project.tracks[0].params[prophet::LfoAmount] = 40; }},
    {"stagearp", [](Shell&, Snapshot& s) { on_stage(s, 0, 2, 0); s.playing = true; s.project.tracks[0].arp_octaves = 2; }},
    {"stagearpheld", [](Shell&, Snapshot& s) { // keys held: the notes it plays from them, up and down
         on_stage(s, 0, 2, 0); s.playing = true; auto& t = s.project.tracks[0]; t.arp = true; t.arp_mode = 2;
         s.held_keys.fill(-1); s.held_keys[0] = 50; s.held_keys[4] = 53; s.held_keys[9] = 57;
     }},
    {"stageperform", [](Shell&, Snapshot& s) { on_stage(s, 0, 2, 1); s.playing = true; }},
    {"stagefx", [](Shell&, Snapshot& s) { on_stage(s, 2, 2, 2); s.playing = true; s.ui.fx_cursor[2] = 1; }},
    {"stagefxempty", [](Shell&, Snapshot& s) { on_stage(s, 0, 2, 2); s.playing = true; }},
    {"stageshift", [](Shell&, Snapshot& s) { on_stage(s, 0, 1, 1); s.ui.shift = true; }}, // AMP ENV's Shift row
    {"stagefm6", [](Shell&, Snapshot& s) { load_sound(s.project.tracks[5], EngineKind::Fm6, 1); on_stage(s, 5, 1, 0); s.playing = true; }},
    {"stagecz", [](Shell&, Snapshot& s) { load_sound(s.project.tracks[6], EngineKind::Cz, 2); on_stage(s, 6, 1, 0); s.playing = true; }},
    {"stagesid", [](Shell&, Snapshot& s) { load_sound(s.project.tracks[7], EngineKind::Sid, 1); on_stage(s, 7, 0, 1); s.playing = true; }},
    {"stagecztone", [](Shell&, Snapshot& s) { load_sound(s.project.tracks[6], EngineKind::Cz, 2); on_stage(s, 6, 2, 1); s.playing = true; }},
    {"stagekitmix", [](Shell&, Snapshot& s) { on_stage(s, 3, 0, 1); s.sample = break_sample(); s.playing = true; s.project.tracks[3].pads[8].pan = 12; }},
    {"stagekitampenv", [](Shell&, Snapshot& s) {
         on_stage(s, 3, 0, 2); s.sample = break_sample(); s.playing = true;
         auto& pad = s.project.tracks[3].pads[8]; pad.attack = 20; pad.decay = 70; pad.velocity_sense = 60;
     }},
    {"stagekittune", [](Shell&, Snapshot& s) { on_stage(s, 3, 1, 0); s.sample = break_sample(); s.playing = true; s.project.tracks[3].pads[8].pitch = -2; }},
    {"stagekitplay", [](Shell&, Snapshot& s) { on_stage(s, 3, 1, 1); s.sample = break_sample(); s.playing = true; s.project.tracks[3].pads[8].group = 2; }},
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
    {"stageemptykit", [](Shell&, Snapshot& s) { on_stage(s, 12, 1, 0); }},
    {"stageemptymidi", [](Shell&, Snapshot& s) { on_stage(s, 12, 2, 0); s.ui.new_channel = 3; }},
    {"tracks", [](Shell&, Snapshot& s) { s.ui.page = PageId::Tracks; s.ui.track = 2; s.playing = true; }}, // Pad Bank tapped twice
    {"tracks2", [](Shell&, Snapshot& s) { s.ui.page = PageId::Tracks; s.ui.track = 9; s.ui.tracks_half = 1; }},
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
    {"mix", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Mix; s.ui.track = 2; s.playing = true;
         s.ui.turning = 0; s.ui.turned_at = s.now;
         s.ui.pots[2] = {90, false, Binding {Binding::Kind::Send, 0, 0, 100, 0}.key(2)};
         const float meters[] {0.5f, 0.42f, 0.2f, 0, 0.6f, 0.25f, 0.3f, 0.05f, 0.18f, 0.08f};
         for (unsigned t = 0; t < 10; ++t) s.meters[t] = meters[t];
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
         s.ui.page = PageId::Patterns; s.ui.track = 2; s.playing = true; s.ui.launch_column = true; // B3: whole columns
         const uint8_t live[] {1, 0, 0, 2, 1, 0, 0, 0};
         for (unsigned t = 0; t < 8; ++t) { s.playing_patterns[t] = live[t]; s.project.tracks[t].pattern = live[t]; s.positions[t] = int((4 + t) * sixteenth_ticks); }
         s.project.tracks[2].pattern = 1;
     }},
    {"bpm", [](Shell&, Snapshot& s) { // B1 held on Patterns, BPM per pattern: pattern 2's own tempo
         s.ui.page = PageId::Patterns; s.ui.track = 0; s.ui.b1_held = true; s.project.seq_tempo = true;
         s.project.pattern_tempo[1] = 985; s.ui.lead_pattern = 1; s.tempo = 985; s.ui.bpm_tenths = true;
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
    {"clipsheet", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Song; s.ui.track = 2; s.ui.song_cursor = 26; s.ui.song_view = 16; s.ui.sheet_open = true;
         s.playing = true;
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
    {"padcopy", [](Shell&, Snapshot& s) { // Shift + Erase, then a pad: where does it go
         s.ui.page = PageId::Pads; s.ui.track = 3; s.sample = break_sample(); s.ui.copying = UiState::Copying::Pads; s.ui.copy_from = 2;
     }},
    {"mutepads", [](Shell&, Snapshot& s) { // mute mode latched on a kit page: two pads muted
         s.ui.page = PageId::Pads; s.ui.track = 3; s.sample = break_sample(); s.ui.mute_latch = true; s.playing = true;
         s.project.tracks[3].set_pad_muted(1, true); s.project.tracks[3].set_pad_muted(6, true);
     }},
    {"padbank", [](Shell&, Snapshot& s) { // Pad Bank held on a kit in bank F: the sixteen tracks
         s.ui.page = PageId::Pads; s.ui.track = 3; s.sample = break_sample(); s.ui.pad_bank = true; s.playing = true;
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
    {"project", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_cursor = 5;
         preview_projects(s);
     }},
    {"filesheet", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_cursor = 5; s.ui.sheet_open = true;
         preview_projects(s);
     }},
    // Parity phase 10: another project's sheet with Load kit, the demos, New project asking first.
    {"loadkit", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.ui.project_cursor = 6; s.ui.sheet_open = true; s.ui.sheet_row = 1;
         preview_projects(s);
     }},
    {"demos", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.ui.project_demos = true; s.ui.project_cursor = 1;
         preview_projects(s);
     }},
    // Parity phase 10b: SD card access, its row on Project, then the card on a computer, ejected, held, and none to lend.
    {"cardrow", [](Shell&, Snapshot& s) {
         s.ui.page = PageId::Project; s.ui.project_cursor = 2;
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
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_cursor = 5; s.ui.move_open = true; s.ui.move_cursor = 1;
         preview_projects(s);
     }},
    {"tempo", [](Shell&, Snapshot& s) { s.ui.page = PageId::Tempo; s.playing = true; s.ticks = ticks_per_quarter; s.project.click = true; s.project.swing = 54; s.ui.turning = 0; s.ui.turned_at = s.now; }},
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
    // Parity phase 7: Knob FX, the master compressor, the picker's new effects.
    {"knobfx", [](Shell&, Snapshot& s) { // a kit: Ensemble on three pads of bank A
         s.ui.page = PageId::KnobFx; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& k = s.project.knob_fx;
         load_knob(k, 19); k.on = true;
         for (unsigned pad : {0u, 1u, 4u}) k.set_pad(3, pad, true);
         s.ui.knob_cursor = 21; s.ui.turning = 1; s.ui.turned_at = s.now;
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
    {"settings", [](Shell&, Snapshot& s) { s.ui.page = PageId::SettingsPage; s.playing = true; s.ui.settings_cursor = 0; }},
    {"settingsaudio", [](Shell&, Snapshot& s) { // the audio rows, scrolled into view, 48 kHz with a dropout
        s.ui.page = PageId::SettingsPage; s.ui.settings_cursor = Settings::AudioBuffer; s.ui.settings_editing = true;
        s.ui.settings.audio_rate = 48000; s.ui.settings.audio_periods = 4;
        s.audio_rate = 48000; s.audio_periods = 4; s.audio_xruns = 1;
    }},
    {"about", [](Shell&, Snapshot& s) { s.ui.page = PageId::About; s.playing = true; }},
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
    {"fader", [](Shell&, Snapshot& s) { // Shift + Pad 9 on the kit: Pad Pan on A09, the fader still to reach it (Pickup)
         s.ui.page = PageId::FaderPage; s.ui.track = 3; s.playing = true; s.sample = break_sample();
         auto& t = s.project.tracks[3];
         t.fader_target = uint8_t(FaderTarget::PadPan); t.pads[8].pan = -18;
         s.ui.fader_cursor = uint8_t(FaderTarget::PadPan);
         s.ui.fader = {100, false, 1};
     }},
    {"fadersynth", [](Shell&, Snapshot& s) { // a synth track with its fader off: the pad targets are for kits
         s.ui.page = PageId::FaderPage; s.ui.track = 2;
         auto& t = s.project.tracks[2];
         t.fader_on = false; t.wheel = 70;
         s.ui.fader_cursor = 2;
     }},
    {"fadercard", [](Shell&, Snapshot& s) { // the fader moved on Mix: the selected track's level
         s.ui.page = PageId::Mix; s.ui.track = 2; s.playing = true;
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
         s.ui.page = PageId::SettingsPage; s.ui.settings_cursor = Settings::FaderTakeover; s.ui.settings_editing = true;
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
    {"cloud", [](Shell&, Snapshot& s) { s.ui.page = PageId::CloudPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         auto& t = s.project.tracks[3]; t.params[sampler::Cloud] = 1; t.last_pad = 4; t.pads[4].start = 0; t.pads[4].end = 230087; s.ui.turning = 0; s.ui.turned_at = s.now; }},
    // Parity phase 10: the sampler's page map (Push) over Play, and Seq + Sample on Patterns.
    {"samplermap", [](Shell&, Snapshot& s) { s.ui.page = PageId::PlayPage; s.ui.track = 3; s.sample = break_sample(); s.playing = true;
         s.ui.map_open = true; s.ui.map_cursor = 7; }},
    {"seqsample", [](Shell&, Snapshot& s) { s.ui.page = PageId::Patterns; s.ui.track = 3; s.playing = true; s.ui.seq_sample = true; }},
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
         s.ui.page = PageId::Pads; s.ui.track = 3; s.ui.shift = true; s.project.click = 2;
     }},
    {"exit", [](Shell&, Snapshot& s) { s.ui.page = PageId::Stage; s.ui.exit_open = true; }},
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
         s.ui.page = PageId::Project; s.playing = true; s.ui.project_cursor = 5;
         preview_projects(s);
         s.ui.name.start(NameTarget::ProjectSave, "Project 1", sizeof(s.project.name) - 1, false);
     }},
    // Parity phase 3. These come last: they write into the shell's patterns, which the previews before them share.
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
    {"notesheet", [](Shell& shell, Snapshot& s) { // Push on the note in focus: B1 CLOSE, B3 DO IT
         roll_bass(shell, s, 24, 57);
         s.ui.sheet_open = true;
     }},
    {"rolledges", [](Shell& shell, Snapshot& s) { // the rows lower: notes above and below are dots on the edges
         roll_bass(shell, s, 30, 55);
         s.ui.roll_top = 63;
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
    require(written > 0, "Unknown Melodee page; use stage stagepreset stage909 stagemidi stageemptysynth tracks keys sounds edit soundmap mod mix steps roll notesheet motion patterns song songzoom clipsheet length rack sends bus fxpick pads samples trim chop levels record project filesheet tempo arp chord capture pad play envelope filter follow cloud padmix padfilter filterenv trimloop trimshift ampenv tune levelsfilter levelssynth shift exit");
    std::cout << "MELODEE_PREVIEW pages=" << written << " text_issues=" << text_issues << '\n';
    // Text under the floor or cut by its box fails the build check.
    return text_issues ? 1 : 0;
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
    // --self-test is what device.py and the SD boot run before stopping the stock app.
    if (argc == 3 && (!std::strcmp(argv[1], "--melodee-sound") || !std::strcmp(argv[1], "--self-test"))) return sound(argv[2]);
    if (argc >= 3 && !std::strcmp(argv[1], "--melodee-bench")) {
        std::vector<std::string> names(argv + 3, argv + argc);
        return bench(unsigned(std::max(1, std::atoi(argv[2]))), names);
    }
    return std::nullopt;
}

} // namespace melodee
