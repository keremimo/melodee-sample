// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/command.h"
#include "melodee/model/sound_file.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/pad_params.h"
#include "melodee/model/project.h"
#include "melodee/model/project_file.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/history.h"
#include "melodee/shell/page_stack.h"
#include "melodee/shell/undo_scope.h"
#include "melodee/shell/ui_state.h"
#include <array>
#include <bitset>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>
#include <string>

// What a page's controls may do, on the MIDI thread: change the project and
// the UI state, and send the matching Commands to the audio thread. Each
// method keeps the model and the audio mirror in step.
namespace melodee {

class EngineHost;
class PluginRack;
struct PluginFile;
class SampleLibrary;
class ProjectStore;
class InputRoute;
struct CardLink;
struct LoadedSample;

class Context {
public:
    Context(Project& project, PatternBank& patterns, UiState& ui, EngineHost& host)
        : project(project), patterns(patterns), ui(ui), host(host) {}
    Project& project;
    PatternBank& patterns;
    UiState& ui;
    // Seq Record into the song (song_tick()).
    uint16_t recording_tracks = 0;                    // tracks whose launch is being written
    std::array<uint16_t, track_count> recording_from {}; // where each one's clip begins
    unsigned seen_song_bar = ~0u;
    int recording_punch = -1;                          // the punch being written, -1 none
    SongPunch punch_take {};
    void record_launch(unsigned track, unsigned pattern);
    void finish_launches();
    void begin_punch(unsigned kind, uint8_t target, uint8_t depth);
    void end_punch();
    unsigned song_sixteenth() const;                   // where the song is, in sixteenths
    // While Seq Record is on: the edit the take becomes in the history when it goes off.
    std::unique_ptr<Edit> take_edit;
    // Notes the take wrote that are still held: their length is what was played.
    struct Held { int32_t key = -1; uint8_t track = 0, pattern = 0; uint32_t tick = 0, since = 0; };
    std::array<Held, 16> held_notes {};
    int take_track = -1, take_pattern = -1;
    bool in_take(unsigned t, unsigned p) const { return take_edit && take_track == int(t) && take_pattern == int(p); } // Seq Record on this pattern
    bool played_song = false; // the last Play was the song's
    EngineHost& host;
    uint32_t now = 0; // ms, set for each control
    // CLOCK_MONOTONIC ns the control or MIDI message being handled came in (its receive stamp; 0: none, a timer's
    // or a remote press): its notes play a block after that frame (EngineHost::send_at) and Seq Record places
    // them where they were heard.
    uint64_t hit_ns = 0;

    Track& track() { return project.tracks[ui.track % track_count]; }
    Pattern& pattern() { return patterns.at[ui.track % track_count][track().pattern % pattern_count]; }

    void send(const Command& command);
    // Pages
    void open(PageId page);
    void back();
    void select_track(unsigned track);
    // Stage (pages/stage_page.cpp): PRESET's sound turned to `at` (its place in sound_list()), loading once the
    // turning has stopped stage_preset_ms; stage_tick() loads it then. Push on an empty track: it becomes what
    // the page in view says (SYNTH, KIT, MIDI).
    void preset_turn(int at);
    void stage_tick(uint32_t at); // often (ms); UINT32_MAX loads a waiting sound now
    void stage_new();
    // Sound
    void play_pad(unsigned pad, uint8_t velocity, bool down); // the selected track, through its layout
    void note(unsigned track, int key, int velocity);         // velocity 0: off
    void set_param(unsigned track, int id, int value);
    void load_sound(unsigned track, EngineKind kind, unsigned sound);
    void audition(int sound);                                 // -1: back to the loaded sound
    void apply(const Binding& binding, int value);            // a pot's new value
    void set_route(unsigned route, const ModRoute& value);   // the selected track's mod matrix
    void pressure(unsigned pad, uint8_t value);             // poly aftertouch from a held pad
    // Punch: a pad of the Punch page (hold to punch), Push with it held to
    // lock it on (Push again unlocks), and a kit pad on Pad Punch (the pad
    // plays punched by its own).
    // Pad FX (parity phase 7b, shell/pad_fx.cpp): four at once, B1 latch, pressure as depth.
    void punch_pad(unsigned pad, bool down);
    void latch_punch();                                     // B1 (or Push): the punch last pressed latched, or let go
    void pad_punch(unsigned pad, uint8_t velocity, bool down);
    void punch_release(bool latched_too = false);           // let go of every punch a pad holds (and the latched ones)
    bool punch_pressure(unsigned pad, uint8_t value);       // a held punch's depth from its pad's pressure
    void set_punch_target(unsigned target);                 // 0 the master, 1 the selected track (everything lets go)
    void step_punch_bank(int way);                          // Melodee 1, Melodee 2, MPC
    void set_pad_fx_param(unsigned mpc, unsigned param, int value); // an MPC Pad FX's K1-K3
    void sync_live();                                       // the newest four engaged punches into the live slots
    uint8_t punch_target_now() const;
    void flex_pad(unsigned pad);                            // Flex Beat: pad 1 Empty, 2-16 its effects
    void set_flex(unsigned what, int value);                // as Command::FlexBeat's index: 1 mode, 2 mix, 3 quantize
    void change_setting(unsigned row, int way);             // one step along a Settings row, kept on the card
    void toggle_favourite();                                // the sound under the Sounds cursor, else the loaded one
    // The Name sheet: what is named, its first text and how long it may be.
    void open_name(NameTarget target, const std::string& initial, unsigned limit, bool capitals);
    void finish_name();                                     // Done: the name goes where it was asked for
    void cancel_name() { ui.name.target = NameTarget::None; }
    void name_project(bool as_new);                         // Save (no file yet) or Save as new: a name first
    void rename_project(const std::string& path, const std::string& name); // Rename on the file sheet
    void name_marker(unsigned bar);                         // Shift + Push on a marker
    void save_sound();                                      // Shift + Sample Select away from Sounds, synth tracks
    void save_sample();                                     // Shift + Sample Select on a kit: the Name sheet for Save Sample
    void save_bank();                                       // Shift + Sample Select on Samples: the Name sheet for Save Bank
    std::string name_path;                                  // Rename: the file being renamed
    unsigned name_bar = 0;                                  // Marker: its bar
    // Sounds of one's own on the card, copied in by the shell; the k-th of an engine's.
    std::vector<UserSound> user_sounds;
    const UserSound* user_sound(EngineKind engine, unsigned k) const;
    unsigned user_sound_count(EngineKind engine) const;
    // Seq Record while the song plays: pattern launches become clips from the
    // next bar, growing a bar at a time; punches land on the song's punch lane.
    void song_tick();                                       // MIDI thread, often: follows the song's bar
    // Shift's second jobs.
    void double_length();     // Shift + Pad 3 (Double Seq): the pattern twice as long, played twice
    void half_length();       // Shift + Pad 2 (Half Seq): the first half only
    void half_speed();        // Shift + Pad 6: every note twice as far apart (what falls past the end goes)
    void double_speed();      // Shift + Pad 7: every note half as far apart
    void time_correct();      // Time Correct's B3: the chosen notes onto Q, swung and shifted; one undo
    void toggle_reverse();    // Shift + Loop: the last pad's reverse
    // Sample Mode jobs on the last pad hit (parity phase 4, shell/sample_mode.cpp).
    void toggle_loop();       // Loop
    void toggle_note_on();    // Shift + Chop: Note On (Gate) or One shot
    void toggle_loop_lock();  // Shift + B1 on Trim: the loop point back on the start, or free
    void normalize();         // Shift + B2 on Trim: the region's loudest point to 0 dB, one undo
    void toggle_warp_mode();  // Shift + Pad 15: Warp by Time stretch or Pitch (the kit's Follow how)
    void set_recording(bool on); // a take begins; when it ends it goes into the history
    void seq_record();           // Seq Record: stopped, arm (Play starts the take); playing, record at once; recording, stop recording
    void sync_punches();
    // Mix
    void set_mute(unsigned track, bool on);
    void set_solo(unsigned track, bool on);
    // Steps
    void set_step(unsigned track, unsigned step, const Step& value);
    // The pattern's notes changed in the model: the audio thread gets a copy to play.
    void sync_pattern(unsigned track, unsigned pattern);
    void free_retired();           // pattern copies the audio thread is done with
    // Motion: a pot move while recording and playing (`lane` as in
    // bindings.h: a parameter, or a kit pad's value on one pad); Erase + pot
    // on Motion, once B3 says so (one undo).
    void record_motion(int lane, int value);
    void record_pad_motion(PadParam which, int value);
    void clear_lane(int lane);
    // Roadmap P1: Motion is each pattern's own. A pot turned with a step (Steps) or a note (Roll, a pad held on it)
    // held writes a lock there: `lane` holds `value` from `tick` for `hold` ticks. Recorded moves reach the audio
    // thread at most every motion_sync_ms (a pattern copy each), flush_motion() sending the last when it is due.
    void write_lock(int lane, uint32_t tick, uint16_t hold, int value);
    void sync_motion(unsigned track, unsigned pattern);
    void flush_motion();
    static constexpr uint32_t motion_sync_ms = 40;
    int motion_waiting = -1;  // track << 8 | pattern whose recorded motion the audio thread has not had yet
    uint32_t motion_synced = 0;
    // Undo and redo (Shift + Minus / Plus): pad deletes and copies, takes,
    // and pattern edits made through edit_pattern() go into the one history.
    History edits;
    Edit begin_edit(const char* what, unsigned t, int p, bool song); // what may change, as it is now
    void commit_edit(Edit&& edit);                                    // as it is after; kept if anything changed
    void apply_edit(const Edit& edit, bool after);
    void restore_song(const Song& value);
    void restore_motion(const TrackMotion& value); // a track's patterns' motion as it was (an effect moved or removed)
    TrackMotion track_motion(unsigned track) const;
    void undo();
    void redo();
    // Parity phase 10 (shell/undo_scope.cpp): a pot's turns are one step per control, closed by another control, any
    // press, or gesture_quiet_ms still; a press on a page is one step of what the page edits (PageEdit).
    struct Gesture { std::unique_ptr<Edit> edit; UndoScope scope; uint32_t key = 0, last = 0; };
    Gesture gesture;
    void touch_gesture(const Binding& binding);  // before a pot changes a value
    void touch_scope(const UndoScope& scope, uint32_t key); // the same for a control with no card (the fader's nudge)
    void close_gesture();                         // the open gesture kept, if it changed anything
    // Roll R0: a pad held on Roll, Steps or Step Edit is one step of the pattern from press to release; the
    // wheel, pots, fader and buttons used meanwhile on the same pattern are part of it. Undo while it is
    // held keeps it first, so the note just written is what goes.
    struct Hold { std::unique_ptr<Edit> edit; UndoScope scope; unsigned pad = 0, pushes = 0; };
    Hold hold;
    void open_hold(unsigned pad);                 // before a pad's press
    void close_hold(unsigned pad);                // after its release: kept, if it changed anything
    bool held(const UndoScope& scope) const { return hold.edit && hold.scope == scope; }
    std::unique_ptr<Edit> scope_edit(const UndoScope& scope, const std::string& what); // what may change, as it is now
    bool finish_scope(Edit& edit);                // as it is after; false when nothing changed
    void restore_mix(const MixState& value);
    void restore_sound(unsigned track, const SoundState& value);
    void restore_master(const MasterState& value);
    void restore_chain(unsigned chain, const Chain& value);
    void restore_chop(const ChopState& value);
    void notice(const std::string& text);                    // a word over the header for a moment
    void delete_pad(unsigned kit);                           // Erase + pad on a kit, stopped: the pad empty
    void erase_pad_notes(unsigned kit);                      // playing or recording: its notes out of the pattern, one edit
    void copy_pad(unsigned from, unsigned to);               // Shift + Erase, pad, pad: sound and effects
    void restore_pattern(unsigned track, unsigned pattern, const Pattern& value);
    void restore_pad(unsigned track, unsigned kit, const PadState& value);
    // Runs `change` on a pattern of a track (and its motion) and keeps it as one edit, if it changed anything.
    // During a take on that same pattern it is part of the take instead: one undo for both.
    template <class Change> void edit_pattern(unsigned t, unsigned p, const char* what, Change&& change) {
        if (in_take(t, p)) { change(); return; }
        Edit edit = begin_edit(what, t, int(p), false);
        change();
        commit_edit(std::move(edit));
    }
    // Patterns (their cards: length, Q, swing; Shift: any bars, time signature, click)
    void launch(unsigned track, unsigned pattern);           // starts on the next bar while playing
    void clear_pattern(unsigned track, unsigned pattern);
    // A pattern onto another of `track`; from_track, when given, is where it comes from.
    void copy_pattern(unsigned track, unsigned from, unsigned to, int from_track = -1);
    void set_length(uint32_t ticks);                         // the selected track's pattern
    void set_division(Division division);
    void set_signature(unsigned index);                      // the project's; patterns of whole bars keep their bar count
    void set_seq_tempo(bool on);                             // Push on Tempo: BPM per pattern (SEQ) or global (GBL)
    void locate(int way);                                    // the wheel on Patterns: by a beat (Shift: by Q)
    unsigned tempo_now() const { return project.tempo_of(ui.lead_pattern); }
    // Plug-ins (shell/plugin_jobs.cpp, docs/MELODEE.md): the rack (set by the app); the picker opened for the selected
    // PLUGIN track's instrument or for a chain's slot; Push on a plug-in there; a card's value; the rack kept in step
    // with the project and the panel of the plug-in in view refreshed (often, from the shell's tick).
    PluginRack* plugins = nullptr;
    void open_plugin_picker(UiState::PluginTarget target, unsigned chain = 0, unsigned slot = 0);
    void choose_plugin(const PluginFile& file);
    void set_plugin_param(int32_t id, int value);
    void open_plugin_presets(unsigned entry); // PRESETS for that plug-in; B1 there comes back here
    void choose_plugin_program(int index);    // PRESETS: Push on the row at `index`
    void plugins_tick(uint32_t now);
    int plugin_in_focus() const; // the entry the page in view shows, -1 none
    void split_shared_plugins(); // a copy's second user of an entry gets one of its own
    uint32_t plugin_panel_at = 0;
    unsigned plugin_view_seen = ~0u;
    bool plugin_came_back = true; // false after going up, until the cursor lands on the folder it came out of
    // Sampler tracks
    SampleLibrary* library = nullptr; // set by the app; the service thread reads the files
    // Projects on the card
    ProjectStore* store = nullptr;
    CardLink* card = nullptr;          // SD card access (shell/card_access.h): set by the shell
    void card_access();                // Project's SD card access row: lend the card, open its page
    std::string project_path;         // the file this project came from or went to; empty when new
    std::string project_folder;       // the Project page's folder, relative to the root
    bool dirty = false;               // changed since it was saved or loaded
    void save_project_to(const std::string& path);
    void save_project();              // to its file, or a new one
    void new_project();
    // Parity phase 10 (shell/project_jobs.cpp): New project asks first (B3 / B1); Load Kit.
    void ask_new_project();                   // the question, with what happens to unsaved changes
    void start_new_project();                 // its B3: saved first if it has a file, then a new one, Stage
    void load_kit_file(const std::string& path, const std::string& name); // a project's kit onto the selected track, once loaded
    void load_kit(const Project& from, const std::string& name);          // that kit onto the selected track, one undo
    std::string kit_from;                     // the file Load Kit waits for
    // Kit bank files (shell/project_jobs.cpp, model/project_file.h): Push on one in the Samples browser
    // loads it into the selected kit's bank in view, once the service thread has read it.
    void load_bank_file(const std::string& path, const std::string& name);
    void load_bank(const BankFile& bank, const std::string& name); // into the bank in view, one undo
    std::string bank_from;                    // the file Load Bank waits for
    // Switches the whole app to `project`: the audio thread gets a copy, samples reload.
    // `plugin_states`: what its file kept of each plug-in (none: each opens at its defaults).
    void switch_to(const Project& project, const PatternBank& patterns, const std::string& path,
                   const std::array<std::vector<uint8_t>, max_plugins>* plugin_states = nullptr);
    // The device's rate changed (Settings' SAMPLE RATE): the same project again, so the audio thread works out every
    // engine and effect anew at it. It stops playback. False when the queue was full (try again).
    bool reformat();
    unsigned samples_to_load = sample_slots; // next sample slot to (re)load after a switch
    std::vector<uint8_t> slots_waiting;      // slots sample_slot() named while the library's queue was full: asked for again each pass
    PadSound& pad() { return track().pads[track().last_pad % sampler_pads]; }
    void send_pad(unsigned track, unsigned pad);
    int sample_slot(const std::string& path, const std::string& name); // the project's slot for a file, asking for it to load
    void rechop();                                                     // Chop's slices from its type and amount
    void chop_tap(unsigned pad, uint8_t velocity);                     // Chop: a pad (audition, or Manual's taps)
    uint32_t zero_cut(uint32_t frame) const;                           // Chop: a cut on a zero crossing, with Zero Snap on
    void snap_slices();                                                // Chop: every cut of the slices onto one
    void toggle_zero_snap();                                           // Chop, B2: Zero Snap on / off (a setting)
    void chop_to_pads();                                               // the slices onto pads, from the first free one
    void follow_tempos(); // Follow on: each pad's own tempo, from its length as whole bars
    bool host_recording() const;                                       // a take is armed, cued or running
    // Input Config and Sample Record (parity phase 6, shell/sample_record.cpp).
    InputRoute* input = nullptr;                          // set by the app: the hardware behind the source
    void set_input_source(InputSource source, unsigned track = 0); // K1 on Input Config and Record (track: InputSource::Track)
    void set_input_tap(bool post);                        // Shift + K1 there: a track before or after its effects
    // Record with Melodee's own sound as the source (a track, the mix): from the pad tapped until the take ends, the
    // pads play the instrument instead of cutting the take (a track source: that track; Resample: the kit itself).
    bool record_keys() const;
    void record_key(unsigned pad, uint8_t velocity, bool down);
    bool record_key_held(unsigned pad) const { return (record_key_pads >> (pad % 16)) & 1u; }
    void record_octave(int way);                          // Minus / Plus meanwhile: the source track's pads an octave
    void cycle_monitor();                                 // B1: Off -> Auto -> On
    void toggle_rec_fx();                                 // B2 on Input Config: Rec Input FX
    void input_changed();                                 // a setting moved: the engine hears it, the card keeps it (soon)
    void record_pad(unsigned kit);                        // a pad on Record: record onto it; during a take, cut it there
    void record_press();                                  // Push on Record: arm onto the next empty pad, start now, stop
    void record_button();                                 // Sample Record on Record: a take ends; back to the pad's page
    void audio_recall();                                  // Shift + Sample Record: the ring's last 25 s onto the next empty pad
    void record_tick(uint32_t at);                        // often (ms): monitor and route wishes, settings kept, renders followed
    uint32_t settings_save_at = 0;                        // ms; 0 nothing waiting
    uint32_t rec_armed_at = 0;                            // ms
    bool monitor_sent = false, monitor_known = false;
    int loop_monitor_sent = -1;
    int gain_sent = 0;
    uint16_t record_key_pads = 0;                         // pads playing through record_key()
    // Renders of the mix (parity phase 6, shell/render.cpp): Resample (Shift + Pad 11, then a pad) and the song's mixdown.
    void resample();                                      // Shift + Pad 11: wait for the pad (again: not)
    void resample_pad(unsigned kit);                      // the pad: the pattern rendered onto it
    void mixdown(const std::string& name);                // Song's export, Audio mixdown, after the Name sheet
    void render_tick(uint32_t at);                        // a render ended: transport back as it was
    bool render_started = false;
    uint32_t render_since = 0;
    // Song -> new pattern (parity phase 6, shell/song_export.cpp): the song's clips as one long pattern on every track, one undo.
    void song_to_pattern();
    void song_export_soft(int key);                       // the export sheet's B1 Cancel, B2 Audio mixdown, B3 New pattern
    // Sample files (parity phase 5, shell/sample_files.cpp): files made from
    // a pad's audio on the service thread, and the browser's loads.
    void trim_sample();                                  // Shift + Pad 13: the last pad's region as a file of its own, one undo
    void extract_slice();                                // Chop, Shift + B1: the slice as a file, onto the next empty pad
    void save_sample_as(const std::string& name);        // Save Sample's Name sheet, Done: the region as Samples/<name>.wav
    void load_file(const std::string& path, const std::string& name, bool next_empty); // the browser's Push, Shift + pad
    void files_landed();                                 // the library changed: made files onto their pads
    int next_empty_pad(unsigned after) const;            // the kit's next pad without a sample after `after`, -1 none
    // The browser (Samples): the file under the cursor heard when B3 is on;
    // a new listing puts the cursor on the folder it came out of; a preview
    // that has arrived plays, and stops away from the page.
    void preview_cursor();
    void browser_tick();
    std::string files_folder_seen;
    unsigned previews_seen = 0;
    int previewing = -1; // the track a preview plays on, -1 none
    // land: 0 Extract (the next empty pad after `pad`), or Trim (`trim`); 1 onto `pad` itself (Recall,
    // Resample; one undo); 2 the take of Sample Record onto `pad`, with the pads cut from it (ui.rec_chop_*).
    struct PendingFile { int16_t slot = -1; uint8_t track = 0; int16_t pad = -1; bool trim = false; PadSound sound {}; uint8_t land = 0; };
    std::array<PendingFile, 8> pending_files {};
    // A sample slot held for a file the service thread makes, landing as `land` says; -1 when slots or the list are full.
    int hold_file(const std::string& name, unsigned track, int pad, uint8_t land);
    void drop_file(int slot);                            // a held slot nothing came for: free again
    void take_landed(unsigned track, unsigned kit, unsigned slot, const LoadedSample& loaded); // Sample Record's take onto its pads
    // Knob FX and the master compressor (parity phase 7, shell/knob_fx.cpp).
    void knob_fx_button();                                // Knob FX: the mode on (its page) or off
    void knob_fx_load(unsigned entry);                    // Push on the list: the entry's effect at its initial values
    void knob_fx_param(unsigned param, int value);        // K1-K3 (Shift: the second row) while the mode is on; knob_morph: the fader
    void knob_fx_edit(unsigned setting);                  // B1 A, B2 B (Knob FX C): the pots change that one, heard at it
    void knob_fx_pad(unsigned pad);                       // a pad on the page: a kit pad of the bank in view, or a track, on / off
    void knob_fx_all();                                   // Shift + B2: all on, or all off when all were on
    void knob_fx_bypass();                                // B3
    bool knob_fx_kit();                                   // its pads are kit pads (the selected track is a kit)
    void send_knob_state();                               // heard or not, and Rec Input FX
    void send_knob_targets();
    void set_compressor(unsigned what, int value);        // as Command::Compressor's index: on, Color, attack, release, amount, boost, Sidechain, key, key pad (-1 whole)
    // Effects: chain numbers as in model/project.h (tracks, buses, master).
    void set_effect(unsigned chain, unsigned slot, EffectKind kind); // kind None removes it
    void set_effect_param(unsigned chain, unsigned slot, unsigned param, int value);
    void toggle_bypass(unsigned chain, unsigned slot);
    void move_effect(unsigned chain, unsigned slot, int way);
    void sync_chain(unsigned chain); // after moving or removing: the whole chain again
    void send_fx(Command::Type type, unsigned chain, unsigned slot, unsigned param, int16_t value);
    void clear_fx_motion(unsigned chain);
    void set_return(unsigned bus, int level);
    // Song: after any change to project.song, send it to the audio thread.
    void sync_song();
    // What the pads played lately, for Capture: track, key, velocity, when and how long (ms).
    struct Played { uint8_t track = 0, key = 0, velocity = 0; uint32_t at = 0, length = 0; };
    std::array<Played, 512> history {};
    unsigned history_at = 0;
    void set_arp(bool on) { set_arp(ui.track, on); }
    void set_arp(unsigned track, bool on); // that track's arp (Note Repeat let go after the track changed)
    // The Looper (shell/looper_jobs.cpp): refusals already said, and whether a loop records (the metronome's Rec).
    std::array<unsigned, loop_tracks> loop_errors {};
    bool loop_click = false;
    void set_click(unsigned mode); // the metronome: 0 off, 1 on, 2 only while Seq Record
    void send_click();
    // Capture: the selected track's last bars from the history into a pattern.
    void capture();
    // Transport
    void play(bool external = false);          // external: MIDI clock in's Start (no count-in; the clock leads)
    void play_continue(bool external = false); // Shift + Play: on from where it stopped; external: MIDI Continue, from Song Position
    void fill(bool on);                        // Play held while playing (roadmap P1): FILL trig conditions play while it is down
    void stop();
    void tap_tempo();
    void set_tempo(int tenths);

    // The keys each pad started (a chord has up to four), so a release ends them, and the track they
    // went to: the selected track may change while the pad is down (Pad Bank + pad, Tracks).
    std::array<std::array<int32_t, notes_per_step>, 16> pad_keys = [] {
        std::array<std::array<int32_t, notes_per_step>, 16> keys {};
        for (auto& pad : keys) pad.fill(-1);
        return keys;
    }();
    std::array<uint8_t, 16> pad_track {};
    void key(int key, int velocity, bool down);
    // Ends whatever `pad` still sounds, on the track it played. Every pad release comes here, whichever
    // page or layer took it (the shell), so a page change while a pad is down never leaves a note on.
    void release_pad(unsigned pad);

    // MIDI in (parity phase 9, shell/midi_io.cpp): a note on track `t` as its pads would play it, but straight (no chord,
    // layout or response curve) and never sent out again; on the selected track it records as a pad does under Seq Record.
    void midi_key(unsigned t, int note, int velocity, bool down);
    void midi_release_all();                                // the input went away: every note it holds ends
    std::array<std::bitset<128>, track_count> midi_held {};  // keys MIDI in holds, by track
    std::array<std::array<int32_t, 128>, track_count> midi_played {}; // what each of them plays
    bool midi_every_key(const Track& tr) const;              // 16 Levels on an instrument: the whole keyboard is that pad
    // An instrument loaded onto a pad: its envelope and velocity go onto the pad once read.
    struct LandingPad { uint8_t track = 0; uint8_t pad = 0; uint16_t slot = 0; };
    std::vector<LandingPad> landing_pads;
    void land_instruments();
    bool refuse_instrument(const char* job);                 // true, and says so, when the last pad holds one
    bool from_midi = false;                                 // note() is playing what came in: not echoed out
    void change_midi(unsigned row, int way);                // one step along a MIDI Config row (shell/midi_config_rows.h)
    void reset_settings();                                  // MIDI Config's Reset Factory Settings, after B3
    // MIDI Panic (MIDI Config's row, `melodee-midi panic`): every note here ends, and MIDI out sends All Sound Off,
    // All Notes Off and pitch bend centre on every channel of every port (shell/midi_io.cpp, counting these).
    void midi_panic();
    unsigned midi_panics = 0;

private:
    void double_notes();
    void scale_notes(bool slower);
    void capture_notes();
    unsigned held_pads() const { unsigned n = 0; for (auto& pad : pad_keys) n += pad[0] >= 0; return n; }
    std::array<uint32_t, 4> taps {};
    unsigned tap_count = 0;
    void record(int key, int velocity);
    void record_release(int key);
};

} // namespace melodee
