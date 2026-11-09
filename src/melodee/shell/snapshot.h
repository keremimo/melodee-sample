// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include <array>
#include <cstdint>

// Everything a page needs to draw one frame, copied under a short lock so
// drawing itself never holds up the MIDI thread.
namespace melodee {

struct Snapshot {
    UiState ui;
    Project project;
    Pattern pattern;                         // the selected track's pattern playing
    std::array<bool, track_count> has_notes {}; // the playing pattern of each track holds notes
    bool playing = false;
    uint32_t position = 0;                   // steps since Play
    std::array<float, track_count> meters {};
    uint32_t now = 0;                        // ms, for transient highlights
    const Track& track() const { return project.tracks[ui.track % track_count]; }
};

} // namespace melodee
