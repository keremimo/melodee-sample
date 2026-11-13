// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// JUCE's LV2 host as Melodee uses it (plugin/plugin_rack.cpp) against
// tests/lv2_fixture: the picker's view of the bundle, the plug-in found in it
// with its category, MIDI notes in, its Level port as a parameter, its presets
// as programs, and its state. Built and run by tests/test_plugin_hosts.py.
#include "melodee/plugin/plugin_files.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <cmath>
#include <cstdio>

using namespace melodee;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

namespace {
float run(juce::AudioPluginInstance& p, std::initializer_list<juce::MidiMessage> midi = {}) {
    juce::AudioBuffer<float> buffer(2, 128);
    buffer.clear();
    juce::MidiBuffer events;
    for (auto& m : midi) events.addEvent(m, 0);
    p.processBlock(buffer, events);
    return buffer.getMagnitude(0, 0, 128);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: lv2_host Tone.lv2\n"); return 2; }
    juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread();
    const std::string bundle = argv[1];

    auto f = describe_plugin(bundle);
    CHECK(f.format == PluginFormat::Lv2 && f.loadable() && std::string(format_tag(f.format)) == "LV2");
    CHECK(f.instrument == 1); // lv2:InstrumentPlugin in its .ttl
    CHECK(f.name == "Tone");  // the bundle's name until it is opened

    juce::LV2PluginFormatHeadless format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, juce::String(bundle));
    CHECK(types.size() == 1);
    if (types.isEmpty()) return 1;
    const auto& d = *types[0];
    CHECK(d.name == "Test Tone" && d.fileOrIdentifier == "urn:melodee:test:tone");
    // The rack's instrument test for LV2 (described_instrument): JUCE's own flag, else audio out and none in.
    CHECK(d.isInstrument || (d.numInputChannels == 0 && d.numOutputChannels > 0));

    juce::String why;
    auto p = format.createInstanceFromDescription(d, 48000, 128, why);
    CHECK(p != nullptr);
    if (!p) { std::fprintf(stderr, "%s\n", why.toRawUTF8()); return 1; }
    p->enableAllBuses();
    p->prepareToPlay(48000, 128);
    CHECK(p->acceptsMidi() && p->getTotalNumInputChannels() == 0 && p->getTotalNumOutputChannels() == 2);

    juce::AudioProcessorParameter* level = nullptr;
    for (auto* x : p->getParameters()) if (x->getName(32) == "Level") level = x;
    CHECK(level != nullptr && std::abs(level->getValue() - 0.5f) < 1e-3f);
    if (!level) return 1;

    CHECK(run(*p) == 0.0f);
    run(*p, {juce::MidiMessage::noteOn(1, 69, 1.0f)});
    CHECK(std::abs(run(*p) - 0.5f) < 0.02f);
    level->setValue(1.0f);
    run(*p);
    CHECK(std::abs(run(*p) - 1.0f) < 0.02f);

    // Presets as programs: PRESETS lists them by name, and picking one sets its ports.
    CHECK(p->getNumPrograms() == 2);
    bool quiet = false, loud = false;
    for (int i = 0; i < p->getNumPrograms(); ++i) {
        quiet |= p->getProgramName(i) == "Quiet";
        loud |= p->getProgramName(i) == "Loud";
    }
    CHECK(quiet && loud);
    for (int i = 0; i < p->getNumPrograms(); ++i)
        if (p->getProgramName(i) == "Quiet") p->setCurrentProgram(i);
    run(*p);
    CHECK(std::abs(level->getValue() - 0.25f) < 1e-3f);
    CHECK(std::abs(run(*p) - 0.25f) < 0.02f);

    // State: saved at Quiet, changed, then loaded back.
    juce::MemoryBlock state;
    p->getStateInformation(state);
    level->setValue(0.75f);
    run(*p);
    p->setStateInformation(state.getData(), int(state.getSize()));
    run(*p);
    CHECK(std::abs(level->getValue() - 0.25f) < 1e-3f);

    run(*p, {juce::MidiMessage::noteOff(1, 69)});
    CHECK(run(*p) == 0.0f);
    p->releaseResources();
    p.reset();
    if (failures) return 1;
    std::puts("lv2 ok");
    return 0;
}
