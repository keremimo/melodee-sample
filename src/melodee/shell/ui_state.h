// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/project.h"
#include "melodee/shell/chop.h"
#include "melodee/shell/name_entry.h"
#include "melodee/shell/settings.h"
#include <array>
#include <cstdint>
#include <string>

// What the user interface remembers between controls: the page, the
// selected track, held buttons, cursors and the pots' pickup. The MIDI
// thread owns it; the screen gets a copy in each Snapshot.
namespace melodee {

enum class PageId : uint8_t { Stage, Keys, Mix, Sounds, Edit, Steps, Roll, Motion, Patterns, Song, InputConfig, Rack, Sends, Bus, Master, Pads, Samples, Trim, Chop, Levels, Record, Project, Tempo, Arp, Chord, Capture, PadPage, PlayPage, EnvelopePage, FilterPage, FollowPage, CloudPage, Punch, PadPunch, SettingsPage, About, PadFx, TimeCorrect, Count };

// Pot pickup: a pot changes its value only once it has reached it, so a
// page change never makes a value jump.
struct Pot {
    int16_t position = -1; // 0..127 where the pot sits; -1 until known
    bool engaged = false;
    uint32_t bound = 0;    // which binding it was last checked against
};

// Whether Samples lists a file under its filter: everything, the takes, or
// the files the project already holds in its sample slots.
inline bool sample_file_listed(const Project& project, unsigned filter, const std::string& path) {
    if (filter == 0) return true;
    if (filter == 1) return path.find("/Recorded/") != std::string::npos;
    for (const auto& ref : project.samples) if (ref.used() && path == ref.path) return true;
    return false;
}

struct UiState {
    PageId page = PageId::Stage;
    // Back (Shift + Push): the pages opened before this one, oldest first.
    // Opening a page already in it goes back to it; turning through a
    // group of sibling pages (Steps, Roll, Motion) replaces the top.
    static constexpr unsigned back_capacity = 16;
    std::array<PageId, back_capacity> back_stack {};
    uint8_t back_depth = 0;
    uint8_t track = 0;
    bool shift = false, shift_used = false; // Shift held; another control used during the hold
    bool pad_bank = false, mute = false, erase = false; // held
    bool pad_bank_used = false;  // Pad Bank held did something (a pad, -/+, the wheel): its release steps no bank
    // Mute mode (shell/mute_mode.cpp): a tap latches it, a hold lasts while held.
    bool mute_latch = false, mute_used = false;
    uint32_t mute_down = 0;
    bool mute_mode() const { return mute || mute_latch; }
    // Note Repeat (shell/note_repeat.cpp): a tap latches, a hold lasts while held, a double tap opens Arp.
    bool repeat_held = false, repeat_turned_on = false;
    uint32_t repeat_down = 0, repeat_tap_at = 0;
    bool exit_open = false, recording = false;
    bool rec_armed = false;        // Seq Record pressed while stopped: Play starts the take (its light flashes)
    uint8_t lead_pattern = 0;      // the pattern launched last: whose tempo plays with BPM per pattern on
    uint32_t locate = 0;           // Patterns: where Shift + Play carries on from, ticks (the wheel moves it)
    bool b1_held = false;          // Patterns: B1 held, the wheel sets the BPM
    bool bpm_tenths = false;       // and Push while it is held: whole BPM or tenths
    // Time Correct (Shift + Pad 14): shift in ticks and swing for the notes moved, and the keys chosen.
    int16_t tc_shift = 0;
    uint8_t tc_swing = 50;
    std::array<uint64_t, 3> tc_keys {}; // a bit a key, 0..191 (kit keys run to 163)
    bool tc_all = true;                 // B2: every note of the pattern, whatever the pads chose
    bool tc_key(unsigned key) const { return key < 192 && ((tc_keys[key / 64] >> (key % 64)) & 1); }
    void tc_toggle(unsigned key) { if (key < 192) tc_keys[key / 64] ^= uint64_t(1) << (key % 64); }
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
    bool launch_column = false; // Patterns, B3: a pad launches that column on every track
    uint16_t song_cursor = 0;   // bar
    uint16_t song_view = 0;     // first bar in view
    uint8_t song_zoom = 2;      // index into song_zooms
    uint8_t song_pattern = 0;   // what Push places on empty time
    uint8_t song_length = 4;    // bars
    // Per chain: the slot (or + ADD) and parameter row; the pad chains share the last entry (cursor_of()).
    std::array<uint8_t, chain_count + 1> fx_cursor {}, fx_row {};
    static unsigned cursor_of(unsigned chain) { return chain < chain_count ? chain : chain_count; }
    bool picker_open = false, picker_items = false;           // the effect picker; focus on its effects column
    uint8_t picker_family = 0, picker_item = 0;
    bool picker_replaces = false;                              // Change on a device, not + ADD
    uint16_t files_cursor = 0;  // Samples
    uint16_t files_count = 0;   // files the library lists, copied in by the shell
    uint8_t trim_zoom = 0;      // Trim: 0 whole sample .. 4 sixteen times closer
    ChopMode chop_mode = ChopMode::Hits;
    std::array<int, 3> chop_amount {{60, 16, 1}}; // per mode: sensitivity, slice count, beats a slice
    Slices slices;              // Chop's markers on the sample in hand
    uint16_t chop_sample = no_sample;
    uint8_t chop_slice = 0;
    int8_t levels_hit = -1;     // Levels: the pad played last, 0..15
    int16_t levels_value = 0;   // and what it played: a velocity or semitones
    uint8_t files_filter = 0;   // Samples: all, recorded, recent (the files this project uses)
    uint8_t rec_source = 1;     // Input Config: 0 mic, 1 line, 2 the mix (resample)
    int8_t rec_threshold = -30; // Input Config, dB; -60 starts at once
    uint8_t rec_bars = 4;       // 0: until stopped
    int8_t rec_gain = 0;        // dB
    int16_t rec_slot = -1, rec_pad = -1; // where the take in progress will land
    uint16_t project_cursor = 0;  // Project: row under the cursor (0 is New project)
    bool delete_armed = false;    // the file sheet's Delete asks once more
    uint8_t project_sort = 0;     // 0 newest first, 1 by name
    int8_t chord_pad = -1;        // Chord: the pad played last
    uint8_t sampler_row = 0;      // Envelope and Cloud: their second row
    uint8_t capture_bars = 2, capture_quantise = 1, capture_into = 0; // Capture
    uint16_t project_rows = 1;    // rows on the Project page, copied in by the shell
    int8_t punch_held = -1, punch_locked = -1; // Punch: the punch a pad holds, the one Shift + pad locked on
    int8_t pad_punch_held = -1;               // Pad Punch: the kit pad held, punching
    uint8_t pad_punch_pressure = 0;           // and how hard, 0..127
    Settings settings;                        // Melodee's own, from the card
    NameEntry name;                           // the Name sheet, over any page while open
    bool move_open = false;                   // Project: the folder picker for Move to folder
    uint16_t move_cursor = 0;
    uint16_t user_sound_count = 0;            // sounds of one's own for the track's engine, copied in by the shell
    uint8_t settings_cursor = 1;
    bool settings_editing = false;            // Push opened the value under the cursor
    std::array<Pot, 3> pots {};
    int8_t turning = -1;      // the card turned last
    uint32_t turned_at = 0;   // when, in ms
    int8_t last_pot = -1;     // for Shift + wheel fine edits
    bool last_pot_shift = false; // and whether it turned its Shift card
    uint32_t held_pads = 0;
    // Shift + Erase: copy one kit pad onto another, or (on Patterns) one
    // pattern onto another, or (Pad Bank held) one track onto another.
    // Armed, then the source, then the target.
    enum class Copying : uint8_t { Off, Pads, Patterns, Tracks };
    Copying copying = Copying::Off;
    int8_t copy_from = -1;    // the kit pad, pattern or track picked first, -1 until then
    uint8_t copy_track = 0;   // Patterns: the track the pattern is copied from
    void stop_copying() { copying = Copying::Off; copy_from = -1; }
    // A word over the header for a moment: what an undo, redo, delete or copy did.
    char notice[32] {};
    uint32_t notice_at = 0;   // ms
};

} // namespace melodee
