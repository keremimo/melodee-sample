// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/command.h"
#include "melodee/model/sound_file.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/project.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/history.h"
#include "melodee/shell/ui_state.h"
#include <array>
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
class SampleLibrary;
class ProjectStore;

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
    // The last take: where each note fell (for Quantise), and while Seq
    // Record is on, the edit it becomes in the history when it goes off.
    struct TakeNote { uint16_t step = 0; uint8_t key = 0; bool late = false; };
    std::array<TakeNote, 256> take_notes {};
    unsigned take_count = 0;
    std::unique_ptr<Edit> take_edit;
    int take_track = -1, take_pattern = -1;
    bool played_song = false; // the last Play was the song's
    EngineHost& host;
    uint32_t now = 0; // ms, set for each control

    Track& track() { return project.tracks[ui.track % track_count]; }
    Pattern& pattern() { return patterns.at[ui.track % track_count][track().pattern % pattern_count]; }

    void send(const Command& command);
    // Pages
    void open(PageId page);
    void back();
    void select_track(unsigned track);
    // Sound
    void play_pad(unsigned pad, uint8_t velocity, bool down); // the selected track, through its layout
    void note(unsigned track, int key, int velocity);         // velocity 0: off
    void set_param(unsigned track, int id, int value);
    void load_sound(unsigned track, EngineKind kind, unsigned sound);
    void audition(int sound);                                 // -1: back to the loaded sound
    void apply(const Binding& binding, int value);            // a pot's new value
    void set_route(unsigned route, const ModRoute& value);   // the selected track's mod matrix
    void pressure(unsigned pad, uint8_t value);             // poly aftertouch from a held pad
    // Punch: a pad of the Punch page (hold to punch, Shift + pad locks it on
    // or off), and a kit pad on Pad Punch (the pad plays punched by its own).
    void punch_pad(unsigned pad, bool down);
    void pad_punch(unsigned pad, uint8_t velocity, bool down);
    void punch_release();                                   // let go of every punch a pad holds
    void change_setting(unsigned row, int way);             // one step along a Settings row, kept on the card
    void toggle_favourite();                                // the sound under the Sounds cursor, else the loaded one
    // The Name sheet: what is named, its first text and how long it may be.
    void open_name(NameTarget target, const std::string& initial, unsigned limit, bool capitals);
    void finish_name();                                     // Done: the name goes where it was asked for
    void cancel_name() { ui.name.target = NameTarget::None; }
    void name_project(bool as_new);                         // Save (no file yet) or Save as new: a name first
    void rename_project(const std::string& path, const std::string& name); // Rename on the file sheet
    void name_marker(unsigned bar);                         // Shift + Push on a marker
    void save_sound();                                      // Shift + Sample Select away from Sounds
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
    void double_length();     // Shift + Loop: the pattern twice as long, played twice
    void quantise_take();     // Shift + Seq: the last take's late notes onto their nearest step
    void play_continue();     // Shift + Play: on from where it stopped
    void transpose(int way);  // Shift + Minus / Plus on Keys: the pads by a semitone
    void set_recording(bool on); // Seq Record: a take begins; when it ends it goes into the history
    void sync_punches();
    // Mix
    void set_mute(unsigned track, bool on);
    void set_solo(unsigned track, bool on);
    // Steps
    void set_step(unsigned track, unsigned step, const Step& value);
    // Motion: a pot move while recording and playing; Erase + pot on Motion.
    void record_motion(int param, int value);
    void clear_lane(int param);
    // Undo and redo (Shift + Minus / Plus): pad deletes and copies, takes,
    // and pattern edits made through edit_pattern() go into the one history.
    History edits;
    Edit begin_edit(const char* what, unsigned t, int p, bool song); // what may change, as it is now
    void commit_edit(Edit&& edit);                                    // as it is after; kept if anything changed
    void apply_edit(const Edit& edit, bool after);
    void restore_song(const Song& value);
    void restore_motion(const Motion& value);
    void undo();
    void redo();
    void notice(const std::string& text);                    // a word over the header for a moment
    void delete_pad(unsigned kit);                           // Erase + pad on a kit: the pad empty
    void copy_pad(unsigned from, unsigned to);               // Shift + Erase, pad, pad: sound and effects
    void restore_pattern(unsigned track, unsigned pattern, const Pattern& value);
    void restore_pad(unsigned track, unsigned kit, const PadState& value);
    // Runs `change` on a pattern of a track (and its motion) and keeps it as one edit, if it changed anything.
    template <class Change> void edit_pattern(unsigned t, unsigned p, const char* what, Change&& change) {
        Edit edit = begin_edit(what, t, int(p), false);
        change();
        commit_edit(std::move(edit));
    }
    static bool same_step(const Step& a, const Step& b) {
        for (unsigned n = 0; n < notes_per_step; ++n)
            if (a.notes[n].key != b.notes[n].key || a.notes[n].velocity != b.notes[n].velocity) return false;
        return a.length == b.length && a.chance == b.chance && a.flags == b.flags;
    }
    // Patterns and Length
    void launch(unsigned track, unsigned pattern);           // starts on the next bar while playing
    void clear_pattern(unsigned track, unsigned pattern);
    void copy_pattern(unsigned track, unsigned from, unsigned to);
    void set_length(unsigned steps);                         // the selected track's pattern
    void set_division(Division division);
    // Sampler tracks
    SampleLibrary* library = nullptr; // set by the app; the service thread reads the files
    // Projects on the card
    ProjectStore* store = nullptr;
    std::string project_path;         // the file this project came from or went to; empty when new
    std::string project_folder;       // the Project page's folder, relative to the root
    bool dirty = false;               // changed since it was saved or loaded
    void save_project_to(const std::string& path);
    void save_project();              // to its file, or a new one
    void new_project();
    // Switches the whole app to `project`: the audio thread gets a copy, samples reload.
    void switch_to(const Project& project, const PatternBank& patterns, const std::string& path);
    unsigned samples_to_load = sample_slots; // next sample slot to (re)load after a switch
    PadSound& pad() { return track().pads[track().last_pad % sampler_pads]; }
    void send_pad(unsigned track, unsigned pad);
    int sample_slot(const std::string& path, const std::string& name); // the project's slot for a file, asking for it to load
    void rechop();                                                     // Chop's markers from its mode and amount
    void chop_to_pads();                                               // the slices onto pads, from the first free one
    void record_press();
    void follow_tempos(); // Follow on: each pad's own tempo, from its length as whole bars
    bool host_recording() const;                                       // a take is armed or running                                               // Sample Record on the Record page: arm, start now, stop
    bool files_match(const std::string& path) const { return sample_file_listed(project, ui.files_filter, path); }
    // Effects: chain numbers as in model/project.h (tracks, buses, master).
    void set_effect(unsigned chain, unsigned slot, EffectKind kind); // kind None removes it
    void set_effect_param(unsigned chain, unsigned slot, unsigned param, int value);
    void toggle_bypass(unsigned chain, unsigned slot);
    void move_effect(unsigned chain, unsigned slot, int way);
    void sync_chain(unsigned chain); // after moving or removing: the whole chain again
    void send_fx(Command::Type type, unsigned chain, unsigned slot, unsigned param, int16_t value);
    void clear_fx_motion(unsigned chain);
    void set_send_bus(unsigned track, unsigned bus);
    void set_return(unsigned bus, int level);
    // Song: after any change to project.song, send it to the audio thread.
    void sync_song();
    // What the pads played lately, for Capture: track, key, velocity, when and how long (ms).
    struct Played { uint8_t track = 0, key = 0, velocity = 0; uint32_t at = 0, length = 0; };
    std::array<Played, 512> history {};
    unsigned history_at = 0;
    void set_arp(bool on);
    void set_click(bool on);
    // Capture: the selected track's last bars from the history into a pattern.
    void capture();
    // Transport
    void play();
    void stop();
    void tap_tempo();
    void set_tempo(int tenths);

    // The keys each pad started (a chord has up to four), so a release ends them.
    std::array<std::array<int16_t, notes_per_step>, 16> pad_keys = [] {
        std::array<std::array<int16_t, notes_per_step>, 16> keys {};
        for (auto& pad : keys) pad.fill(-1);
        return keys;
    }();
    void key(int key, int velocity, bool down);

private:
    void quantise_notes();
    void double_notes();
    void capture_notes();
    unsigned held_pads() const { unsigned n = 0; for (auto& pad : pad_keys) n += pad[0] >= 0; return n; }
    std::array<uint32_t, 4> taps {};
    unsigned tap_count = 0;
    void record(int key, int velocity);
};

} // namespace melodee
