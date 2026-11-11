// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/demo_projects.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/scales.h"
#include <cstdio>
#include <cstring>

namespace melodee {
namespace {
void note(Pattern& p, unsigned step, uint8_t key, uint8_t velocity, unsigned length = 1) {
    Event e;
    e.tick = step * sixteenth_ticks; e.length = uint16_t(length * sixteenth_ticks); e.key = key; e.velocity = velocity;
    p.add(e);
}
void bars(Pattern& p, unsigned count) { p.length = count * 16 * sixteenth_ticks; }
void marker(Song& song, uint16_t bar, const char* name) {
    if (song.marker_count >= song.markers.size()) return;
    auto& m = song.markers[song.marker_count++];
    m.bar = bar;
    std::strncpy(m.name, name, sizeof(m.name) - 1);
}
void clip(Song& song, uint8_t track, uint8_t pattern, uint16_t bar, uint16_t count) {
    Clip c; c.track = track; c.pattern = pattern; c.bar = bar; c.bars = count;
    song.place(c);
}
// 909 keys: 36 + the drum (engine/kit909.h): kick, snare, clap, rim, toms, closed and open hat, crash, ride.
constexpr uint8_t kick = 36, snare = 37, clap = 38, rim = 39, closed = 43, open = 44;

// Night Drive: 104 BPM in A minor; CZ bass, PROPHET pad chords, SID arp, 909; two patterns each and a song.
void night_drive(Project& project, PatternBank& patterns) {
    project = Project {};
    std::snprintf(project.name, sizeof(project.name), "Night Drive");
    project.tempo = 1040; project.root = 9; // A
    load_sound(project.tracks[0], EngineKind::Cz, 2);      // CZ BASS
    load_sound(project.tracks[1], EngineKind::Prophet, 9); // STRING MACHINE
    load_sound(project.tracks[2], EngineKind::Sid, 3);     // ARP PLUCK
    load_sound(project.tracks[3], EngineKind::Kit909, 1);  // TIGHT KIT
    project.tracks[1].mix.level = 78; project.tracks[2].mix.level = 84; project.tracks[2].mix.pan = 18;
    patterns = PatternBank {};
    // Bass: A, then F and G under the chorus pattern.
    const uint8_t verse[] {33, 33, 45, 33, 36, 33, 43, 33};
    for (unsigned n = 0; n < 16; ++n) note(patterns.at[0][0], n * 2, verse[n % 8], n % 4 == 0 ? 116 : 84);
    bars(patterns.at[0][0], 2);
    const uint8_t chorus[] {29, 29, 41, 29, 31, 31, 43, 31};
    for (unsigned n = 0; n < 16; ++n) note(patterns.at[0][1], n * 2, chorus[(n / 2) % 8], n % 4 == 0 ? 120 : 88);
    bars(patterns.at[0][1], 2);
    // Pad: Am, then F and G, a chord a bar.
    auto chord = [&](Pattern& p, unsigned bar, std::initializer_list<uint8_t> keys) { for (auto k : keys) note(p, bar * 16, k, 92, 16); };
    chord(patterns.at[1][0], 0, {57, 60, 64}); chord(patterns.at[1][0], 1, {57, 60, 64}); bars(patterns.at[1][0], 2);
    chord(patterns.at[1][1], 0, {53, 57, 60}); chord(patterns.at[1][1], 1, {55, 59, 62}); bars(patterns.at[1][1], 2);
    // Arp: up the chord in sixteenths.
    const uint8_t up[] {69, 72, 76, 81};
    for (unsigned s = 0; s < 32; ++s) note(patterns.at[2][1], s, up[s % 4], s % 4 == 0 ? 100 : 70);
    bars(patterns.at[2][1], 2);
    // Drums: four on the floor, claps on two and four, hats; the chorus adds open hats.
    for (unsigned p = 0; p < 2; ++p) {
        auto& d = patterns.at[3][p];
        for (unsigned s = 0; s < 32; s += 4) note(d, s, kick, 120);
        for (unsigned s = 4; s < 32; s += 8) note(d, s, clap, 104);
        for (unsigned s = 0; s < 32; s += 2) note(d, s, closed, s % 4 == 2 ? 92 : 60);
        if (p == 1) for (unsigned s = 2; s < 32; s += 4) note(d, s, open, 80);
        bars(d, 2);
    }
    auto& song = project.song;
    for (uint8_t t : {0, 1, 3}) clip(song, t, 0, 0, 8);
    for (uint8_t t : {0, 1, 2, 3}) clip(song, t, 1, 8, 8);
    for (uint8_t t : {0, 1, 3}) clip(song, t, 0, 16, 8);
    marker(song, 0, "VERSE"); marker(song, 8, "CHORUS"); marker(song, 16, "OUTRO");
}

// Broken Tape: 86 BPM swung, FM6 keys over a PROPHET sub bass and a dusty 909, tape on the master.
void broken_tape(Project& project, PatternBank& patterns) {
    project = Project {};
    std::snprintf(project.name, sizeof(project.name), "Broken Tape");
    project.tempo = 860; project.swing = 60; project.root = 5; // F
    load_sound(project.tracks[0], EngineKind::Prophet, 8); // SUB BASS
    load_sound(project.tracks[1], EngineKind::Fm6, 1);     // TINE EP
    load_sound(project.tracks[2], EngineKind::Kit909, 4);  // DIRTY KIT
    load_sound(project.tracks[3], EngineKind::Cz, 1);      // GLASS PAD
    project.tracks[3].mix.level = 70; project.tracks[1].mix.send = 30;
    auto& slot = project.master.slots[0]; // tape on the master
    slot.kind = uint8_t(EffectKind::Tape);
    const auto& info = effect_info(EffectKind::Tape);
    for (unsigned q = 0; q < effect_params; ++q) slot.params[q] = info.params[q].label ? info.params[q].initial : 0;
    patterns = PatternBank {};
    note(patterns.at[0][0], 0, 29, 118, 6); note(patterns.at[0][0], 7, 29, 90, 2); note(patterns.at[0][0], 10, 32, 100, 4);
    note(patterns.at[0][0], 16, 27, 118, 6); note(patterns.at[0][0], 26, 24, 96, 4);
    bars(patterns.at[0][0], 2);
    // Keys: Fm9 and Ebmaj7 stabs off the beat.
    for (unsigned s : {2u, 6u, 11u}) for (uint8_t k : {56, 60, 63, 67}) note(patterns.at[1][0], s, k, 84, 2);
    for (unsigned s : {18u, 22u, 27u}) for (uint8_t k : {55, 58, 62, 65}) note(patterns.at[1][0], s, k, 80, 2);
    bars(patterns.at[1][0], 2);
    auto& d = patterns.at[2][0];
    for (unsigned s : {0u, 10u, 16u, 19u, 26u}) note(d, s, kick, 118);
    for (unsigned s : {4u, 12u, 20u, 28u}) note(d, s, snare, 108);
    for (unsigned s = 0; s < 32; ++s) note(d, s, s % 2 ? rim : closed, s % 2 ? 40 : 76);
    bars(d, 2);
    note(patterns.at[3][0], 0, 65, 70, 32); note(patterns.at[3][0], 0, 72, 64, 32); bars(patterns.at[3][0], 2);
}
} // namespace

// A new project: one empty kit on track 1, every note in reach.
void starting_project(Project& project, PatternBank& patterns) {
    project = Project {};
    std::snprintf(project.name, sizeof(project.name), "Untitled");
    project.tempo = 1200;
    project.root = 0; project.scale = chromatic_scale; // C Chromatic
    load_sound(project.tracks[0], EngineKind::Sampler, 0); // Sample Select fills its pads
    patterns = PatternBank {};
}

namespace {
void melodee_starter(Project& project, PatternBank& patterns) {
    project = Project {};
    project.tempo = 1200;
    load_sound(project.tracks[0], EngineKind::Prophet, 1);  // FUNK BASS II
    load_sound(project.tracks[1], EngineKind::Prophet, 5);  // SOFT PAD
    load_sound(project.tracks[2], EngineKind::Prophet, 6);  // GLASS PLUCK
    load_sound(project.tracks[3], EngineKind::Prophet, 3);  // SYNC LEAD
    load_sound(project.tracks[4], EngineKind::Sampler, 0); // an empty kit: Sample Select fills its pads
    load_sound(project.tracks[5], EngineKind::Kit909, 0);  // drums, with a beat
    project.tracks[1].mix.level = 80; project.tracks[1].octave = 3;
    project.tracks[2].octave = 4; project.tracks[3].octave = 4;
    patterns = PatternBank {};
    // A bass line in D for track 1, so Play has something to play.
    struct { unsigned step; uint8_t key, length, velocity; } line[] {
        {0, 38, 2, 110}, {3, 38, 1, 70}, {4, 41, 1, 96}, {6, 36, 2, 96}, {8, 33, 3, 120},
        {11, 36, 1, 64}, {12, 38, 2, 104}, {14, 31, 2, 88},
    };
    for (auto& n : line) note(patterns.at[0][0], n.step, n.key, n.velocity, n.length);
    // Track 6: kick on the beat, snare on two and four, closed hats on the off-beats.
    auto& beat = patterns.at[5][0];
    for (unsigned st = 0; st < 16; st += 4) note(beat, st, kick, 120, 1);
    for (unsigned st : {4u, 12u}) note(beat, st, snare, 110, 1);
    for (unsigned st = 2; st < 16; st += 4) note(beat, st, closed, 90, 1);
}
} // namespace

const char* demo_name(unsigned n) {
    static const char* names[demo_count] {"Melodee Starter", "Night Drive", "Broken Tape"};
    return names[n % demo_count];
}

void make_demo(unsigned n, Project& project, PatternBank& patterns) {
    switch (n % demo_count) {
    case 1: night_drive(project, patterns); break;
    case 2: broken_tape(project, patterns); break;
    default: melodee_starter(project, patterns); std::snprintf(project.name, sizeof(project.name), "Melodee Starter"); break;
    }
}

int kit_track(const Project& from) {
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& tr = from.tracks[t];
        if (tr.engine != EngineKind::Sampler) continue;
        for (const auto& pad : tr.pads) if (pad.used()) return int(t);
    }
    return -1;
}

KitImport import_kit(const Project& from, unsigned track, Track& into, const std::function<int(const SampleRef&)>& slot_for) {
    KitImport result;
    const auto& src = from.tracks[track % track_count];
    into.engine = EngineKind::Sampler;
    into.preset = src.preset;
    into.params = src.params;
    into.set_name(src.name);
    into.pad_fx = src.pad_fx;
    into.pad_mutes = {};
    into.bank = 0; into.last_pad = 0; into.levels = false;
    std::array<int, sample_slots> mapped;
    mapped.fill(-2); // -2 not asked yet
    for (unsigned n = 0; n < sampler_pads; ++n) {
        PadSound pad = src.pads[n];
        if (pad.used()) {
            unsigned s = pad.sample % sample_slots;
            if (mapped[s] == -2) mapped[s] = from.samples[s].used() ? slot_for(from.samples[s]) : -1;
            if (mapped[s] < 0) { pad = PadSound {}; into.pad_fx.at[n] = Chain {{}, pad_slots}; ++result.missing; }
            else { pad.sample = uint16_t(mapped[s]); ++result.pads; }
        }
        into.pads[n] = pad;
    }
    return result;
}

KitImport import_bank(const BankFile& from, unsigned bank, Track& into, const std::function<int(const SampleRef&)>& slot_for) {
    KitImport result;
    std::array<int, bank_pads> mapped;
    mapped.fill(-2); // -2 not asked yet
    for (unsigned k = 0; k < bank_pads; ++k) {
        unsigned n = (bank % sampler_banks) * bank_pads + k;
        PadSound pad = from.pads[k];
        Chain chain = from.chains[k];
        if (pad.used()) {
            unsigned s = pad.sample % bank_pads;
            if (mapped[s] == -2) mapped[s] = from.samples[s].used() ? slot_for(from.samples[s]) : -1;
            if (mapped[s] < 0) { pad = PadSound {}; ++result.missing; }
            else { pad.sample = uint16_t(mapped[s]); ++result.pads; }
        }
        if (!pad.used()) chain = Chain {{}, pad_slots};
        into.pads[n] = pad;
        into.pad_fx.at[n] = chain;
        into.set_pad_muted(n, false);
    }
    return result;
}

} // namespace melodee
