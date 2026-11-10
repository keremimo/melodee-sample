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

enum class PageId : uint8_t { Stage, Keys, Mix, Sounds, Edit, Steps, Roll, Motion, Patterns, Song, InputConfig, Rack, Sends, Bus, Master, Pads, Samples, Trim, Chop, Levels, Record, Project, Tempo, Arp, Chord, Capture, PadPage, PlayPage, EnvelopePage, FilterPage, FollowPage, CloudPage, Punch, PadPunch, SettingsPage, About, PadFx, TimeCorrect, StepEdit,
                            PadMix, PadFilterPage, FilterEnvPage, // parity phase 4: the MPC's Mix, Filter and Filt Env for a kit pad
                            KnobFx, Compressor, FlexBeat,      // parity phase 7 and 7b
                            FaderPage,                         // parity phase 8: Shift + Pad 9
                            MidiConfig, Count };               // parity phase 9: Shift + Pad 8

// Takeover (shell/takeover.h): a pot or the fader drives its value once it has
// met it, so a page change never makes a value jump; until then Pickup waits
// and Scaled moves the value part of the way.
struct Pot {
    int16_t position = -1; // 0..127 where the control sits; -1 until known
    bool engaged = false;
    uint32_t bound = 0;    // which binding it was last checked against
    float scaled = -1;     // Scaled, not met yet: where the value is, unrounded (0..1); -1 none
};

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
    uint8_t step_event = 0;                   // Step Edit: which of the cursor step's events (shell/step_edit.h)
    // The fader (shell/fader.cpp): where it sits (-1 until it moves; Step Edit nudges by how far it went)
    // and whether it has met the value it drives (takeover), when it last moved (its card shows a moment).
    Pot fader;
    uint32_t fader_moved_at = 0;
    uint8_t fader_cursor = 0;                 // the Fader page: the target under the cursor
    uint8_t midi_cursor = 0;                  // MIDI Config: the row under the cursor (shell/midi_config_rows.h)
    bool midi_editing = false;                // and its value open (the wheel changes it)
    uint32_t midi_in_at = 0, midi_out_at = 0; // ms: MIDI last came in, went out (the page's lights)
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
    uint8_t knob_cursor = 0;                                   // Knob FX: the list's entry under the cursor (Push loads it)
    uint16_t files_cursor = 0;  // Samples, the browser: the row under the cursor
    uint16_t files_count = 0;   // rows in the folder listed, copied in by the shell
    uint8_t files_drive = 0;    // B2: 0 the card, 1 the internal drive (Drive in sample_library.h)
    bool preview = false;       // B3: each file plays as the cursor lands on it
    uint8_t trim_zoom = 0;      // Trim: 0 whole sample .. 4 sixteen times closer
    uint8_t trim_focus = 0;     // and where it closes in: 0 the start, 1 the end, 2 the loop point (Shift + K1-K3)
    bool sample_held = false;   // Sample held: a pad picks a kit pad without playing it (as on the MPC)
    bool sample_used = false;   // a pad was picked meanwhile, so letting go opens no page
    uint8_t warp_stretch = 0;   // Shift + Pad 15: the Follow how (Beats or Tones) Time stretch goes back to
    // Chop (parity phase 5): the last pad's region cut into slices.
    ChopMode chop_mode = ChopMode::Threshold;
    std::array<int, 4> chop_amount {{50, 8, 1, 0}}; // per mode: threshold, slice count, beats a slice (Manual: none)
    Slices slices;              // Chop's slices of the sample in hand
    uint16_t chop_sample = no_sample;
    uint32_t chop_from = 0, chop_to = 0; // the pad's region it chops (to 0: the end)
    uint8_t chop_slice = 0;
    uint8_t chop_zoom = 0;      // Shift + K1 / K2: 0 the whole region .. 4 sixteen times closer
    uint8_t chop_focus = 0;     // and where: 0 the slice's start, 1 its end
    bool chop_armed = false;    // Manual: pad 1 plays the region and the pads tapped meanwhile cut it
    uint32_t chop_take_at = 0;  // ms when pad 1 started that take, 0 none
    int8_t levels_hit = -1;     // Levels: the pad played last, 0..15
    int16_t levels_value = 0;   // and what it played: a velocity or semitones
    // Sample Record (parity phase 6; Input Config itself is in settings.input).
    int16_t rec_slot = -1, rec_pad = -1; // where the take in progress will land (a kit pad, 0..127)
    uint8_t rec_track = 0;               // on that track
    // Pads tapped during the take: each cuts it there and gets the part from there on.
    static constexpr unsigned rec_chop_capacity = 15;
    std::array<uint8_t, rec_chop_capacity> rec_chop_pad {};
    std::array<uint32_t, rec_chop_capacity> rec_chop_at {}; // frames into the take
    uint8_t rec_chops = 0;
    // Resample (Shift + Pad 11): waiting for the pad to render onto; then the render's pad (-1 none).
    bool resample_pick = false;
    int16_t render_pad = -1, render_slot = -1;
    uint8_t render_track = 0;
    uint8_t rendering = 0;               // 0 none, 1 Resample, 2 the song's mixdown
    bool render_resume = false;          // Resample stopped what was playing: Play it again afterwards
    bool song_export = false;            // Song, B1: Cancel, Audio mixdown, New pattern
    uint16_t project_cursor = 0;  // Project: row under the cursor (0 is New project)
    bool delete_armed = false;    // the file sheet's Delete asks once more
    uint8_t project_sort = 0;     // 0 newest first, 1 by name
    int8_t chord_pad = -1;        // Chord: the pad played last
    uint8_t sampler_row = 0;      // Envelope and Cloud: their second row
    uint8_t capture_bars = 2, capture_quantise = 1, capture_into = 0; // Capture
    uint16_t project_rows = 1;    // rows on the Project page, copied in by the shell
    // Pad FX (parity phase 7b): the punches engaged, oldest first (held, or latched with B1); the newest four sound,
    // the older ones wait bypassed until those let go. Which punch each live slot plays, and the one last pressed
    // (K1-K3 and B1 act on it).
    struct Engaged { int8_t kind = -1, pad = -1; bool latched = false; uint8_t depth = 100; };
    static constexpr unsigned engaged_capacity = 16;
    std::array<Engaged, engaged_capacity> engaged {};
    uint8_t engaged_count = 0;
    std::array<int8_t, 4> punch_slot {{-1, -1, -1, -1}};
    int8_t punch_selected = -1;
    uint8_t flex_chosen = 0; // Flex Beat: the pad chosen last (0 Empty)
    int engaged_at(int kind) const { for (unsigned n = 0; n < engaged_count; ++n) if (engaged[n].kind == kind) return int(n); return -1; }
    bool punch_sounding(int kind) const { for (auto k : punch_slot) if (k == kind && kind >= 0) return true; return false; }
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
    // A question over the page that B1 (Cancel) or B3 (Do it) answers, as the
    // MPC asks before erasing: Erase + pad on Step Edit (`ask_target` the
    // key), Erase + pot on Motion (the lane, bindings.h). shell/ask.cpp.
    enum class Ask : uint8_t { None, EraseKeyEvents, EraseLane, ResetSettings };
    Ask ask = Ask::None;
    int32_t ask_target = 0;
    // A word over the header for a moment: what an undo, redo, delete or copy did.
    char notice[32] {};
    uint32_t notice_at = 0;   // ms
};

} // namespace melodee
