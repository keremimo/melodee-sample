// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Melodee's CLAP host (plugin/clap_instance.h) against tests/clap_fixture.cpp:
// listing a library, notes as CLAP notes and CCs as MIDI, parameters both ways,
// state, latency, the main-thread callback, presets found (with their folders,
// kept in a cache) and loaded, and every call on the right thread.
// Built and run by tests/test_plugin_hosts.py on aarch64 Linux, with Melodee's objects.
#include "melodee/plugin/clap_instance.h"
#include "melodee/plugin/plugin_files.h"

#include <cmath>
#include <cstdio>
#include <dlfcn.h>

using namespace melodee;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

namespace {
constexpr int block = 128;
constexpr double rate = 48000;

// One block through the plug-in: `in` on both channels (an effect's input), `midi` at frame 0.
juce::AudioBuffer<float> run(juce::AudioPluginInstance& p, std::initializer_list<juce::MidiMessage> midi = {}, float in = 0) {
    juce::AudioBuffer<float> buffer(2, block);
    for (int c = 0; c < 2; ++c) for (int f = 0; f < block; ++f) buffer.setSample(c, f, in);
    juce::MidiBuffer events;
    for (auto& m : midi) events.addEvent(m, 0);
    p.processBlock(buffer, events);
    return buffer;
}
float peak(const juce::AudioBuffer<float>& b) { return b.getMagnitude(0, 0, b.getNumSamples()); }
juce::AudioProcessorParameter* param(juce::AudioPluginInstance& p, const char* name) {
    for (auto* x : p.getParameters()) if (x->getName(64) == name) return x;
    return nullptr;
}
void write(const juce::File& f, const char* text) {
    f.getParentDirectory().createDirectory();
    f.replaceWithText(text, false, false, "\n"); // JUCE writes \r\n by default
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: clap_host Test.clap\n"); return 2; }
    juce::File file {juce::String(argv[1])};
    // Held open here too, so its counts can be read once the host has let it go.
    void* fixture = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    auto counts = reinterpret_cast<void (*)(int*, int*, int*)>(dlsym(fixture, "fixture_counts"));
    CHECK(fixture && counts);
    int wrong = 0, callbacks = 0, inits = 0;

    // The picker's view of it.
    auto f = describe_plugin(argv[1]);
    CHECK(f.format == PluginFormat::Clap && f.loadable() && f.library == argv[1] && std::string(format_tag(f.format)) == "CLAP");

    juce::String error;
    auto list = ClapInstance::list(file, error);
    CHECK(list.size() == 2);
    CHECK(list.size() == 2 && list[0].name == "Test Sine" && list[0].instrument && list[1].name == "Test Gain" && !list[1].instrument);
    counts(&wrong, &callbacks, &inits);
    CHECK(inits == 0); // listed, then let go

    {
        auto sine = ClapInstance::load(file, 0, rate, block, error); // 0: the first
        CHECK(sine != nullptr);
        if (!sine) return 1;
        juce::PluginDescription d;
        sine->fillInPluginDescription(d);
        CHECK(d.isInstrument && d.name == "Test Sine" && d.pluginFormatName == "CLAP" && d.uniqueId == clap_id_hash("org.melodee.test.sine"));
        CHECK(sine->acceptsMidi() && sine->getTotalNumInputChannels() == 0 && sine->getTotalNumOutputChannels() == 2);
        CHECK(sine->getParameters().size() == 2);
        sine->prepareToPlay(rate, block);
        auto* level = param(*sine, "Level");
        auto* wave = param(*sine, "Wave");
        CHECK(level && wave && std::abs(level->getValue() - 0.5f) < 1e-6f && level->getText(0.5f, 32) == "50 %");
        CHECK(wave && wave->isDiscrete() && wave->getNumSteps() == 3);

        CHECK(peak(run(*sine)) == 0.0f);
        run(*sine, {juce::MidiMessage::noteOn(1, 69, 1.0f)});
        CHECK(std::abs(peak(run(*sine)) - 0.5f) < 0.01f); // A at Level 0.5

        level->setValue(1.0f); // the next block hears it
        CHECK(level->getValue() == 1.0f);
        CHECK(std::abs(peak(run(*sine)) - 1.0f) < 0.01f);

        run(*sine, {juce::MidiMessage::controllerEvent(1, 1, 64)}); // MIDI: the plug-in sets Level and says so
        CHECK(std::abs(level->getValue() - 64.0f / 127.0f) < 1e-4f);

        juce::MemoryBlock saved;
        sine->getStateInformation(saved);
        level->setValue(0.1f);
        run(*sine);
        sine->setStateInformation(saved.getData(), int(saved.getSize()));
        CHECK(std::abs(level->getValue() - 64.0f / 127.0f) < 1e-4f); // read back from the plug-in

        run(*sine, {juce::MidiMessage::noteOff(1, 69)});
        CHECK(peak(run(*sine)) == 0.0f);
        run(*sine, {juce::MidiMessage::noteOn(1, 60, 1.0f), juce::MidiMessage::noteOn(1, 64, 1.0f)});
        run(*sine, {juce::MidiMessage::allNotesOff(1)}); // as a choke of every note
        CHECK(peak(run(*sine)) == 0.0f);

        sine->idle(); // its request_callback from init()
        counts(&wrong, &callbacks, &inits);
        CHECK(callbacks == 1 && inits == 1);
        sine->releaseResources();
    }

    {
        auto gain = ClapInstance::load(file, clap_id_hash("org.melodee.test.gain"), rate, block, error);
        CHECK(gain != nullptr);
        if (!gain) return 1;
        CHECK(gain->getName() == "Test Gain" && !gain->acceptsMidi() && gain->getTotalNumInputChannels() == 2);
        gain->prepareToPlay(rate, block);
        CHECK(gain->getLatencySamples() == 32);
        CHECK(std::abs(peak(run(*gain, {}, 0.25f)) - 0.25f) < 1e-6f); // Gain 1
        param(*gain, "Gain")->setValue(0.25f);                          // 0.5 of 0..2
        CHECK(std::abs(peak(run(*gain, {}, 0.25f)) - 0.125f) < 1e-6f);
    }
    { // Presets: files in folders beside the library, as Surge XT keeps them.
        auto reads = reinterpret_cast<int (*)()>(dlsym(fixture, "fixture_preset_reads"));
        CHECK(reads != nullptr);
        if (!reads) return 1;
        auto dir = file.getParentDirectory(), presets = dir.getChildFile("TestPresets");
        write(presets.getChildFile("Bank.tsp"), "Bank Low\t0.25\nBank High\t0.75\n"); // a file of two, by load key
        write(presets.getChildFile("Leads/Bright.tsp"), "Bright\t0.9\n");
        write(presets.getChildFile("Leads/Soft.tsp"), "Soft\t0.3\t-\n");       // for no plug-in named: anyone's
        write(presets.getChildFile("Leads/Loud.tsp"), "Loud\t1.5\tgain\n");    // Test Gain's
        write(presets.getChildFile("Broken.tsp"), "nothing here\n");             // the provider says no
        write(presets.getChildFile("._Bright.tsp"), "Junk\t1\n");               // a Mac's beside-file: never read
        write(presets.getChildFile("Notes.txt"), "Not\t1\n");                   // not its file type
        auto cache = dir.getChildFile("cache/sine.txt");

        auto sine = ClapInstance::load(file, 0, rate, block, error);
        CHECK(sine != nullptr);
        if (!sine) return 1;
        sine->prepareToPlay(rate, block);
        CHECK(sine->getNumPrograms() == 1 && sine->getProgramName(0).isEmpty()); // none until found
        int before = reads();
        auto found = sine->findPresets(cache);
        CHECK(found.size() == 4);
        if (found.size() != 4) {
            for (const auto& p : found) std::fprintf(stderr, "found \"%s\" in \"%s\" key \"%s\"\n", p.name.c_str(), p.folder.c_str(), p.key.c_str());
            return 1;
        }
        CHECK(found[0].name == "Bank High" && found[0].folder.empty() && found[0].key == "1");
        CHECK(found[1].name == "Bank Low" && found[1].key == "0");
        CHECK(found[2].name == "Bright" && found[2].folder == "Leads" && found[2].key.empty());
        CHECK(found[3].name == "Soft" && found[3].folder == "Leads");
        CHECK(reads() - before == 5 && cache.existsAsFile()); // Bank, Bright, Soft, Loud, Broken
        CHECK(sine->findPresets(cache).size() == 4 && reads() - before == 5); // the second time from the cache

        sine->setPresets(found);
        CHECK(sine->getNumPrograms() == 4 && sine->getProgramName(2) == "Bright");
        auto* level = param(*sine, "Level");
        sine->setCurrentProgram(2);
        CHECK(sine->getCurrentProgram() == 2 && level && std::abs(level->getValue() - 0.9f) < 1e-4f);
        sine->setCurrentProgram(0); // one of a file of several, by its key
        CHECK(sine->getCurrentProgram() == 0 && level && std::abs(level->getValue() - 0.75f) < 1e-4f);

        // A second location: each its own folder, named without the plug-in's name. Only the new and the changed
        // files are read.
        write(dir.getChildFile("TestUser/Mine.tsp"), "Mine\t0.5\n");
        write(presets.getChildFile("Leads/Bright.tsp"), "Brighter\t0.8\n");
        before = reads();
        auto two = sine->findPresets(cache);
        CHECK(two.size() == 5);
        if (two.size() != 5) return 1;
        CHECK(two[0].folder == "Factory" && two[1].folder == "Factory" && two[2].name == "Brighter" && two[2].folder == "Factory/Leads");
        CHECK(two[3].folder == "Factory/Leads" && two[4].name == "Mine" && two[4].folder == "User");
        CHECK(reads() - before == 2);
        sine->releaseResources();

        auto gain = ClapInstance::load(file, clap_id_hash("org.melodee.test.gain"), rate, block, error);
        CHECK(gain != nullptr);
        if (!gain) return 1;
        auto its = gain->findPresets({}); // no cache: every file read
        CHECK(its.size() == 2 && its[0].name == "Loud" && its[1].name == "Soft");
    }
    CHECK(ClapInstance::load(file, 12345, rate, block, error) == nullptr && error == "No longer in its library");

    counts(&wrong, &callbacks, &inits);
    CHECK(wrong == 0);  // every call on the thread CLAP gives it
    CHECK(inits == 0);  // every init() matched by a deinit()
    dlclose(fixture);
    if (failures) return 1;
    std::puts("clap ok");
    return 0;
}
