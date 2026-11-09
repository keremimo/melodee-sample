// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/command.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/project.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/ui_state.h"
#include <array>
#include <cstdint>
#include <functional>

// What a page's controls may do, on the MIDI thread: change the project and
// the UI state, and send the matching Commands to the audio thread. Each
// method keeps the model and the audio mirror in step.
namespace melodee {

class EngineHost;

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
    // Mix
    void set_mute(unsigned track, bool on);
    void set_solo(unsigned track, bool on);
    // Steps
    void set_step(unsigned track, unsigned step, const Step& value);
    // Motion: a pot move while recording and playing; Erase + pot on Motion.
    void record_motion(int param, int value);
    void clear_lane(int param);
    // Transport
    void play();
    void stop();
    void tap_tempo();
    void set_tempo(int tenths);

private:
    std::array<int16_t, 16> pad_keys {{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}};
    std::array<uint32_t, 4> taps {};
    unsigned tap_count = 0;
    void record(int key, int velocity);
};

} // namespace melodee
