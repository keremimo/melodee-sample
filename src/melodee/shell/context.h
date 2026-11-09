// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/command.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/project.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/ui_state.h"
#include <array>
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
    // Mix
    void set_mute(unsigned track, bool on);
    void set_solo(unsigned track, bool on);
    // Steps
    void set_step(unsigned track, unsigned step, const Step& value);
    // Motion: a pot move while recording and playing; Erase + pot on Motion.
    void record_motion(int param, int value);
    void clear_lane(int param);
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
    bool files_match(const std::string& path) const { return ui.files_filter == 0 || path.find("/Recorded/") != std::string::npos; }
    // Effects: chain numbers as in model/project.h (tracks, buses, master).
    void set_effect(unsigned chain, unsigned slot, EffectKind kind); // kind None removes it
    void set_effect_param(unsigned chain, unsigned slot, unsigned param, int value);
    void toggle_bypass(unsigned chain, unsigned slot);
    void move_effect(unsigned chain, unsigned slot, int way);
    void sync_chain(unsigned chain); // after moving or removing: the whole chain again
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
    unsigned held_pads() const { unsigned n = 0; for (auto& pad : pad_keys) n += pad[0] >= 0; return n; }
    std::array<uint32_t, 4> taps {};
    unsigned tap_count = 0;
    void record(int key, int velocity);
};

} // namespace melodee
