// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
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
};

// Pad 1 (bottom left) first.
void pad_looks(const PadScene& scene, std::array<pad_light::Look, 16>& out);

} // namespace melodee
