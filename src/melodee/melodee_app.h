// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_host.h"
#include "melodee/shell/input_route.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/print_engine.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/snapshot.h"
#include "melodee/ui/boot.h"
#include "drivers/platform.h"
#include "drivers/sd_update.h"
#include "drivers/usb_audio.h"
#include "runtime/app.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

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
    std::string command_names() const override { return "melodee-press, melodee-midi, melodee-update, melodee-print"; }
    std::string status() const override;
    bool claims_wheel_push() const override { return true; }
    bool finished() const override { return shell.leaving() || power_off_failed.load(); }

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
    melodee::PluginRack plugins;     // its own thread: the plug-ins the project uses (freed before the engine host)
    melodee::PrintEngine printer;    // its own thread: prints (freeze plan F2a), stopped before the rack and library go
    // About (round 2): how the machine is doing, read on the service thread (each core every half second, the
    // card every five) and copied into the snapshot by the screen thread.
    struct MachineReadings {
        std::array<std::atomic<uint8_t>, 8> core_load {};
        std::atomic<uint8_t> cores {0}, plugins {0};
        std::atomic<float> plugins_load {0};
        std::atomic<bool> card {false};
        std::atomic<uint64_t> card_free {0}, card_total {0};
    } machine;
    std::vector<CpuTimes> cpu_before; // service thread: the last reading of each core
    uint32_t machine_at = 0, card_at = 0; // service thread: when they were read
    void read_machine(Hardware& hardware); // service thread
    // Roadmap P2: whether the render threads keep to cores 1-3 and the rest to core 0 unless $MELODEE_RENDER_THREADS
    // says otherwise (measured on the MPC, docs/ROADMAP.md); and the service thread's sweep that keeps the rest there.
    static constexpr bool render_pinned_default = false;
    void keep_rest_off();
    uint32_t pinned_at = 0; // service thread: the last sweep
    // Roadmap P2: at each dropout the busiest track then (0 none, else 1-16) and its share of a block, for status.
    unsigned xruns_seen = 0; // service thread
    std::atomic<unsigned> dropout_track {0}, dropout_load {0}; // tenths of a percent
    void note_dropout();     // service thread
    std::unique_ptr<melodee::Snapshot> frame; // screen thread
    melodee::ui::BootAnimation boot;          // screen thread: the logo, then the app
    std::atomic<bool> boot_skip {false};      // a button or pad pressed: the boot animation fades out now
    uint64_t power_presses = 0;               // service thread
    // `melodee-press [shift+]BUTTON|wheel+N|wheel-N|padN`: the service thread
    // queues, the MIDI thread acts it out as if by hand.
    struct Remote { uint8_t kind = 0, index = 0; int8_t delta = 0; }; // kind 0 button (index | 0x80 Shift), 1 wheel, 2 pad (delta: held button + 1), 3 pot (index | 0x80 Shift, delta: 0..127), 4 hold (delta 1) or release a button, 5 the same for a pad
    MidiQueue<16, Remote> presses;
    // `melodee-midi [usb] HEX...`: one message as if it came in on the External (or USB) port, for checks without a cable.
    struct RemoteMidi { bool usb = false; MidiPacket packet; };
    MidiQueue<16, RemoteMidi> remote_midi;
    MidiQueue<16, melodee::LooperAsk> remote_looper; // `melodee-looper`, parsed on arrival, done on the MIDI thread
    // `melodee-print TRACK [PATTERNS|all] [pre|post] [loop|ring|cut]`, `status`, `cancel`: parsed on arrival, asked on
    // the MIDI thread.
    MidiQueue<4, melodee::Context::PrintAsk> remote_print;
    std::atomic<bool> panic_asked {false}; // `melodee-midi panic`
    bool power_known = false;
    bool power_off_asked = false;             // service thread: systemd asked to turn the MPC off
    std::atomic<bool> power_off_failed {false}; // it could not be asked (or this is not the MPC): leave for the stock app
    melodee::CardNote card_note = melodee::CardNote::None; // service thread: why the last lend did not start
    uint64_t card_returns = 0;
    // Updates from the card (shell/update.h, base drivers/sd_update.h): service thread only.
    void card_update(Hardware& hardware);
    bool update_ready();                       // sd-update.sh is next to the binary and the key file is written
    sd_update::Setup update_setup;
    bool update_setup_tried = false;
    bool update_installing = false;            // an install started by this app is running
    bool update_settled = false;               // the status of an earlier install has been read
    bool update_rescan = false;                // `melodee-update`: check again now
    std::string update_card, update_zip, update_version; // folder checked, zip offered, its version
    std::string update_override;               // `melodee-update DIR`: folder to check instead of the card
    uint64_t update_returns = ~0ull;           // CardShareStatus::returns at the last check
    std::vector<std::string> update_skip;      // zips skipped (LATER or failed) until the card is returned
    uint32_t update_looked_at = 0, update_polled = 0, update_started = 0; // ms
    std::atomic<bool> update_offered {false};  // for status
    std::string update_note;                   // notice held until the boot animation has ended
    unsigned screen_applied = 0; // the brightness % last set; 0: never touched (the runtime restores it on leaving)
};
