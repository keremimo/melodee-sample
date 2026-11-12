// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/looper.h"
#include "melodee/model/project.h"
#include "melodee/shell/comp_trail.h"
#include "melodee/shell/context.h"
#include "melodee/shell/playheads.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/plugin/plugin_rack.h"
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
    uint16_t count = 0; // its notes, for the question before it is cleared
    uint16_t moves = 0; // its Motion points (roadmap P1: the pattern's own), the same
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
    unsigned tempo = 1200;                   // tenths of a BPM, as it plays (BPM per pattern or global, or MIDI clock in's)
    bool following = false;                  // MIDI clock in leads the transport (parity phase 9)
    bool following_mtc = false;              // and MTC in (parity phase 9b)
    std::array<uint8_t, track_count> playing_patterns {};
    uint32_t ticks = 0;                      // 96 a quarter since Play
    bool song_playing = false;
    unsigned song_bar = 0;
    std::array<float, track_count> meters {};
    // The voices of the track in view as the engine left them (engine/voice_tap.h), output frames from their stamp
    // to the frame heard now, and each voice's head where it is heard, newest first (shell/playheads.h).
    TrackTaps taps;
    double tap_frames = 0;
    std::array<PlayHead, HeadTrail::size> heads {};
    std::array<GrainPlace, GrainTrail::size> grains {}; // a cloud's grains heard now, and the marks of those just gone
    float compression = 0;                   // dB the master compressor takes off now (parity phase 7)
    std::array<CompColumn, comp_columns> comp_trail {}; // what it heard and took off, a column every comp_column_ms, oldest first
    unsigned flex_playing = 0;               // Flex Beat's pad playing, 0 Empty (parity phase 7b)
    std::array<std::array<PatternSketch, pattern_count>, track_count> sketches {};
    LoadedSample sample;               // the sample in hand: the last pad's, or Chop's
    struct Take {
        uint8_t state = 0;             // TakeRecorder::State
        uint32_t frames = 0, length = 0;
        float level = 0, left = 0, right = 0; // the input's peak, and each side's
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
    PluginRack::View plugins;          // the PLUG-INS picker: the folder listed
    PluginPrograms presets;            // PRESETS: the plug-in's programs
    ProjectStore::View projects;       // Project page and Exit
    std::string project_path, project_folder;
    bool dirty = false;
    std::array<int16_t, 16> held_keys {};         // Arp: the first key of each pad held
    uint32_t now = 0;                        // ms, for transient highlights
    // The Looper (docs/LOOPER.md): each loop as the engine shows it, its waveform and its track's meter; the
    // engine on and its clock (ticks), for bar.beat.
    struct LoopShown {
        Looper::View view;
        std::array<uint8_t, looper::peak_columns> peaks {};
        float meter = 0;
    };
    std::array<LoopShown, loop_tracks> loops {};
    bool looper_on = false;
    double loop_tick = 0;
    // The audio device as it runs now (the app fills these from the runtime's session): Settings shows them.
    unsigned audio_rate = 44100, audio_periods = 3, audio_xruns = 0;
    // About (round 2, about C): how the machine is doing, filled by the app (melodee_app.cpp).
    struct Machine {
        std::array<uint8_t, 8> core_load {}; // each core busy, percent, over the last half second
        uint8_t cores = 0;                   // 0: not read yet
        float render_load = 0;               // the audio blocks' share of their time (the recent peak)
        bool card = false;                   // a card is in and Melodee's
        uint64_t card_free = 0, card_total = 0;
        unsigned stream_late = 0;            // times the card could not stream a sample in time (an SFZ's)
        uint8_t plugins = 0;                 // plug-ins loaded
        float plugins_load = 0;              // their share of the audio time
    } machine;
    const Track& track() const { return project.tracks[ui.track % track_count]; }
};

} // namespace melodee
