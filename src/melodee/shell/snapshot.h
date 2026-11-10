// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/context.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/ui_state.h"
#include <array>
#include <cstdint>

// Everything a page needs to draw one frame, copied under a short lock so
// drawing itself never holds up the MIDI thread.
namespace melodee {

// A pattern in small, for the Patterns grid and the Song's clips: its length
// in ticks and the first note of each of its first 64 sixteenths (0: none).
struct PatternSketch {
    uint32_t length = 16 * sixteenth_ticks;
    bool notes = false;
    std::array<uint8_t, 64> keys {};
};

struct Snapshot {
    UiState ui;
    Project project;
    Pattern pattern;                         // the selected track's pattern playing
    std::array<bool, track_count> has_notes {}; // the playing pattern of each track holds notes
    bool playing = false;
    uint32_t position = 0;                   // where the selected track is in its pattern, ticks
    std::array<int, track_count> positions {}; // each track's, -1 none
    unsigned count_in = 0;                   // the Count-In's beat, 0 none
    unsigned tempo = 1200;                   // tenths of a BPM, as it plays (BPM per pattern or global)
    std::array<uint8_t, track_count> playing_patterns {};
    uint32_t ticks = 0;                      // 96 a quarter since Play
    bool song_playing = false;
    unsigned song_bar = 0;
    std::array<float, track_count> meters {};
    float compression = 0;                   // dB the master compressor takes off now (parity phase 7)
    std::array<std::array<PatternSketch, pattern_count>, track_count> sketches {};
    LoadedSample sample;               // the sample in hand: the last pad's, or Chop's
    struct Take {
        uint8_t state = 0;             // TakeRecorder::State
        uint32_t frames = 0, length = 0;
        float level = 0;
        std::array<uint8_t, 128> peaks {};
    } take;
    // Parity phase 6: a render (Bounce::State, frames so far), what Audio Recall holds,
    // whether the source is heard, the speaker quieted by the monitored mic, USB-C input.
    uint8_t render_state = 0;
    uint32_t render_frames = 0;
    uint32_t recall_frames = 0;
    bool monitoring = false, speaker_quieted = false;
    uint8_t usb_state = 0;             // UsbStreamState
    std::vector<SampleFile> files;     // Samples: the folder listed
    std::string sample_root;           // the card's Samples folder; empty: no card
    std::string files_folder;          // the folder listed (empty: the internal drive's categories)
    bool files_top = true;             // nothing above it (B1 has no job)
    ProjectStore::View projects;       // Project page and Exit
    std::string project_path, project_folder;
    bool dirty = false;
    std::array<Context::Played, 512> history {}; // Capture
    unsigned history_at = 0;
    std::array<int16_t, 16> held_keys {};         // Arp: the first key of each pad held
    uint32_t now = 0;                        // ms, for transient highlights
    const Track& track() const { return project.tracks[ui.track % track_count]; }
};

} // namespace melodee
