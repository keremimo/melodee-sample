// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// An armv7 VST2 plug-in, run in a 32-bit vst2_bridge32 process and wrapped
// as a JUCE AudioPluginInstance for the 64-bit host. Audio, MIDI and calls
// cross in one shared-memory block (vst2_bridge_protocol.h); processBlock
// hands the block over and waits for it in the same period, so there is no
// added latency. A crash in the plug-in ends the bridge, not the host.
//
// Any thread may call in: each round trip holds the connection's lock, so a
// parameter read from the MIDI thread waits at most for the block the audio
// thread is handing over (and the audio thread for that read).
#pragma once

#include "melodee/plugin/vst2_bridge_protocol.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <array>
#include <mutex>
#include <sys/types.h>

namespace melodee {

class Vst2BridgeInstance final : public juce::AudioPluginInstance
{
public:
    // bridgeFolder holds vst2_bridge32 and armhf/ (ld-linux-armhf.so.3 and
    // the armhf glibc, libstdc++ and libgcc_s the plug-ins link).
    static std::unique_ptr<Vst2BridgeInstance> load (const juce::File& library, const juce::File& bridgeFolder,
                                                     double rate, int block, juce::String& error);
    ~Vst2BridgeInstance() override;

    // Whether a file is a 32-bit ARM ELF shared object.
    static bool isArm32 (const juce::File&);

    const juce::String getName() const override { return name; }
    void fillInPluginDescription (juce::PluginDescription&) const override;
    void prepareToPlay (double rate, int block) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return std::max (1, (int) shared->info.numPrograms); }
    int getCurrentProgram() override;
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    bool bridgeDied() const { return connection->dead; }
    // How long processBlock waits for its block before the bridge counts as dead.
    void setProcessTimeout (int milliseconds) { processTimeoutMs = milliseconds; }
    // Times `count` empty round trips (a command the plug-in answers
    // instantly): {mean, p99, max} in microseconds. The handoff's own cost.
    std::array<double, 3> measureRoundTrips (int count);

    // The bridge process and its shared block; one command at a time.
    struct Connection
    {
        bridge::Shared* shared = nullptr;
        pid_t pid = -1;
        uint32_t sequence = 0;
        bool dead = false;
        std::recursive_mutex lock; // around a whole exchange: its fields, the round trip, the answer
        bool call (bridge::Command, int timeoutMs = 2000);
        ~Connection();
    };

private:
    class Parameter;
    friend class Parameter;

    Vst2BridgeInstance (std::unique_ptr<Connection>, const juce::File&, double rate, int block);
    static BusesProperties busesOf (const bridge::Info&);
    bool call (bridge::Command command, int timeoutMs = 2000) const { return connection->call (command, timeoutMs); }
    juce::String callText (bridge::Command, int index) const;
    float parameter (int index) const;

    std::unique_ptr<Connection> connection;
    bridge::Shared* shared;
    juce::File file;
    juce::String name, vendor;
    bool active = false;
    int processTimeoutMs = 200;
    double rate = 48000.0;
    int block = 128;
};

} // namespace melodee
