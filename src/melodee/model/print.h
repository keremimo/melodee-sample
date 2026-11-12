// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Prints (freeze plan F2a): one pattern of one track rendered to a WAV file in
// "<project> Prints/" beside the project, for a frozen track to play instead
// of its engine and chain. Each print keeps a record in the project (PRNT):
// the track and pattern, the file, its length, where the sound was taken
// (tap), how the pattern's end was handled (tail) and a hash of everything
// that made it, so an unchanged pattern is never printed twice. The files are
// 32-bit float stereo at the device's rate as it printed them (never
// converted); a print made at another rate counts as changed.
namespace melodee {

// PostFx: after the track's chain (its level, pan, sends, mute and punches stay live); PreFx: the engine alone.
enum class PrintTap : uint8_t { PostFx, PreFx };
// Loop: two passes rendered, the second kept, so a tail wraps into the start as when the pattern loops; Ring: one
// pass and then its tail until it falls silent (10 s at most); Cut: one pass, nothing after.
enum class PrintTail : uint8_t { Loop, Ring, Cut };
inline const char* tap_name(PrintTap t) { return t == PrintTap::PreFx ? "pre" : "post"; }
inline const char* tail_name(PrintTail t) { return t == PrintTail::Ring ? "ring" : t == PrintTail::Cut ? "cut" : "loop"; }

struct PrintRecord {
    uint8_t track = 0, pattern = 0;
    PrintTap tap = PrintTap::PostFx;
    PrintTail tail = PrintTail::Loop;
    uint32_t rate = 44100; // the file's, the device's when it was printed
    uint32_t frames = 0;   // the file's, at `rate`
    uint32_t pass = 0;     // one pass of the pattern, in 44.1 kHz frames as the project counts (Ring: the file runs on past it)
    uint64_t hash = 0;     // print_hash() of what made it
    std::string file;      // its name in the Prints folder
    bool operator==(const PrintRecord& o) const {
        return track == o.track && pattern == o.pattern && tap == o.tap && tail == o.tail && rate == o.rate && frames == o.frames &&
               pass == o.pass && hash == o.hash && file == o.file;
    }
};

// Freeze plan F2b: a track frozen plays its prints instead of its engine and chain (engine/print_player.h); a
// deep freeze (F2d) also unloads them. Each pattern frozen plays its print; the rest of the track's patterns play
// live. Its prints are taken at `tap` and end as `tail` says.
enum class FreezeState : uint8_t { None, Frozen, Deep };
struct TrackFreeze {
    FreezeState state = FreezeState::None;
    PrintTap tap = PrintTap::PostFx;
    PrintTail tail = PrintTail::Loop;
    std::array<uint64_t, (pattern_count + 63) / 64> patterns {}; // a bit a pattern frozen
    bool frozen() const { return state != FreezeState::None; }
    bool plays(unsigned pattern) const { pattern %= pattern_count; return (patterns[pattern / 64] >> (pattern % 64)) & 1; }
    void set(unsigned pattern, bool on) {
        pattern %= pattern_count;
        uint64_t bit = uint64_t(1) << (pattern % 64);
        patterns[pattern / 64] = on ? patterns[pattern / 64] | bit : patterns[pattern / 64] & ~bit;
    }
    bool operator==(const TrackFreeze& o) const { return state == o.state && tap == o.tap && tail == o.tail && patterns == o.patterns; }
};

// A project's prints, a record a track and pattern, kept in that order; and each track's freeze (FRZN).
struct PrintTable {
    std::vector<PrintRecord> records;
    std::array<TrackFreeze, track_count> freeze {};
    const PrintRecord* find(unsigned track, unsigned pattern) const;
    void put(const PrintRecord& record); // in its place; one for the same track and pattern is replaced
    bool operator==(const PrintTable& o) const { return records == o.records && freeze == o.freeze; }
};

// `project` and `bank` (copies) made into what a print of pattern `pattern` of track `track` plays: that track alone
// in its place, playing that pattern and no other, its level at unity, pan centred, no sends, mute or solo, no
// chain of patterns; the pattern without its kept punches, follow action and mix lanes (level, pan, sends, the
// master's and the returns' values), which stay live; the project's tempo the one that pattern plays at; no buses'
// or master's effects, Knob FX, master compressor, Looper, macros or song; only the samples, chops, plug-ins and
// modulator targets the track uses. What the screen keeps of the track (its name, last pad and note, fader) is
// cleared too, so it does not move the hash.
void print_project(Project& project, PatternBank& bank, unsigned track, unsigned pattern);

// What made a print: a print_project() and the plug-ins' states (plugin/plugin_rack.h; only the entries it uses
// count), with its tap, tail and the device's rate. 64-bit FNV-1a over the project file save_project() writes of them.
uint64_t print_hash(const Project& project, const PatternBank& bank, const std::array<std::vector<uint8_t>, max_plugins>& states,
                    PrintTap tap, PrintTail tail, unsigned rate);

// The folder beside a project file that holds its prints ("" without a file), and a print's file name in it.
std::string prints_folder(const std::string& project_path);
std::string print_file_name(unsigned track, unsigned pattern);
// A pattern's name as the pages give it: its bank's letter and its pad, A1 .. H16.
std::string print_pattern_name(unsigned pattern);

} // namespace melodee
