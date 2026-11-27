// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/print.h"
#include "melodee/model/project.h"
#include "melodee/plugin/plugin_panel.h"
#include "melodee/shell/auto_sample.h"
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

// Sends and Tracks are gone with the mixer (docs/MIXER_PLAN.md step 6): opening either opens it; kept so the numbers stay.
enum class PageId : uint8_t { Stage, Keys, Sounds, Edit, Roll, Motion, Patterns, Song, InputConfig, Rack, Sends, Bus, Master, Pads, Samples, Trim, Chop, Record, Project, Tempo, PadPage, PlayPage, EnvelopePage, FilterPage, FollowPage, CloudPage, Punch, SettingsPage, About, PadFx, TimeCorrect, StepEdit,
                            PadMix, PadFilterPage, FilterEnvPage, // parity phase 4: the MPC's Mix, Filter and Filt Env for a kit pad
                            KnobFx, Compressor, FlexBeat,      // parity phase 7 and 7b
                            FaderPage,                         // parity phase 8: Shift + Pad 9
                            MidiConfig,                        // parity phase 9: Shift + Pad 8
                            CardAccess,                        // parity phase 10b: Project's SD card access
                            Tracks,                            // gone with the mixer (docs/MIXER_PLAN.md step 6): opens it; kept so the numbers stay
                            Looper,                            // the four loops (Loop + 16 Levels; docs/LOOPER.md)
                            Plugins,                           // the plug-in picker: the card's Plugins folder (docs/MELODEE.md)
                            PluginPresets,                     // PRESETS: a plug-in's programs (docs/MELODEE.md)
                            LayersPage,                        // roadmap P3: a kit pad's four layers
                            ModPage,                           // roadmap P4: modulators and macros (MOTION again on Motion)
                            Print,                             // freeze plan F2c: the PRINT sheet (Sample Record on a plug-in or MIDI track)
                            Scenes,                            // roadmap P5: the bank's scenes (B1 again on Patterns)
                            AutoSample,                        // the AUTO SAMPLE sheet: Resample (Shift + Pad 11) on a plug-in track
                            Stems,                             // the STEMS sheet: Shift + B3 on a kit pad's TRIM (shell/stems_jobs.cpp)
                            Mixer,                             // the mixer (docs/MIXER_PLAN.md): Pad FX + Knob FX
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
// The value under the pot changed without it: it picks the value up again.
inline void release(Pot& pot) { pot.engaged = false; pot.bound = 0; }

// SD card access (parity phase 10b, shell/card_access.h): what the page shows.
// `state` is the base's CardShareState (0 home, 1 lending, 2 lent, 3 returning).
// Camera: a computer shows Screen Mirror's camera, which the drive would unplug (base drivers/screen_mirror.h).
enum class CardNote : uint8_t { None, NoCard, InUse, Writing, Locked, Empty, Failed, Camera, Network };
struct CardView {
    uint8_t state = 0;
    bool connected = false, ejected = false, locked = false;
    CardNote note = CardNote::None;
    uint64_t returns = 0;
};

// Song's Export, what B2 makes (UiState::export_kind), in the wheel's order.
namespace export_kinds {
constexpr uint8_t mix32 = 0, mix24 = 1, mix16 = 2, stems = 3, midi = 4, mpc3 = 5, live = 6, count = 7;
}

// Trim's and Chop's view zoomed in, kept while its handle moves inside it (shell/zoom_view.h); key 0: none kept.
struct ZoomView {
    uint32_t from = 0, to = 0, key = 0;
};

// Samples: the audio file under the cursor trimmed before it goes onto a pad (shell/browse_trim.h). K2 or K3 read it
// into a sample slot, as Push would, and raise the sheet; Push or Shift + pad load it with START and END on the pad.
struct BrowseTrim {
    int16_t slot = -1;           // the slot the file is read into; -1: the sheet is down
    bool fresh = false;          // the slot was taken for the sheet: it is freed again unless the file goes onto a pad
    uint32_t start = 0, end = 0; // frames (44.1 kHz); end 0: the file's end
    uint8_t focus = 0;           // 0 START, 1 END: the handle the zoom is on and the wheel moves
    uint8_t zoom = 0;            // Shift + K2 / K3: 0..100 as on Trim (shell/zoom_view.h)
    bool own_zoom = false;       // a zoom set by Shift + K2 / K3; else about 120 ms round the handle
    ZoomView view;               // the view kept while an own zoom is set
    uint32_t opened_at = 0;      // ms: the sheet's rise
    uint32_t heard_at = 0;       // ms the kept part plays once, a moved end gone still (0: none due)
    bool up() const { return slot >= 0; }
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
    uint32_t pad_bank_down = 0;  // ms: when Pad Bank went down (held past pad_bank_hold_ms, its release steps no bank)
    bool erase_used = false;     // Erase held did something (a pad, a pot, the fader): its release deletes nothing
    uint8_t erase_cleared = 0;   // Erase held: the pots that took a lock out (a bit each); the rest of their turn says nothing
    // Mute mode (shell/mute_mode.cpp): a tap latches it, a hold lasts while held.
    bool mute_latch = false, mute_used = false;
    uint32_t mute_down = 0;
    bool mute_mode() const { return mute || mute_latch; }
    bool light_show = false; // Sample + Mute: every light follows the tempo and the pads (shell/light_show.h)
    uint8_t light_style = 0; // its style (ShowStyle), Sample + Minus / Plus while it is on
    // Note Repeat (shell/note_repeat.cpp): a tap latches, a hold lasts while held.
    bool repeat_held = false, repeat_turned_on = false;
    uint8_t repeat_track = 0;    // the track it went down on
    uint32_t repeat_down = 0;
    bool exit_open = false, recording = false;
    // A project loading (Context::send_reload): the audio thread parked while it is built, then its samples read.
    // The Loading sheet covers the screen and owns the controls (main volume still turns) until both are done.
    bool loading = false;
    uint32_t loading_since = 0;                 // ms
    uint16_t loading_total = 0, loading_left = 0; // what it has to read (a sample, an instrument's file) and what is not in yet
    // The same apart, as the sheet's rows count them: its samples, and its instruments' files (how many each
    // instrument has is known once its own file is read; one not read yet counts as one).
    uint16_t loading_samples = 0, loading_samples_in = 0;
    uint16_t loading_files = 0, loading_files_in = 0;
    uint16_t loading_instruments = 0;
    int16_t loading_slot = -1;                  // the sample slot being read now (its name is shown); -1 none
    uint16_t loading_bar = 0;                   // the bar, 0..1000: it never goes back
    bool exit_off = false;         // the Exit sheet's POWER OFF chosen: saving, then the MPC turns off
    bool exit_saved = false;       // and the project is on the card: the sheet says so for a second first
    // The wheel's highlight on screens that offer their keys as choices (shell/key_pick.h): the power sheet's
    // key (B1 STAY when it opens), a question's (B1 CANCEL when asked), Project's tiles (B1 SAVE).
    uint8_t exit_pick = 0, ask_pick = 0, project_pick = 0;
    // The MENU (shell/menu.h), the encoder held: 0 closed, 1 HERE (the page's jobs as cards), 2 GO (the sixteen
    // places as the pads sit); the tile the wheel is on (a pad's index) and the card.
    uint8_t menu = 0, menu_tile = 0, menu_row = 0;
    // The undo list (shell/history_list.h), Shift + Minus held: open, and the row the cursor is on.
    bool history = false;
    uint16_t history_row = 0;
    // A lesson (shell/lesson.h): which (0 none, else its number + 1), the step in hand, the button held with the
    // step's control (-1 none), where the step's knob stood when it first moved (-1 not yet), when the step began.
    uint8_t lesson = 0, lesson_step = 0;
    int8_t lesson_with = -1;
    int16_t lesson_pot = -1;
    uint32_t lesson_at = 0;
    // A soft key that steps through several pages lists them by name for a moment after a press (pages/overlays.cpp):
    // the key (0-2) and when it was pressed.
    uint8_t key_list_key = 0;
    uint32_t key_list_at = 0;
    bool rec_armed = false;        // Seq Record pressed while stopped: Play starts the take (its light flashes)
    // Free length (shell/free_length.h), for K1's card: the longest pattern with notes on another track in the
    // selected pattern's sequence (0: none, Free is offered), and a take growing the selected pattern now.
    uint32_t column_ticks = 0;
    bool free_growing = false;
    uint8_t lead_pattern = 0;      // the pattern launched last: whose tempo plays with BPM per pattern on
    uint32_t locate = 0;           // Patterns: where Shift + Play carries on from, ticks of the track in view's pattern as written (the wheel moves it)
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
    // An empty track picked (shell/new_track.h): that track, its Stage open (-1 none), and the page to go back to
    // once it is made.
    int8_t new_for = -1;
    uint8_t new_pick = 0; // the carousel's card in the middle: a NewType, or with new_sources one of its sources
    uint8_t new_type = 0; // the type whose sources are open (NewType)
    bool new_sources = false; // the carousel shows that type's sources (B1 BACK: the types)
    float new_pick_from = 0; // and where it was drawn when it last moved (new_pick_shown())
    uint32_t new_pick_at = 0; // ms, when (0: still)
    PageId new_back = PageId::Stage;
    uint32_t new_taken = 0; // buttons whose press the carousel's lock took (a bit each): their release goes too
    int8_t new_kit = -1; // the kit made for Samples' first sample (CANCEL there makes it empty again), -1 none
    uint16_t sounds_cursor = 0;
    uint8_t sounds_filter = 0;       // all, favourites, recent
    uint8_t audition_velocity = 100;
    int16_t auditioning = -1;        // the sound heard on the pads while browsing, -1 for the loaded one
    std::array<uint16_t, 8> recent {}; // recently loaded sounds of the track's engine, newest first
    uint8_t recent_count = 0;
    uint8_t steps_bar = 0;
    int16_t step_cursor = 0, held_step = -1;
    int16_t spare_step = -1; // Step Edit: the caret past the last step, where a step makes the pattern longer (shell/spare_bar.h)
    // Settings > SEQ > Follow (shell/step_edit.cpp seq_follow): the bar (Page) or step (Step) the caret last went to; -1 none.
    int16_t follow_step = -1;
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
    // What the loops and MIDI tracks need of the inputs (InputRoute::need_line, need_mic, need_usb): a source on the ADC
    // side they did not get is IN USE (InputRoute::adc_taken).
    uint8_t adc_needs = 0;
    // Roadmap P4, Song's Export: what B2 makes, the wheel's pick (export_kinds): a mixdown as 32-bit float, 24-bit or
    // 16-bit (both dithered), the stems, a MIDI file, an MPC 3 project or an Ableton Live Set.
    uint8_t export_kind = 0;
    uint32_t held_since = 0;                  // when the held step's pad went down, ms
    bool held_created = false, held_edited = false;
    bool sheet_open = false;  // a sheet behind Push: Roll's note, a chain's device, a project's file
    uint8_t sheet_row = 0;
    // Samples' LOAD KIT sheet (Push on a KIT over a kit track, shell/project_jobs.cpp): the kit read, how many pads
    // each of its banks fills, and which of its banks goes into each bank of the track (-1 none).
    struct KitSheet {
        char name[32] {};
        std::array<uint8_t, sampler_banks> pads {};
        std::array<int8_t, sampler_banks> into {-1, -1, -1, -1, -1, -1, -1, -1};
    };
    KitSheet kit_sheet;
    // Roll (shell/roll.h): the cursor is step_cursor (a step of Q) and roll_key (a pitch), and the note
    // starting there on that key is in focus. roll_view: the first step in view; roll_top: the top row's key;
    // roll_new: the length a pad's new note gets, in ticks (0: one step of Q).
    uint8_t roll_key = 62, roll_top = 65;
    int16_t roll_view = 0;
    uint16_t roll_new = 0;
    // Roll's selection (shell/roll.h): Shift + wheel marks notes one by one (roll_marking while Shift is held); let
    // go, the marked notes take the wheel, Shift + wheel, Erase, Mute, Note Repeat and K1-K3; a Shift tap clears
    // them. Each mark is a note's tick, key and pitch on one track's pattern (roll_mark_track, roll_mark_pattern).
    struct RollMark { uint32_t tick = 0; uint8_t key = 0; int8_t pitch = 0; };
    static constexpr unsigned roll_marks_max = 128;
    std::array<RollMark, roll_marks_max> roll_marks {};
    uint8_t roll_marked = 0, roll_mark_track = 0;
    uint16_t roll_mark_pattern = 0;
    bool roll_marking = false;
    // Loop tapped on Roll, Steps or Step Edit (shell/note_jobs.h): the track's pattern plays only these ticks while
    // the editor stays open on it.
    bool edit_loop = false;
    uint8_t edit_loop_track = 0, edit_loop_pattern = 0;
    uint32_t edit_loop_from = 0, edit_loop_ticks = 0;
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
    // Jam capture (model/jam_log.h): the bars of the run being logged and whether one is (Song's foot), the jams
    // kept; JAMS (Song's Shift + B1): open, the jam picked (its index, oldest first), its bars, the part FROM-TO
    // (bars, TO not included), and whether B2 plays it.
    uint16_t jam_bars = 0;
    bool jam_logging = false;
    uint8_t jam_count = 0;
    bool jam_sheet = false;
    uint8_t jam_pick = 0;
    uint16_t jam_pick_bars = 0, jam_from = 0, jam_to = 0;
    bool jam_audition = false;
    // Freeze plan F3: an audio track's take armed or recording (its track, -1 none), and whether it runs now.
    int8_t audio_rec_track = -1;
    bool audio_rec_running = false;
    // Per chain: the slot (or + ADD) and parameter row; the pad chains share the last entry (cursor_of()).
    std::array<uint8_t, mix_chain_count + 1> fx_cursor {}, fx_row {};
    static unsigned cursor_of(unsigned chain) { return chain < mix_chain_count ? chain : mix_chain_count; } // the pads' chains share the last
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
    bool plugin_replace = false; // REPLACE was pressed on "Replace <plug-in>?": choose_plugin goes ahead
    bool scene_capture = false;  // Scenes' Shift + B2 CAPTURE: the next pad picks the scene the playing patterns go to
    PluginTarget plugin_target = PluginTarget::Instrument;
    uint8_t plugin_chain = 0, plugin_slot = 0;  // Effect: where it goes
    PageId plugin_back = PageId::Stage;          // where a load goes, and an effect's B1 (CANCEL) at the top
    PageId plugin_from = PageId::Stage;          // the page it opened over: an instrument's B1 (CANCEL) at the top
    PluginPanel plugin_panel;
    std::array<uint16_t, max_plugins> plugin_row {};
    // A plug-in's own screen (shell/screen_view.h): an effect slot's full page open on a chain page (Push on it).
    bool fx_panel = false;
    DrumVoices drum_voices; // the selected track's Drums pads: its drum plug-in's voices (Keys, the pad lights)
    // PRESETS: the entry whose programs it lists, the row under the cursor, and where B1 goes back to.
    uint8_t presets_entry = no_plugin;
    uint16_t presets_cursor = 0;
    PageId presets_back = PageId::Stage;
    std::array<char, 192> presets_folder {}; // PRESETS: the folder in view, "" the top (plugin/preset_rows.h)
    uint16_t files_cursor = 0;  // Samples, the browser: the row under the cursor
    uint16_t files_count = 0;   // rows in the folder listed, copied in by the shell
    uint8_t files_drive = 0;    // B2: 0 the card, 1 the internal drive (Drive in sample_library.h)
    bool files_audio = false;   // the row under the cursor is an audio file (K2 / K3 trim it), copied in by the shell
    BrowseTrim browse;          // and its trim sheet
    uint8_t trim_zoom = 0;      // Trim: 0 the whole sample .. 100 a few hundred frames (shell/zoom_view.h)
    ZoomView trim_view;         // and the view it keeps
    uint8_t pad_layer = 0;      // Layers (roadmap P3): the layer in hand, 0-3
    uint16_t zone = 0;          // Stage's ZONES (round 3 "instrument C"): the instrument's zone in hand; the key played picks it
    int8_t layer_load = -1;     // Samples opened by Layers' LOAD: the layer Push loads onto (1-3); -1 the pad
    uint8_t trim_focus = 0;     // and where it closes in: 0 the start, 1 the end, 2 the loop point (Shift + K1-K3)
    bool sample_held = false;   // Sample held: a pad picks a kit pad without playing it (as on the MPC)
    bool sample_used = false;   // a pad was picked meanwhile, so letting go opens no page
    uint32_t sample_down = 0;   // ms: when Sample went down (its sheet shows sample_sheet_ms after)
    bool pad_fx_down = false, knob_fx_down = false; // Pad FX, Knob FX down (their own jobs wait for the release)
    bool fx_chord_used = false;                     // both went down together: the MIX hub, and neither's own job
    // Seq held (shell/seq_layer.h): the Sequence layer on any page. When it went down, whether a control
    // did something meanwhile (its release then goes nowhere), and whether B1-B3 left for an editor (the
    // sheet stays away until Seq is let go). Each track's editor last open (0 none yet, then 1 + EditorKind).
    bool seq_held = false, seq_used = false, seq_left = false;
    uint32_t seq_down = 0;
    std::array<uint8_t, track_count> seq_editor {};
    uint8_t warp_stretch = 0;   // Shift + Pad 15: the Follow how (Beats or Tones) Time stretch goes back to
    // Chop (parity phase 5): the last pad's region cut into slices.
    ChopMode chop_mode = ChopMode::Threshold;
    std::array<int, 5> chop_amount {{50, 8, int(quarter_division), 0, int(auto_default_count)}}; // per mode: threshold, slice count, Beats' division (shell/chop.h; Manual: none), Auto's SLICES (8 << it)
    Slices slices;              // Chop's slices of the sample in hand
    HandCuts chop_hand;         // its cuts made or moved by hand: a new chop keeps them (model/chop_slices.h)
    uint16_t chop_sample = no_sample;
    uint32_t chop_from = 0, chop_to = 0; // the pad's region it chops (to 0: the end)
    uint8_t chop_slice = 0;
    uint8_t chop_zoom = 0;      // Shift + K1 / K2: 0 the whole region .. 100 a few hundred frames (shell/zoom_view.h)
    uint8_t chop_focus = 0;     // and where: 0 the slice's start, 1 its end (K2 touched); the wheel moves it
    bool chop_fine = false;     // K1 / K2 or the wheel touched: that handle drawn wider
    bool chop_zoom_auto = false; // the zoom is the wheel's (closed in on the cut it moves); the next pad opens it again
    uint32_t chop_heard_at = 0; // ms the slice in hand plays once, a moved cut gone still (0: none due)
    ZoomView chop_view;         // the view zoomed in, kept
    uint32_t chop_take_at = 0;  // ms when the take (or its part from a slice: a lit pad, the red one after) began, 0 none
    uint32_t chop_take_from = 0; // the frame it plays from
    bool chop_more = false;     // a take played out: the red stays on the next slice's pad, and tapped it plays on
    bool chop_rechop_ok = false; // issue 24 (3b): Rechop asked and answered on this visit to Chop: no more asking
    // Auto's CLEAN (B2): its cuts found on the sample's DRUMS stem (shell/stems.h), separated once for that sample.
    bool chop_clean = false;     // on, with the stem in
    bool chop_cleaning = false;  // its stems being separated, or the DRUMS stem being read
    int16_t chop_clean_slot = -1; // the DRUMS stem's sample slot; -1: none yet
    uint16_t chop_clean_for = 0;  // the sample (slot) that stem was made from
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
    // The mixer (docs/MIXER_PLAN.md): the block the card shows and K1-K3 turn (0 SENDS, 1 EQ, 2 COMP), and the EQ's
    // three bands in hand (0 LOW 63-250 Hz, 1 MID 500 Hz-2 kHz, 2 HIGH 4-16 kHz).
    uint8_t mixer_block = 0, mixer_eq_group = 0;
    // The mixer's pad bank (shell/mixer_channels.h): 0 the tracks (the one in hand ui.track), 1 the submixes, buses,
    // input and master, `mixer_other` the one in hand there (its place in that row).
    uint8_t mixer_bank = 0, mixer_other = 0;
    // B1's two pages on a track or submix (0 SENDS, 1 FX: its chain in the card), and the row − / + put SENDS and
    // COMP on (0 the first, 1 the second, as Shift shows it).
    uint8_t mixer_b1 = 0, mixer_row = 0;
    // The mixer's view: every channel of the row as a strip as Melodee starts; the channel in hand's card (its
    // block, the neighbours narrow) once Plus is held, until Minus held (shell/mixer_channels.h MixerViewHold).
    bool mixer_card = false;
    float mixer_card_from = 0;   // the view on its way from here (0 the strips, 1 the card) ...
    uint32_t mixer_card_at = 0;  // ... since then, ms (0: still)
    uint8_t sends_bus = 0;        // the bus in view on Bus (its chain page), and on a kit's Stage SENDS
    // Roadmap P4, the MOD page: modulators (0) or macros (1), the one in hand and its target in hand; the lane a pot
    // turned last (-1 none) and its track, which ADD takes (shell/mod_jobs.h).
    uint8_t mod_view = 0, mod_index = 0, mod_target = 0, macro_index = 0, macro_target = 0;
    uint8_t patterns_cards = 0; // Patterns' cards: 0 the pattern's, 1 SCENE (Shift + B1), 2 FOLLOW (Shift + B2)
    uint8_t step_cards = 0;     // Step Edit's B1 (shell/step_lane.h StepCards): NOTE, LOCK, TRIG, SOUND (kit, keygroup), EUCLID (kit or Drums lane)
    bool step_pads = false;     // Step Edit's Shift + B1 PADS: false PLAY (entry at the caret), true STEPS (the bar's steps)
    // Step SOUND's sounds on the pads (Step Edit, SAMPLE SELECT with a step held or on the SOUND cards;
    // shell/step_sounds.h): the step (-1 closed) of track `sounds_for`, the track whose pads show and their bank, and
    // B2 SLICES: the step's sample's slices on the pads instead.
    int16_t sounds_step = -1;
    uint8_t sounds_for = 0, sounds_track = 0, sounds_bank = 0;
    bool sounds_slices = false;
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
    Settings settings;                        // Melodee's own, from the card
    NameEntry name;                           // the Name sheet, over any page while open
    bool move_open = false;                   // Project: the folder picker for Move to folder
    uint16_t move_cursor = 0;
    uint16_t user_sound_count = 0;            // sounds of one's own for the track's engine, copied in by the shell
    uint8_t settings_cursor = 1;              // the row (Settings::Row) in hand behind a door
    // K1 on a row that reopens the audio device or the USB camera (Settings::reopens): the value it waits to set
    // until the knob rests (Context::settings_tick); -1 none.
    int8_t settings_wait_row = -1;
    uint8_t settings_wait_value = 0;
    uint8_t settings_door = 0;                // Settings: the door in hand (pages/settings_page.cpp)
    bool settings_in = false;                 // and behind it: its rows
    bool settings_look = false;               // SCREEN's Palette or Font row: its cards over the rows, the look turned live
    uint8_t settings_look_before = 0;         // the palette or face to go back to on BACK
    uint8_t about_view = 0;                   // About (round 2): 0 how the machine is doing, 1 the credits (B3 again)
    std::array<Pot, 3> pots {};
    int8_t turning = -1;      // the card turned last
    uint32_t turned_at = 0;   // when, in ms
    int8_t last_pot = -1;     // for Shift + wheel fine edits
    bool last_pot_shift = false; // and whether it turned its Shift card
    uint32_t motion_cursor = 0;  // Motion's cursor, a tick of the pattern: the lane in hand's point there is the one edited (shell/motion_points.h)
    bool motion_draw = false;    // Motion's draw mode (Shift + B3): the cursor and the points set by hand
    int16_t motion_pin = -1;     // a lane Push began on Motion, shown first while drawing and it has points (bindings.h motion_lanes)
    uint8_t motion_pin_track = 0;
    uint32_t held_pads = 0;
    // Shift + Erase: copy one kit pad onto another, one pattern onto another
    // (Seq held, or Patterns), one track onto another (Pad Bank held, or
    // Tracks), one key's notes onto another key (a synth's play pages, Roll,
    // Step Edit), or one step onto another (Steps). Armed, then the source,
    // then the target (shell/erase_copy.h).
    enum class Copying : uint8_t { Off, Pads, Patterns, Tracks, Keys, Steps, Notes, Scenes }; // Notes: Roll's marked notes, Push pastes; Scenes: Sequences' pads
    Copying copying = Copying::Off;
    int16_t copy_from = -1;   // the kit pad, pattern, track, key, step or sequence picked first, -1 until then
    uint8_t copy_track = 0;   // Patterns and kit pads: the track the pattern or pad is copied from
    bool moving = false;      // Tracks: a move (B2 MOVE: the two trade places) instead of a copy (shell/track_swap.h)
    void stop_copying() { copying = Copying::Off; copy_from = -1; moving = false; }
    // A question over the page that B1 (Cancel) or B3 (Do it) answers, as the
    // MPC asks before erasing: Erase + pad on Step Edit (`ask_target` the
    // key), Erase + pot on Motion (the lane, bindings.h). shell/ask.cpp.
    enum class Ask : uint8_t { None, EraseKeyEvents, EraseLane, ResetSettings, NewProject, TakeCardBack, ClearLoop, LooperOff, ClearPattern, ErasePunches, ClearTrack, InstallUpdate, Unfreeze, DeleteFile, ReplacePlugin, Rechop }; // Rechop (issue 24, Context::chop_change): Chop's slices renumbered under slice hits; target how many hits // ReplacePlugin (shell/plugin_jobs.cpp): a picked plug-in over one in use; target the picker's row, `ask_name` the old plug-in's name // DeleteFile (shell/delete_jobs.h): Erase in a browser; target what it is (DeleteWhat), `ask_name` its name // Unfreeze (freeze plan F2c): an edit of a frozen track; target the track // InstallUpdate: newer release on the card (shell/update.h); ClearLoop: target the loop; ClearPattern (Seq held + Erase + pad): target the pattern; ClearTrack (Pad Bank held + Erase + pad): target the track // NewProject (phase 10): target 0 nothing unsaved, 1 saved first, 2 lost; TakeCardBack (10b): the computer still holds the card
    Ask ask = Ask::None;
    int32_t ask_target = 0;
    std::array<char, 48> ask_name {}; // DeleteFile: the file's or the folder's name
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
    // or Shift + 16 Levels took the type at once, and the release then does nothing. Held, K1-K3 and − / + are
    // 16 Levels' own on any page (levels_held_cards); held past levels_hold_ms, or a pot or − / + turned, the
    // release does nothing either.
    bool levels_held = false, levels_used = false;
    uint32_t levels_down = 0; // ms: when 16 Levels went down
    int8_t loop_clear = -1;
    uint32_t loop_clear_at = 0;
    int8_t fx_loop = -1;
    int8_t fx_sub = -1; // the effect page (Rack) on a mixer submix's chain (Shift + Knob FX on the mixer), -1 none
    bool mixer_picker = false; // the effect picker came from the mixer's FX block (its plus): done or backed out, back there
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
    // The AUTO SAMPLE sheet (shell/auto_sample.h): the plug-in track it samples, how, which row of cards is on
    // K1-K3 (0 KEYS: low, high, step; 1 NOTE: layers, hold, tail; 2 SOUND: tap, loop, level), and whether it runs.
    struct AutoSheet {
        uint8_t track = 0;
        AutoSampleSettings settings;
        uint8_t row = 0;
        bool sampling = false;
        bool checking = false; // the check under way (settings.found its findings once heard)
        // The check is still to start: once the track's plug-in is open and the print engine is free
        // (Context::autosample_tick()).
        bool check_wanted = false;
        // Why the last check or sampling failed, its first words ("G#3 clips", "card full"), until the next starts;
        // the key it is about (an input that clips there), -1 none.
        char error[48] {};
        int16_t error_key = -1;
    };
    AutoSheet auto_sheet;
    // The STEMS sheet (shell/stems_jobs.cpp): the kit pad whose sample is separated, the sample it held when the
    // sheet opened and its length, and whether the job runs (B1 then calls it off).
    struct StemsSheet {
        uint8_t track = 0, pad = 0;
        uint16_t sample = 0;
        uint32_t frames = 0;
        bool running = false;
    };
    StemsSheet stems_sheet;
    // Seq held + Sample Record: each pattern pad freezes or thaws its pattern of the selected track until Seq is let
    // go (`seq_frozen`: the patterns as they will be then); the prints are made on the release.
    bool seq_print = false;
    std::array<uint64_t, (pattern_count + 63) / 64> seq_frozen {};
    bool seq_frozen_at(unsigned p) const { p %= pattern_count; return (seq_frozen[p / 64] >> (p % 64)) & 1; }
    // Prog layout (shell/progression_jobs.h): the card's progressions as the wheel walks them (prog_at of prog_count;
    // -1: the track's is not among them), its neighbours' names, and when the wheel last turned (Keys shows the name
    // big for a moment). Storing: the keys held when B2 STORE was pressed, until a pad takes them (B1 BACK: none).
    int16_t prog_at = -1;
    uint16_t prog_count = 0;
    char prog_prev[48] {}, prog_next[48] {};
    uint32_t prog_shown_at = 0;
    bool prog_storing = false;
    std::array<uint8_t, notes_per_step> prog_store_keys {};
    uint8_t prog_store_count = 0;
    uint8_t prog_store_from = 0; // the layout to go back to on B1 BACK
    // 16 Levels held's keys (shell/levels_keys.h): the sheet FILE or SET opened (0 none, 1 the progression files,
    // 2 SET waiting for its pad, 3 the chord being built). FILE: the rows in view, the first of them and the row in
    // hand. SET: the chord's place in the progression, its root (as heard), type and inversion on the cards, and
    // the chord as it was, for B1 BACK.
    uint8_t levels_sheet = 0;
    uint8_t levels_sheet_track = 0; // the track it was opened on: another track selected closes it
    std::array<std::array<char, 40>, 6> prog_rows {};
    uint8_t prog_rows_count = 0;
    uint16_t prog_rows_first = 0, prog_cursor = 0;
    uint8_t chord_index = 0, chord_root = 0, chord_type = 0, chord_invert = 0;
    ProgChord chord_before;
    // A word over the header for a moment: what an undo, redo, delete or copy did.
    char notice[32] {};
    uint32_t notice_at = 0;   // ms
};
// What the three pots turn changed under them (a new page of values): each picks its value up again.
inline void release_pots(UiState& ui) { for (auto& pot : ui.pots) release(pot); }

// Chop's Manual chop with no cut yet (one slice, the region) is armed: pad 1 blinks red and starts the take. With cuts
// it is not, however it came to be (K3 onto Manual, a restore, an undo, Split): the cuts show and pad 1 plays slice 1.
inline bool chop_fresh(const UiState& ui) { return ui.chop_mode == ChopMode::Manual && ui.slices.count <= 1; }
inline bool chop_armed(const UiState& ui) { return chop_fresh(ui) && !ui.chop_take_at; }

} // namespace melodee
