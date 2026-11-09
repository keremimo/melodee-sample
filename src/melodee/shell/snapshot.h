// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/ui_state.h"
#include <array>
#include <cstdint>

// Everything a page needs to draw one frame, copied under a short lock so
// drawing itself never holds up the MIDI thread.
namespace melodee {

// A pattern in small, for the Patterns grid and the Song's clips: its length,
// division and the first note of each of its first 64 steps (0: none).
struct PatternSketch {
    uint16_t length = 16;
    Division division = Division::Sixteenth;
    bool notes = false;
    std::array<uint8_t, 64> keys {};
};

struct Snapshot {
    UiState ui;
    Project project;
    Pattern pattern;                         // the selected track's pattern playing
    std::array<bool, track_count> has_notes {}; // the playing pattern of each track holds notes
    bool playing = false;
    uint32_t position = 0;                   // the selected track's step playing
    std::array<int, track_count> steps {};   // each track's step playing, -1 none
    std::array<uint8_t, track_count> playing_patterns {};
    uint32_t ticks = 0;                      // 24 a quarter since Play
    bool song_playing = false;
    unsigned song_bar = 0;
    std::array<float, track_count> meters {};
    std::array<std::array<PatternSketch, pattern_count>, track_count> sketches {};
    LoadedSample sample;               // the sample in hand: the last pad's, or Chop's
    struct Take {
        uint8_t state = 0;             // TakeRecorder::State
        uint32_t frames = 0, length = 0;
        float level = 0;
        std::array<uint8_t, 128> peaks {};
    } take;
    std::vector<SampleFile> files;     // Samples
    std::string sample_root;
    uint32_t now = 0;                        // ms, for transient highlights
    const Track& track() const { return project.tracks[ui.track % track_count]; }
};

} // namespace melodee
