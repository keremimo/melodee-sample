// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A CLAP plug-in (a .clap shared library exporting clap_entry) wrapped as a
// JUCE AudioPluginInstance, so the rack treats it like a JUCE-loaded VST3:
// same prepareToPlay/processBlock/parameters/state calls. JUCE 9 hosts no
// CLAP, so this is Melodee's own host on the CLAP headers (MIT,
// build/deps/clap).
//
// Threads: CLAP splits its calls between a main thread and an audio thread.
// Here the audio thread is whichever thread is in processBlock() (the engine's,
// or the rack's while it warms a plug-in up); every other call is a main-thread
// call, made under one lock so two of the rack's threads never run them at
// once. Parameter changes are not sent to the plug-in at once: they wait for
// its next block, as CLAP parameter events. getValue() reads what the plug-in
// last said, so the MIDI thread never calls into it for a value.
#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct clap_plugin;
struct clap_plugin_params;
struct clap_plugin_state;
struct clap_plugin_latency;
struct clap_plugin_tail;
struct clap_plugin_audio_ports;
struct clap_plugin_note_ports;
struct clap_event_header;
struct clap_output_events;

namespace melodee {

class ClapInstance final : public juce::AudioPluginInstance
{
public:
    struct Info
    {
        std::string id, name, vendor;
        bool instrument = false;
    };
    // The plug-ins a library holds, without making any; empty and a reason on failure.
    static std::vector<Info> list (const juce::File& library, juce::String& error);
    // Makes the plug-in whose clap_id_hash() is `id` (0: the first in the library); nullptr and a reason on failure.
    static std::unique_ptr<ClapInstance> load (const juce::File& library, int32_t id, double rate, int block,
                                               juce::String& error);
    ~ClapInstance() override;

    // The rack's thread, a few times a second: what the plug-in asked of the main thread (request_callback, a
    // parameter rescan, a new latency) done now.
    void idle();

    const juce::String getName() const override { return name; }
    void fillInPluginDescription (juce::PluginDescription&) const override;
    void prepareToPlay (double rate, int block) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return notePort >= 0; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    // CLAP has no program list (presets are files, found through preset discovery): PRESETS lists none.
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;
    juce::AudioProcessorParameter* getBypassParameter() const override;

    struct Host; // the clap_host it gives the plug-in, and what the plug-in asked of it

private:
    class Parameter;
    friend class Parameter;
    struct Port { uint32_t channels = 0; };
    struct Library;

    ClapInstance (std::unique_ptr<Host>, Library*, const clap_plugin*, const Info&, const juce::File&,
                  BusesProperties, std::vector<Port> inputs, std::vector<Port> outputs, int notePort,
                  bool clapNotes, bool midiNotes, double rate, int block);
    void readParameters();          // main thread, under `mainLock`
    void refreshValues();           // main thread, under `mainLock`: every cached value from the plug-in
    void readLatency();             // main thread, under `mainLock`
    void stopProcessing();          // main thread, the audio thread no longer running it
    static bool pushOut (const clap_output_events*, const clap_event_header*);

    std::unique_ptr<Host> host;
    Library* library;
    const clap_plugin* plugin;
    const clap_plugin_params* params = nullptr;
    const clap_plugin_state* state = nullptr;
    const clap_plugin_latency* latency = nullptr;
    const clap_plugin_tail* tail = nullptr;
    juce::File file;
    Info info;
    juce::String name;
    std::recursive_mutex mainLock;
    bool active = false, processing = false;
    double rate;
    int block;
    int64_t steadyTime = 0;

    std::vector<Port> inputPorts, outputPorts;
    int notePort = -1;              // the note port notes go to, -1 none
    bool clapNotes = false;         // notes as CLAP note events (else MIDI)
    bool midiNotes = false;         // the port takes MIDI (CCs, bend, pressure)
    std::vector<float> audio;       // every port's channels, `block` frames each
    std::vector<float*> inputPointers, outputPointers;

    std::vector<Parameter*> parameters;              // owned by the AudioProcessor
    std::vector<std::pair<uint32_t, int>> byId;      // clap_id to index in `parameters`, sorted
    std::atomic<bool> anyPending { false };
    Parameter* bypass = nullptr;

    struct Event;                   // room for any event this host sends
    std::vector<Event> events;
    uint32_t eventCount = 0;
};

} // namespace melodee
