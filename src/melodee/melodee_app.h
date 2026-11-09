// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_host.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/snapshot.h"
#include "runtime/app.h"
#include <atomic>
#include <cstdint>
#include <memory>

// Melodee on sample-base (docs/MELODEE.md). This class only hands each
// runtime hook to its owner: the engine host renders, the shell takes the
// controls and lights, the pages draw a snapshot.
class MelodeeApp : public App {
public:
    MelodeeApp();
    void render(const AudioBlock& input, AudioBlock& output) override; // audio thread
    void surface(const SurfaceEvent& event) override;                   // MIDI thread
    void lights(SurfaceLights& lights, Clock::time_point now) override; // MIDI thread
    void draw(juce::Graphics& g) override;                              // screen thread
    void service(Hardware& hardware) override;                          // service thread
    std::string command(const std::string& action, const std::string& argument, Hardware&) override;
    std::string command_names() const override { return "melodee-press"; }
    std::string status() const override;
    bool claims_wheel_push() const override { return true; }
    bool finished() const override { return shell.leaving(); }

private:
    uint32_t now_ms() const;
    Clock::time_point started = Clock::now();
    melodee::EngineHost host;
    melodee::Shell shell;
    melodee::SampleLibrary library; // service thread: files on the card
    melodee::ProjectStore store;    // service thread: projects on the card
    std::unique_ptr<melodee::Snapshot> frame; // screen thread
    uint64_t power_presses = 0;               // service thread
    // `melodee-press [shift+]BUTTON|wheel+N|wheel-N|padN`: the service thread
    // queues, the MIDI thread acts it out as if by hand.
    struct Remote { uint8_t kind = 0, index = 0; int8_t delta = 0; }; // kind 0 button (index | 0x80 Shift), 1 wheel, 2 pad, 3 pot (delta: 0..127)
    MidiQueue<16, Remote> presses;
    bool power_known = false;
};
