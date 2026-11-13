// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/print.h"
#include "melodee/model/project.h"
#include "melodee/plugin/plugin_panel.h"
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

enum class PageId : uint8_t { Stage, Keys, Sounds, Edit, Steps, Roll, Motion, Patterns, Song, InputConfig, Rack, Sends, Bus, Master, Pads, Samples, Trim, Chop, Levels, Record, Project, Tempo, PadPage, PlayPage, EnvelopePage, FilterPage, FollowPage, CloudPage, Punch, PadPunch, SettingsPage, About, PadFx, TimeCorrect, StepEdit,
                            PadMix, PadFilterPage, FilterEnvPage, // parity phase 4: the MPC's Mix, Filter and Filt Env for a kit pad
                            KnobFx, Compressor, FlexBeat,      // parity phase 7 and 7b
                            FaderPage,                         // parity phase 8: Shift + Pad 9
                            MidiConfig,                        // parity phase 9: Shift + Pad 8
                            CardAccess,                        // parity phase 10b: Project's SD card access
                            Tracks,                            // Sample's second stop: the sixteen tracks and the strip in hand
                            Looper,                            // LOOPER on Tracks: the four loops (docs/LOOPER.md)
                            RecQuantize,                       // Shift + Pad 10: Rec Quantize on or off, and its grid
                            Plugins,                           // the plug-in picker: the card's Plugins folder (docs/MELODEE.md)
                            PluginPresets,                     // PRESETS: a plug-in's programs (docs/MELODEE.md)
                            LayersPage,                        // roadmap P3: a kit pad's four layers
                            ModPage,                           // roadmap P4: modulators and macros (MOTION again on Motion)
                            Scenes,                            // roadmap P5: scenes and follow actions (Push on Patterns)
                            Print,                             // freeze plan F2c: the PRINT sheet (Sample Record on a plug-in or MIDI track)
                            Count };

// Takeover (shell/takeover.h): a pot or the fader drives its value once it has
// met it, so a page change never makes a value jump; until then Pickup waits
// and Scaled moves the value part of the way.
struct Pot {
    int16_t position = -1; // 0..127 where the control sits; -1 until known
    bool engaged = false;
    uint32_t bound = 0;    // which binding it was last checked against
    float scaled = -1;     // Scaled, not met yet: where the value is, unrounded (0..1); -1 none
};

// SD card access (parity phase 10b, shell/card_access.h): what the page shows.
// `state` is the base's CardShareState (0 home, 1 lending, 2 lent, 3 returning).
enum class CardNote : uint8_t { None, NoCard, InUse, Writing, Locked, Empty, Failed };
struct CardView {
    uint8_t state = 0;
    bool connected = false, ejected = false, locked = false;
    CardNote note = CardNote::None;
    uint64_t returns = 0;
};

// Trim's and Chop's view zoomed in, kept while its handle moves inside it (shell/zoom_view.h); key 0: none kept.
struct ZoomView {
    uint32_t from = 0, to = 0, key = 0;
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
    bool erase_used = false;     // Erase held did something (a pad, a pot, the fader): its release deletes nothing
    // Mute mode (shell/mute_mode.cpp): a tap latches it, a hold lasts while held.
    bool mute_latch = false, mute_used = false;
    uint32_t mute_down = 0;
    bool mute_mode() const { return mute || mute_latch; }
    // Note Repeat (shell/note_repeat.cpp): a tap latches, a hold lasts while held.
    bool repeat_held = false, repeat_turned_on = false;
    uint8_t repeat_track = 0;    // the track it went down on
    uint32_t repeat_down = 0;
    bool exit_open = false, recording = false;
    // A project loading (Context::send_reload): the audio thread parked while it is built, then its samples read.
    // The Loading sheet covers the screen and owns the controls (main volume still turns) until both are done.
    bool loading = false;
    uint32_t loading_since = 0;                 // ms
    uint16_t loading_total = 0, loading_left = 0; // the project's sample slots in use, and those not in yet
    bool exit_off = false;         // the Exit sheet's POWER OFF chosen: saving, then the MPC turns off
    bool exit_saved = false;       // and the project is on the card: the sheet says so for a second first
    // The wheel's highlight on screens that offer their keys as choices (shell/key_pick.h): the power sheet's
    // key (B1 STAY when it opens), a question's (B1 CANCEL when asked), Project's tiles (B1 SAVE).
    uint8_t exit_pick = 0, ask_pick = 0, project_pick = 0;
    bool rec_armed = false;        // Seq Record pressed while stopped: Play starts the take (its light flashes)
    uint8_t lead_pattern = 0;      // the pattern launched last: whose tempo plays with BPM per pattern on
    uint32_t locate = 0;           // Patterns: where Shift + Play carries on from, ticks (the wheel moves it)
    bool bpm_tenths = false;       // Seq held + Push: the wheel's tempo by whole BPM or tenths
    // Time Correct (Shift + Pad 14): shift in ticks and swing for the notes moved, and the keys chosen.
    int16_t tc_shift = 0;
    uint8_t tc_swing = 50;
    std::array<uint64_t, 3> tc_keys {}; // a bit a key, 0..191 (kit keys run to 163)
    bool tc_all = true;                 // B2: every note of the pattern, whatever the pads chose
    bool tc_key(unsigned key) const { return key < 192 && ((tc_keys[key / 64] >> (key % 64)) & 1); }
    // Time Correct C (round 2): QUANTIZE off keeps the feel taken off to compare by ear (strength, humanize, its
    // velocity, the groove's amount) and which pattern it was (track * 16 + pattern; -1 none), to put back.
    std::array<uint8_t, 4> tc_stash {};
    int16_t tc_stash_at = -1;
    void tc_toggle(unsigned key) { if (key < 192) tc_keys[key / 64] ^= uint64_t(1) << (key % 64); }
    std::array<uint8_t, track_count> edit_page {}, edit_row {};
    // Stage (pages/stage_page.cpp, shell/stage_map.h): each track's button (B1-B3) and the page of its cycle.
    std::array<uint8_t, track_count> stage_group {}, stage_sub {};
    uint32_t preset_due = 0;  // ms: PRESET's sound stopped turning; the sound under the cursor loads then (0 none)
    uint16_t preset_cursor = 0; // and where it is in the list (sound_list())
    // An empty track's choosers: the engine and its sound (SYNTH), the source (KIT), port and channel (MIDI).
    uint8_t new_engine = 0, new_sound = 0, new_source = 0, new_port = 1, new_channel = 1;
    // The New track sheet (shell/new_track.h): open, its card lit, the page it was opened over, and the track
    // whose first sample from Samples goes back to that page (-1 none).
    bool new_open = false;
    uint8_t new_pick = 0;
    PageId new_back = PageId::Stage;
    int8_t new_return = -1;
    // Pad Bank (shell/pad_bank.cpp): when it went down; held a second, the tracks over the page.
    uint32_t pad_bank_down = 0;
    uint16_t sounds_cursor = 0;
    uint8_t sounds_filter = 0;       // all, favourites, recent
    uint8_t audition_velocity = 100;
    int16_t auditioning = -1;        // the sound heard on the pads while browsing, -1 for the loaded one
    std::array<uint16_t, 8> recent {}; // recently loaded sounds of the track's engine, newest first
    uint8_t recent_count = 0;
    uint8_t steps_bar = 0;
    int16_t step_cursor = 0, held_step = -1;
    uint8_t step_event = 0;                   // Step Edit: which of the cursor step's events (shell/step_edit.h)
    // Step Edit (round 2, step-edit2 C, step entry): what a pad writes at the caret (step_cursor): its length in
    // steps of Q, its velocity (0: as played) and chance; the pads held for this step, and whether they wrote.
    bool chain_building = false;              // Seq held: a pad tapped after the first adds to the chain (seq-sheet2 B)
    uint8_t entry_length = 1, entry_velocity = 0, entry_chance = 100;
    uint16_t entry_held = 0;
    bool entry_wrote = false;
    bool trig_cards = false;                  // Steps and Roll: their lit key again turns the cards to the note's TRIG row (roadmap P1)
    bool fill = false;                        // Play held while playing: Fill (FILL trig conditions play)
    int8_t held_note = -1;                    // Roll: the pad held on the note in focus (its cards lock it, roadmap P1)
    bool euclid_cards = false;                // Steps on a kit, STEPS a third time: the EUCLID row of the last pad's lane (roadmap P1)
    uint8_t vary_amount = 30;                 // Steps' VARY sheet (Push): how far thinner, busier, humanised go, percent
    // The fader (shell/fader.cpp): where it sits (-1 until it moves)
    // and whether it has met the value it drives (takeover), when it last moved (its card shows a moment).
    Pot fader;
    uint32_t fader_moved_at = 0;
    uint8_t midi_cursor = 0;                  // MIDI Config: the row under the cursor (shell/midi_config_rows.h)
    bool midi_editing = false;                // and its value open (the wheel changes it)
    uint32_t midi_in_at = 0, midi_out_at = 0; // ms: MIDI last came in, went out (the page's lights)
    std::array<uint32_t, track_count> track_out_at {}; // ms: a MIDI track last sent on its own port (Edit's DEVICE picture)
    // midi-track A (round 2): the last notes MIDI tracks sent, for their DEVICE picture: the track, key and velocity,
    // when it went out and when it was let go (0: still held), ms. A ring, newest at sent_next - 1.
    struct SentNote { uint8_t track = 255, key = 0, velocity = 0; uint32_t at = 0, until = 0; };
    std::array<SentNote, 32> sent_notes {};
    uint8_t sent_next = 0;
    // The MIDI outputs open now, for the DEVICE picture: bit 0 the jacks, bit 1 USB-C's gadget, bits 8-15 USB host devices.
    uint32_t midi_outputs = 0;
    // Roadmap P4 (docs/MIDI_TRACKS.md M4-M5): the USB MIDI devices connected (by name, as their ports say), the .syx
    // files in the card's SysEx folder, each MIDI track's SysEx sending (percent, 0 none) and whether its input is
    // taken by the ADC's route elsewhere (IN USE), a bit a track.
    std::array<std::array<char, 32>, 6> usb_devices {};
    uint8_t usb_device_count = 0;
    std::array<std::array<char, 32>, 16> sysex_names {};
    uint8_t sysex_count = 0;
    bool sysex_listed = false;
    std::array<uint8_t, track_count> sysex_percent {};
    uint16_t midi_in_use = 0;
    // Roadmap P4, Song's Export: what B2 makes, the wheel's pick: a mixdown as 32-bit float, 24-bit or 16-bit (both
    // dithered), the stems, or a MIDI file.
    uint8_t export_kind = 0;
    uint32_t held_since = 0;                  // when the held step's pad went down, ms
    bool held_created = false, held_edited = false;
    bool sheet_open = false;  // a sheet behind Push: Roll's note, a chain's device, a project's file
    uint8_t sheet_row = 0;
    // Roll (shell/roll.h): the cursor is step_cursor (a step of Q) and roll_key (a pitch), and the note
    // starting there on that key is in focus. roll_view: the first step in view; roll_top: the top row's key;
    // roll_new: the length a pad's new note gets, in ticks (0: one step of Q).
    uint8_t roll_key = 62, roll_top = 65;
    int16_t roll_view = 0;
    uint16_t roll_new = 0;
    bool map_open = false;    // the page map over Edit
    uint8_t map_cursor = 0;
    int8_t pattern_held = -1; // Patterns: a pattern held (its number), so the next pad copies it there
    // The bank of patterns the pads and the grids show, A-H (Seq held + Pad Bank steps it). It follows the selected
    // track's pattern (the one playing, else the one that will) whenever that changes: bank_seen is the last seen.
    uint8_t pattern_bank = 0;
    int16_t bank_seen = -1;
    unsigned pad_pattern(unsigned pad) const { return pattern_bank % pattern_banks * pattern_pads + pad % pattern_pads; }
    bool pattern_copied = false;
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
    uint8_t comp_bank = 0;                                     // Compressor: the kit bank those pads show
    bool comp_pads = false;                                    // Compressor, Sidechain on a kit: the pads pick the key pad, not the track
    uint8_t knob_cursor = 0;                                   // Knob FX: the list's entry under the cursor (Push loads it)
    // Plug-ins (docs/MELODEE.md): the picker's cursor and rows (copied in by the shell), what Push loads into (a
    // PLUGIN track's instrument, or a chain's slot), the plug-in in focus as the page shows it, and each entry's row.
    enum class PluginTarget : uint8_t { Instrument, Effect };
    uint16_t plugins_cursor = 0, plugins_count = 0;
    PluginTarget plugin_target = PluginTarget::Instrument;
    uint8_t plugin_chain = 0, plugin_slot = 0;  // Effect: where it goes
    PageId plugin_back = PageId::Stage;          // where B1 (Back) at the top and a load go
    PluginPanel plugin_panel;
    std::array<uint16_t, max_plugins> plugin_row {};
    DrumVoices drum_voices; // the selected track's Drums pads: its drum plug-in's voices (Keys, the pad lights)
    // PRESETS: the entry whose programs it lists, the row under the cursor, and where B1 goes back to.
    uint8_t presets_entry = no_plugin;
    uint16_t presets_cursor = 0;
    PageId presets_back = PageId::Edit;
    uint16_t files_cursor = 0;  // Samples, the browser: the row under the cursor
    uint16_t files_count = 0;   // rows in the folder listed, copied in by the shell
    uint8_t files_drive = 0;    // B2: 0 the card, 1 the internal drive (Drive in sample_library.h)
    bool preview = false;       // B3: each file plays as the cursor lands on it
    uint8_t trim_zoom = 0;      // Trim: 0 the whole sample .. 100 a few hundred frames (shell/zoom_view.h)
    ZoomView trim_view;         // and the view it keeps
    uint8_t pad_layer = 0;      // Layers (roadmap P3): the layer in hand, 0-3
    uint16_t zone = 0;          // Stage's ZONES (round 3 "instrument C"): the instrument's zone in hand; the key played picks it
    int8_t layer_load = -1;     // Samples opened by Layers' LOAD: the layer Push loads onto (1-3); -1 the pad
    uint8_t trim_focus = 0;     // and where it closes in: 0 the start, 1 the end, 2 the loop point (Shift + K1-K3)
    bool sample_held = false;   // Sample held: a pad picks a kit pad without playing it (as on the MPC)
    bool sample_used = false;   // a pad was picked meanwhile, so letting go opens no page
    // Seq held (shell/seq_layer.h): the Sequence layer on any page. When it went down, whether a control
    // did something meanwhile (its release then goes nowhere), and whether B1-B3 left for an editor (the
    // sheet stays away until Seq is let go). Each track's editor last open (0 none yet, then 1 + EditorKind).
    bool seq_held = false, seq_used = false, seq_left = false;
    uint32_t seq_down = 0;
    std::array<uint8_t, track_count> seq_editor {};
    uint8_t warp_stretch = 0;   // Shift + Pad 15: the Follow how (Beats or Tones) Time stretch goes back to
    // Chop (parity phase 5): the last pad's region cut into slices.
    ChopMode chop_mode = ChopMode::Threshold;
    std::array<int, 4> chop_amount {{50, 8, 1, 0}}; // per mode: threshold, slice count, beats a slice (Manual: none)
    Slices slices;              // Chop's slices of the sample in hand
    uint16_t chop_sample = no_sample;
    uint32_t chop_from = 0, chop_to = 0; // the pad's region it chops (to 0: the end)
    uint8_t chop_slice = 0;
    uint8_t chop_zoom = 0;      // Shift + K1 / K2: 0 the whole region .. 100 a few hundred frames (shell/zoom_view.h)
    uint8_t chop_focus = 0;     // and where: 0 the slice's start, 1 its end
    bool chop_fine = false;     // K1 / K2 touched: the wheel fine-edits the handle in focus until a pad picks another slice
    ZoomView chop_view;         // the view zoomed in, kept
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
    bool resample_song = false;          // roadmap P3: the pick is the song's (Resample on Song), not the pattern's
    uint32_t render_stop_at = 0;         // a song past 60 s onto a pad: ms when its first 60 s are in and it stops (0 none)
    int16_t render_pad = -1, render_slot = -1;
    uint8_t render_track = 0;
    uint8_t rendering = 0;               // 0 none, 1 Resample, 2 the song's mixdown, 3 the song onto a pad (roadmap P3)
    bool render_resume = false;          // Resample stopped what was playing: Play it again afterwards
    bool song_export = false;            // Song, B1: Cancel, Audio mixdown, New pattern
    bool project_list = false;    // Project: the list behind OPEN (else SAVE, OPEN and NEW as tiles)
    uint8_t sends_bus = 0;        // the bus in view on Sends and Bus (round 2: a track sends into all four; K2 on Sends picks)
    // Roadmap P4, the MOD page: modulators (0) or macros (1), the one in hand and its target in hand; the lane a pot
    // turned last (-1 none) and its track, which ADD takes (shell/mod_jobs.h).
    uint8_t mod_view = 0, mod_index = 0, mod_target = 0, macro_index = 0, macro_target = 0;
    uint8_t scene = 0; // roadmap P5: the scene in hand (SCENES; the one launched last)
    int16_t learn_lane = -1;
    uint8_t learn_track = 0;
    uint16_t project_cursor = 0;  // Project: row under the cursor (0 is SD card access at the top)
    bool delete_armed = false;    // the file sheet's Delete asks once more
    uint8_t project_sort = 0;     // 0 newest first, 1 by name
    int8_t chord_pad = -1;        // Chord: the pad played last
    uint8_t sampler_row = 0;      // Envelope and Cloud: their second row
    // Capture (Shift + Seq Record; round 2, capture C, no page): the pattern the last bars went into and when,
    // for the header a few seconds.
    uint32_t captured_at = 0;
    uint8_t captured_pattern = 0;
    uint8_t captured_tracks = 0; // roadmap P5: how many tracks it filled (the header's +n beside the selected one's)
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
    // pad-fx C (round 2): the take, the punches heard in one pass of the selected track's pattern, oldest first, for
    // the bar Pad FX draws and its B3 KEEP; a punch in a later pass begins a new take (the sequencer's tick where
    // this one's pass ends).
    static constexpr unsigned span_capacity = 24;
    std::array<PunchSpan, span_capacity> spans {};
    uint8_t span_count = 0;
    uint64_t take_until = 0;
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
    uint8_t settings_cursor = 1;              // the row (Settings::Row) in hand behind a door
    bool settings_editing = false;            // Push opened the value under the cursor
    uint8_t settings_door = 0;                // Settings: the door in hand (pages/settings_page.cpp)
    bool settings_in = false;                 // and behind it: its rows
    uint8_t about_view = 0;                   // About (round 2): 0 how the machine is doing, 1 the credits (B3 again)
    std::array<Pot, 3> pots {};
    int8_t turning = -1;      // the card turned last
    uint32_t turned_at = 0;   // when, in ms
    int8_t last_pot = -1;     // for Shift + wheel fine edits
    bool last_pot_shift = false; // and whether it turned its Shift card
    uint32_t held_pads = 0;
    // Shift + Erase: copy one kit pad onto another, one pattern onto another
    // (Seq held, or Patterns), one track onto another (Pad Bank held, or
    // Tracks), one key's notes onto another key (a synth's play pages, Roll,
    // Step Edit), or one step onto another (Steps). Armed, then the source,
    // then the target (shell/erase_copy.h).
    enum class Copying : uint8_t { Off, Pads, Patterns, Tracks, Keys, Steps };
    Copying copying = Copying::Off;
    int16_t copy_from = -1;   // the kit pad, pattern, track, key or step picked first, -1 until then
    uint8_t copy_track = 0;   // Patterns: the track the pattern is copied from
    bool moving = false;      // Tracks: a move (B2 MOVE: the two trade places) instead of a copy (shell/track_swap.h)
    void stop_copying() { copying = Copying::Off; copy_from = -1; moving = false; }
    // A question over the page that B1 (Cancel) or B3 (Do it) answers, as the
    // MPC asks before erasing: Erase + pad on Step Edit (`ask_target` the
    // key), Erase + pot on Motion (the lane, bindings.h). shell/ask.cpp.
    enum class Ask : uint8_t { None, EraseKeyEvents, EraseLane, ResetSettings, NewProject, TakeCardBack, ClearLoop, LooperOff, ClearPattern, ErasePunches, ClearTrack, InstallUpdate, Unfreeze }; // Unfreeze (freeze plan F2c): an edit of a frozen track; target the track // InstallUpdate: newer release on the card (shell/update.h); ClearLoop: target the loop; ClearPattern (Seq held + Erase + pad): target the pattern; ClearTrack (Pad Bank held + Erase + pad): target the track // NewProject (phase 10): target 0 nothing unsaved, 1 saved first, 2 lost; TakeCardBack (10b): the computer still holds the card
    Ask ask = Ask::None;
    int32_t ask_target = 0;
    // The Looper (docs/LOOPER.md, shell/looper_jobs.cpp): the loop selected, the page's view (0 LOOPS, 1 MIX),
    // its sheet (B3 MORE, shell/loop_sheet.h); Loop down (held, the pads are the loop grid on any page,
    // loop_grip once held long or a grid pad hit: its release then does nothing) and whether it went down on Song;
    // the loop whose clear pad is held on the grid, since when; and the
    // loop whose chain FX Select shows (-1: the selected track's).
    uint8_t loop = 0, looper_view = 0;
    bool loop_sheet = false;
    uint32_t loop_down = 0;
    bool loop_held = false, loop_grip = false, loop_on_song = false;
    bool loop_shift_held = false; // Shift + Loop down: let go soon, Reverse; held, the Looper on or off
    // 16 Levels down: its job waits for the release; used once Loop came with it (the Looper, shell/looper_chord.h)
    // or Shift + 16 Levels took the type at once, and the release then does nothing.
    bool levels_held = false, levels_used = false;
    int8_t loop_clear = -1;
    uint32_t loop_clear_at = 0;
    int8_t fx_loop = -1;
    // SD card access (shell/card_access.h): the card as the service thread
    // last saw it, copied in on the MIDI thread.
    CardView card;
    // Update from the card (shell/update.h): offered or installing version, UpdatePhase, UpdateError, and the
    // project store's save count to wait for after INSTALL.
    struct UpdateView { uint8_t phase = 0, error = 0; char version[32] {}; int32_t after_save = -1; };
    UpdateView update;
    // Freeze plan F2c (shell/print_sheet.cpp): the PRINT sheet's choices for its track: the patterns lit (a bit each),
    // where the prints are taken and how they end; `printing` once B3 FREEZE went (the sheet shows the prints made
    // until they are ready, B1 calls them off), for the freeze `print_serial` names.
    struct PrintSheet {
        uint8_t track = 0;
        std::array<uint64_t, (pattern_count + 63) / 64> picks {};
        PrintTap tap = PrintTap::PostFx;
        PrintTail tail = PrintTail::Loop;
        bool printing = false;
        uint32_t serial = 0;
        bool picked(unsigned p) const { p %= pattern_count; return (picks[p / 64] >> (p % 64)) & 1; }
        void pick(unsigned p, bool on) {
            p %= pattern_count;
            uint64_t bit = uint64_t(1) << (p % 64);
            picks[p / 64] = on ? picks[p / 64] | bit : picks[p / 64] & ~bit;
        }
    };
    PrintSheet print_sheet;
    // Seq held + Sample Record: each pattern pad freezes or thaws its pattern of the selected track until Seq is let
    // go (`seq_frozen`: the patterns as they will be then); the prints are made on the release.
    bool seq_print = false;
    std::array<uint64_t, (pattern_count + 63) / 64> seq_frozen {};
    bool seq_frozen_at(unsigned p) const { p %= pattern_count; return (seq_frozen[p / 64] >> (p % 64)) & 1; }
    // A word over the header for a moment: what an undo, redo, delete or copy did.
    char notice[32] {};
    uint32_t notice_at = 0;   // ms
};

} // namespace melodee
