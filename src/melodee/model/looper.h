// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include <array>
#include <cstdint>

// The Looper's values as a project keeps them (docs/LOOPER.md): on or off,
// the three settings every loop shares, and each loop's track L1-L4 (its mix
// strip and chain live with Project, model/project.h) with what its loop
// records and how. The audio itself is the engine's (engine/looper.h); a
// saved loop's length, tempo and layers come back with its file.
namespace melodee {

namespace looper {
// Set lengths, bars (0 free).
constexpr std::array<uint8_t, 6> lengths {0, 1, 2, 4, 8, 16};
enum class Quantize : uint8_t { Bar, Beat, Off, Count };
enum class Then : uint8_t { Play, Overdub, Stop, Count };     // after the first take
enum class Transport : uint8_t { Follow, Free, Count };     // stopped: the loops hold, or keep time on their own
// What a loop records: the input, a track (1-16), another loop's track or the mix.
constexpr uint8_t source_input = 0;
constexpr uint8_t source_track(unsigned t) { return uint8_t(1 + t); }
constexpr uint8_t source_loop(unsigned l) { return uint8_t(1 + track_count + l); }
constexpr uint8_t source_mix = uint8_t(1 + track_count + loop_tracks);
constexpr unsigned source_count = source_mix + 1u;
constexpr bool source_is_track(uint8_t s) { return s >= 1 && s <= track_count; }
constexpr bool source_is_loop(uint8_t s) { return s > track_count && s < source_mix; }
inline const char* quantize_name(unsigned q) { static const char* const n[] {"Bar", "Beat", "Off"}; return n[q % 3]; }
inline const char* then_name(unsigned t) { static const char* const n[] {"Play", "Overdub", "Stop"}; return n[t % 3]; }
} // namespace looper

struct LoopTrack {
    uint8_t bars = 0;        // the set length: 0 free, else one of looper::lengths
    uint8_t feedback = 100;  // percent of the old layers an overdub pass keeps
    uint8_t source = looper::source_input;
    // A saved loop (L4): its frames at 44.1 kHz, the ticks they span, the tempo it was recorded at (tenths of a BPM)
    // and its layers. frames 0: no loop kept.
    uint32_t frames = 0;
    double ticks = 0;
    uint16_t tempo = 0;
    uint8_t layers = 0;
    bool stopped = false; // saved stopped (a loop committed to a pad stops): it comes back stopped
};

struct LooperSettings {
    bool on = false;
    uint8_t quantize = uint8_t(looper::Quantize::Bar);
    uint8_t then = uint8_t(looper::Then::Play);
    uint8_t transport = uint8_t(looper::Transport::Follow);
    std::array<LoopTrack, loop_tracks> loops {};
};

} // namespace melodee
