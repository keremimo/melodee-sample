// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/print.h"
#include "melodee/model/project.h"
#include "melodee/shell/pad_context.h"
#include "melodee/shell/pad_light.h"
#include <array>
#include <cstdint>

// The sixteen pads' lights for the context they are in (shell/pad_context.h:
// the one Shell::hit() dispatches on), in the grammar of shell/pad_light.h.
// Pure: the light pass (shell/lights.cpp, the MIDI thread every 5 ms) and
// the tests call it with what the MIDI thread knows. docs/MELODEE.md, Pad
// lights, lists what each context shows.
namespace melodee {

class EngineHost;
struct TrackTaps;

// A pad's sound as last lit, so a sound that stops (a one-shot's end, a choke) fades as a pad let go does
// (Settings > LIGHTS > Pad fade) instead of going dark at once. Kept by the shell across frames; for one track and bank.
struct PadGlow {
    std::array<float, 16> level {};
    std::array<float, 16> levels {}; // 16 Levels' pads, the same
    uint32_t at = 0;  // ms of the frame it was kept
    int owner = -1;   // track * 256 + bank it belongs to
};

struct PadScene {
    const UiState& ui;
    const Project& project;
    const PatternBank& patterns;
    const EngineHost& host;
    const TrackTaps& taps;                    // the selected track's voices (track 255: none)
    PadFacts facts;
    uint32_t now;                             // ms
    const std::array<uint32_t, 16>* let_go;   // when each pad was let go, ms + 1 (0 never): a hand's flare; null none
    uint32_t count_beat_at;                   // when the count-in's beat now began, ms; 0 none
    // Freeze plan F2c: each track's freeze (null: none frozen), and the PRINT sheet's patterns printed so far and the
    // one printing (-1 none), a bit each.
    const std::array<TrackFreeze, track_count>* freeze = nullptr;
    std::array<uint64_t, (pattern_count + 63) / 64> printed {};
    int printing = -1;
    PadGlow* glow = nullptr; // null: a sound's flare follows it exactly (the tests)
};

// Pad 1 (bottom left) first.
void pad_looks(const PadScene& scene, std::array<pad_light::Look, 16>& out);

} // namespace melodee
