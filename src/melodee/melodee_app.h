// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_host.h"
#include "melodee/shell/input_route.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/snapshot.h"
#include "drivers/usb_audio.h"
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
    AudioFormat audio_format() const override { return {shell.audio_rate(), shell.audio_periods()}; } // any thread: Settings'
    void audio_opened(const AudioFormat& format) override;              // audio thread
    const UsbStems* stems() const override { return &stem_block; }    // audio thread
    void surface(const SurfaceEvent& event) override;                   // MIDI thread
    void lights(SurfaceLights& lights, Clock::time_point now) override; // MIDI thread
    void midi(const MidiPortInfo& port, const MidiPacket& packet) override; // MIDI thread: external MIDI in (parity phase 9)
    void midi_lost() override;                                             // MIDI thread
    void midi_output(MidiOutput& out, Clock::time_point now) override;   // MIDI thread: MIDI out and the sync jack, timed
    void draw(juce::Graphics& g) override;                              // screen thread
    void service(Hardware& hardware) override;                          // service thread
    void jack(const JackEvent& event, Hardware& hardware) override;     // service thread
    std::string command(const std::string& action, const std::string& argument, Hardware&) override;
    std::string command_names() const override { return "melodee-press, melodee-midi"; }
    std::string status() const override;
    bool claims_wheel_push() const override { return true; }
    bool finished() const override { return shell.leaving(); }

private:
    uint32_t now_ms() const;
    void card_access(Hardware& hardware); // service thread: SD card access (parity phase 10b)
    Clock::time_point started = Clock::now();
    melodee::EngineHost host;
    UsbStems stem_block {}; // audio thread: each track's mono stem for USB-C
    melodee::Shell shell;
    melodee::SampleLibrary library; // service thread: files on the card
    melodee::ProjectStore store;    // service thread: projects on the card
    melodee::InputRoute input_route; // service thread: Input Config's source on the hardware, USB-C in, the speaker
    std::unique_ptr<melodee::Snapshot> frame; // screen thread
    uint64_t power_presses = 0;               // service thread
    // `melodee-press [shift+]BUTTON|wheel+N|wheel-N|padN`: the service thread
    // queues, the MIDI thread acts it out as if by hand.
    struct Remote { uint8_t kind = 0, index = 0; int8_t delta = 0; }; // kind 0 button (index | 0x80 Shift), 1 wheel, 2 pad (delta: held button + 1), 3 pot (index | 0x80 Shift, delta: 0..127), 4 hold (delta 1) or release a button, 5 the same for a pad
    MidiQueue<16, Remote> presses;
    // `melodee-midi [usb] HEX...`: one message as if it came in on the External (or USB) port, for checks without a cable.
    struct RemoteMidi { bool usb = false; MidiPacket packet; };
    MidiQueue<16, RemoteMidi> remote_midi;
    bool power_known = false;
    melodee::CardNote card_note = melodee::CardNote::None; // service thread: why the last lend did not start
    uint64_t card_returns = 0;
    unsigned screen_applied = 0; // the brightness % last set; 0: never touched (the runtime restores it on leaving)
};
