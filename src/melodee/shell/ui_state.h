// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/project.h"
#include <array>
#include <cstdint>

// What the user interface remembers between controls: the page, the
// selected track, held buttons, cursors and the pots' pickup. The MIDI
// thread owns it; the screen gets a copy in each Snapshot.
namespace melodee {

enum class PageId : uint8_t { Stage, Keys, Mix, Sounds, Edit, Steps, Roll, Motion, Patterns, Song, Length, Count };

// Pot pickup: a pot changes its value only once it has reached it, so a
// page change never makes a value jump.
struct Pot {
    int16_t position = -1; // 0..127 where the pot sits; -1 until known
    bool engaged = false;
    uint32_t bound = 0;    // which binding it was last checked against
};

struct UiState {
    PageId page = PageId::Stage, previous = PageId::Stage;
    uint8_t track = 0;
    bool shift = false, shift_used = false; // Shift held; another control used during the hold
    bool pad_bank = false, mute = false, erase = false;
    bool exit_open = false, recording = false;
    std::array<uint8_t, track_count> edit_page {}, edit_row {};
    uint16_t sounds_cursor = 0;
    uint8_t sounds_filter = 0;       // all, favourites, recent
    uint8_t audition_velocity = 100;
    int16_t auditioning = -1;        // the sound heard on the pads while browsing, -1 for the loaded one
    std::array<uint16_t, 8> recent {}; // recently loaded sounds of the track's engine, newest first
    uint8_t recent_count = 0;
    uint8_t steps_bar = 0;
    int16_t step_cursor = 0, held_step = -1;
    uint32_t held_since = 0;                  // when the held step's pad went down, ms
    bool held_created = false, held_edited = false;
    bool sheet_open = false;  // the note sheet over Roll
    uint8_t sheet_row = 0;
    uint8_t roll_note = 0;    // which of the cursor step's notes is selected
    bool map_open = false;    // the page map over Edit
    uint8_t map_cursor = 0;
    int8_t pattern_held = -1; // Patterns: a pad held, so the next one copies it
    bool pattern_copied = false;
    uint16_t song_cursor = 0;   // bar
    uint16_t song_view = 0;     // first bar in view
    uint8_t song_zoom = 2;      // index into song_zooms
    uint8_t song_pattern = 0;   // what Push places on empty time
    uint8_t song_length = 4;    // bars
    std::array<Pot, 3> pots {};
    int8_t turning = -1;      // the card turned last
    uint32_t turned_at = 0;   // when, in ms
    int8_t last_pot = -1;     // for Shift + wheel fine edits
    uint32_t held_pads = 0;
};

} // namespace melodee
