// SPDX-License-Identifier: AGPL-3.0-only
// A VST2 plug-in (.so exporting VSTPluginMain) wrapped as a JUCE
// AudioPluginInstance, so a host treats it like a JUCE-loaded VST3: same
// prepareToPlay/processBlock/parameters/state calls. JUCE's own VST2 host
// needs Steinberg's withdrawn SDK headers; this uses vst2_abi.h instead.
#pragma once

#include "vst2_abi.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <array>
#include <atomic>
#include <vector>

class Vst2Instance final : public juce::AudioPluginInstance
{
public:
    // dlopens the library and opens its effect; nullptr and a reason on failure.
    static std::unique_ptr<Vst2Instance> load (const juce::File& library, double rate, int block,
                                               juce::String& error);
    // Whether a .so exports a VST2 entry point, without opening an effect.
    static bool exportsEntryPoint (const juce::File& library, juce::String& error);
    ~Vst2Instance() override;

    const juce::String getName() const override { return name; }
    void fillInPluginDescription (juce::PluginDescription&) const override;
    void prepareToPlay (double rate, int block) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return midiIn; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return std::max (1, effect->numPrograms); }
    int getCurrentProgram() override;
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Opcodes the plug-in sent that this host does not answer, for the probe.
    juce::String unansweredHostOpcodes() const;

private:
    class Parameter;
    friend class Parameter;

    Vst2Instance (void* library, vst2::Effect* effect, const juce::File& file, double rate, int block);
    static BusesProperties busesOf (const vst2::Effect&);
    static intptr_t hostCallback (vst2::Effect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
    intptr_t dispatch (int32_t opcode, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0.0f) const;
    juce::String dispatchString (int32_t opcode, int32_t index = 0) const;

    void* library;
    vst2::Effect* effect;
    juce::File file;
    juce::String name, vendor;
    bool midiIn = false, active = false;
    double rate;
    int block;
    vst2::TimeInfo timeInfo {};
    std::vector<float> inputs, outputs;
    std::vector<float*> inputPointers, outputPointers;
    std::vector<vst2::MidiEvent> midiEvents;
    vst2::Events<512> events {};
    std::array<std::atomic<bool>, 64> unanswered {};
};
