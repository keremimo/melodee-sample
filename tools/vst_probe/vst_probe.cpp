// SPDX-License-Identifier: AGPL-3.0-only
// vst_probe: try VST3 and VST2 plug-ins on the MPC Sample with JUCE's
// headless host, before Melodee grows VST instrument tracks and VST effect
// slots. VST3 goes through JUCE's VST3 format; VST2 (.so, e.g. Open MPC's
// plug-ins) through Vst2Instance, the same AudioPluginInstance interface.
//
//   vst_probe scan DIR_OR_PLUGIN
//       Every .vst3 bundle and VST2 .so under DIR: which builds a bundle
//       carries, whether the library loads (dlerror names a missing
//       library), and the plug-ins inside (instrument or effect, buses).
//   vst_probe run BUNDLE_OR_SO [--index N] [--rate HZ] [--block FRAMES]
//                        [--seconds S] [--instances N] [--wav OUT.wav]
//       Loads the plug-in, plays chords into an instrument or noise bursts
//       and a tone into an effect, times every processBlock against the
//       block's real-time budget, round-trips its state and writes what it
//       played to a WAV file.
//
// It opens no audio device, so it runs beside Melodee without taking ALSA.
// The main thread is JUCE's message thread (VST3 creation and the Linux
// IRunLoop live there); the work runs on a second thread, as audio would.

#include "vst2_bridge_instance.h"
#include "vst2_instance.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <fstream>
#include <random>
#include <sys/utsname.h>
#include <thread>
#include <vector>

using namespace juce;

namespace
{
long residentKiB()
{
    std::ifstream status ("/proc/self/status");
    std::string line;
    while (std::getline (status, line))
        if (line.rfind ("VmRSS:", 0) == 0)
            return std::atol (line.c_str() + 6);
    return -1;
}

// The folder JUCE's VST3 host loads from: uname's machine plus "-linux".
String machineFolder()
{
    utsname name {};
    uname (&name);
    return String (name.machine) + "-linux";
}

// The builds a bundle carries: Contents/<arch>-linux, x86_64-win, MacOS...
StringArray buildsIn (const File& bundle)
{
    StringArray builds;
    for (const auto& child : bundle.getChildFile ("Contents").findChildFiles (File::findDirectories, false))
        if (child.getFileName() != "Resources")
            builds.add (child.getFileName());
    return builds;
}

// dlopen the bundle's library ourselves first, so a missing dependency is
// printed by name instead of JUCE's silent "no plug-ins found".
bool libraryLoads (const File& bundle)
{
    const auto folder = bundle.getChildFile ("Contents").getChildFile (machineFolder());
    const auto libraries = folder.findChildFiles (File::findFiles, false, "*.so");
    if (libraries.isEmpty())
    {
        std::printf ("  no %s library\n", machineFolder().toRawUTF8());
        return false;
    }
    for (const auto& library : libraries)
    {
        const auto t0 = std::chrono::steady_clock::now();
        void* handle = dlopen (library.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
        const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        if (handle == nullptr)
        {
            std::printf ("  dlopen failed: %s\n", dlerror());
            return false;
        }
        std::printf ("  %s loads (%.0f ms)\n", library.getFileName().toRawUTF8(), ms);
        dlclose (handle);
    }
    return true;
}

OwnedArray<PluginDescription> typesIn (VST3PluginFormatHeadless& format, const File& bundle)
{
    OwnedArray<PluginDescription> types;
    MessageManager::callSync ([&] { format.findAllTypesForFile (types, bundle.getFullPathName()); });
    return types;
}

Array<File> bundlesUnder (const File& root)
{
    if (root.hasFileExtension ("vst3"))
        return { root };
    if (! root.isDirectory())
        return {};
    return root.findChildFiles (File::findDirectories, true, "*.vst3");
}

// VST2 libraries: .so files outside .vst3 bundles.
Array<File> vst2Under (const File& root)
{
    if (root.hasFileExtension ("so"))
        return { root };
    Array<File> libraries;
    if (root.isDirectory())
        for (const auto& file : root.findChildFiles (File::findFiles, true, "*.so"))
            if (! file.getFullPathName().contains (".vst3/"))
                libraries.add (file);
    return libraries;
}

// The 32-bit bridge: bridge32/ beside the probe, holding vst2_bridge32 and
// the armhf runtime in armhf/.
File bridgeFolder()
{
    return File::getSpecialLocation (File::currentExecutableFile).getSiblingFile ("bridge32");
}

std::unique_ptr<AudioPluginInstance> loadVst2 (const File& library, double rate, int block, String& error)
{
    if (Vst2BridgeInstance::isArm32 (library))
        return Vst2BridgeInstance::load (library, bridgeFolder(), rate, block, error);
    return Vst2Instance::load (library, rate, block, error);
}

void scanVst2 (const File& library)
{
    std::printf ("%s (VST2)\n", library.getFullPathName().toRawUTF8());
    String error;
    if (! Vst2BridgeInstance::isArm32 (library) && ! Vst2Instance::exportsEntryPoint (library, error))
    {
        std::printf ("  not loadable: %s\n", error.toRawUTF8());
        return;
    }
    auto plugin = loadVst2 (library, 48000.0, 128, error);
    if (plugin == nullptr)
    {
        std::printf ("  failed: %s\n", error.toRawUTF8());
        return;
    }
    PluginDescription t;
    plugin->fillInPluginDescription (t);
    std::printf ("  [0] %s by %s: %s, in %d out %d, %d parameters\n", t.name.toRawUTF8(),
                 t.manufacturerName.toRawUTF8(), t.isInstrument ? "instrument" : "effect",
                 t.numInputChannels, t.numOutputChannels, plugin->getParameters().size());
}

int scan (const File& root)
{
    VST3PluginFormatHeadless format;
    const auto bundles = bundlesUnder (root);
    const auto libraries = vst2Under (root);
    if (bundles.isEmpty() && libraries.isEmpty())
        std::printf ("no .vst3 bundles or VST2 .so files under %s\n", root.getFullPathName().toRawUTF8());
    for (const auto& library : libraries)
        scanVst2 (library);

    for (const auto& bundle : bundles)
    {
        std::printf ("%s\n  builds: %s\n", bundle.getFileName().toRawUTF8(),
                     buildsIn (bundle).joinIntoString (", ").toRawUTF8());
        if (! libraryLoads (bundle))
            continue;
        const auto types = typesIn (format, bundle);
        if (types.isEmpty())
            std::printf ("  JUCE found no plug-ins in it\n");
        for (int i = 0; i < types.size(); ++i)
        {
            const auto& t = *types[i];
            std::printf ("  [%d] %s by %s: %s, %s, in %d out %d\n", i, t.name.toRawUTF8(),
                         t.manufacturerName.toRawUTF8(), t.isInstrument ? "instrument" : "effect",
                         t.category.toRawUTF8(), t.numInputChannels, t.numOutputChannels);
        }
    }
    return 0;
}

struct RunOptions
{
    File bundle;
    int index = 0;
    double rate = 48000.0;
    int block = 128;
    double seconds = 10.0;
    int instances = 1;
    File wav;
};

// 32-bit float stereo WAV, so no decoder module is needed.
void writeWav (const File& file, const std::vector<float>& interleaved, int channels, double rate)
{
    FileOutputStream out (file);
    if (! out.openedOk())
        return;
    out.setPosition (0);
    out.truncate();
    const auto dataBytes = (uint32) (interleaved.size() * sizeof (float));
    out.write ("RIFF", 4); out.writeInt ((int) (36 + dataBytes)); out.write ("WAVE", 4);
    out.write ("fmt ", 4); out.writeInt (16); out.writeShort (3); out.writeShort ((short) channels);
    out.writeInt ((int) rate); out.writeInt ((int) rate * channels * 4);
    out.writeShort ((short) (channels * 4)); out.writeShort (32);
    out.write ("data", 4); out.writeInt ((int) dataBytes);
    out.write (interleaved.data(), dataBytes);
}

void describe (AudioPluginInstance& plugin)
{
    std::printf ("buses: in %d ch, out %d ch, latency %d samples, tail %.2f s\n",
                 plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels(),
                 plugin.getLatencySamples(), plugin.getTailLengthSeconds());
    const auto& params = plugin.getParameters();
    std::printf ("parameters: %d\n", params.size());
    for (int i = 0; i < std::min (params.size(), 24); ++i)
    {
        auto* p = params[i];
        std::printf ("  %3d %-28s %s %s%s\n", i, p->getName (28).toRawUTF8(),
                     p->getCurrentValueAsText().toRawUTF8(), p->getLabel().toRawUTF8(),
                     p->isAutomatable() ? "" : " (not automatable)");
    }
    if (params.size() > 24)
        std::printf ("  ...\n");
    std::printf ("programs: %d\n", plugin.getNumPrograms());
}

void checkState (AudioPluginInstance& plugin)
{
    MemoryBlock state;
    const auto t0 = std::chrono::steady_clock::now();
    plugin.getStateInformation (state);
    const auto t1 = std::chrono::steady_clock::now();
    plugin.setStateInformation (state.getData(), (int) state.getSize());
    const auto t2 = std::chrono::steady_clock::now();
    MemoryBlock again;
    plugin.getStateInformation (again);
    std::printf ("state: %zu bytes, save %.1f ms, load %.1f ms, round trip %s\n", state.getSize(),
                 std::chrono::duration<double, std::milli> (t1 - t0).count(),
                 std::chrono::duration<double, std::milli> (t2 - t1).count(),
                 state == again ? "identical" : "differs (may be fine: some plug-ins stamp versions)");
}

int run (const RunOptions& options)
{
    VST3PluginFormatHeadless format;
    const bool vst2 = options.bundle.hasFileExtension ("so");
    PluginDescription description;
    if (! vst2)
    {
        if (! libraryLoads (options.bundle))
            return 1;
        const auto types = typesIn (format, options.bundle);
        if (! isPositiveAndBelow (options.index, types.size()))
        {
            std::printf ("no plug-in %d in %s (%d found)\n", options.index,
                         options.bundle.getFileName().toRawUTF8(), types.size());
            return 1;
        }
        description = *types[options.index];
    }

    std::vector<std::unique_ptr<AudioPluginInstance>> plugins;
    const auto rssBefore = residentKiB();
    for (int i = 0; i < options.instances; ++i)
    {
        String error;
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<AudioPluginInstance> plugin;
        if (vst2)
            plugin = loadVst2 (options.bundle, options.rate, options.block, error);
        else
            plugin = format.createInstanceFromDescription (description, options.rate, options.block, error);
        if (plugin != nullptr && i == 0)
        {
            if (vst2)
                plugin->fillInPluginDescription (description);
            std::printf ("%s by %s, %s %s\n", description.name.toRawUTF8(), description.manufacturerName.toRawUTF8(),
                         description.pluginFormatName.toRawUTF8(), description.isInstrument ? "instrument" : "effect");
        }
        const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        if (plugin == nullptr)
        {
            std::printf ("instance %d failed: %s\n", i, error.toRawUTF8());
            return 1;
        }
        std::printf ("instance %d created in %.0f ms\n", i, ms);
        plugins.push_back (std::move (plugin));
    }

    auto& first = *plugins.front();
    describe (first);
    for (auto& plugin : plugins)
    {
        plugin->setNonRealtime (false);
        plugin->prepareToPlay (options.rate, options.block);
    }
    std::printf ("memory: %ld KiB more resident for %d instance(s)\n", residentKiB() - rssBefore, options.instances);

    const int channels = std::max ({ 2, first.getTotalNumInputChannels(), first.getTotalNumOutputChannels() });
    const int outChannels = std::max (1, first.getTotalNumOutputChannels());
    AudioBuffer<float> buffer (channels, options.block);
    MidiBuffer midi;
    midi.ensureSize (4096);
    std::vector<AudioBuffer<float>> extraBuffers (plugins.size());
    std::vector<MidiBuffer> extraMidi (plugins.size());
    for (size_t p = 1; p < plugins.size(); ++p)
    {
        extraBuffers[p].setSize (channels, options.block);
        extraMidi[p].ensureSize (4096);
    }
    const auto blocks = (int) std::ceil (options.seconds * options.rate / options.block);
    const auto beat = (int64) options.rate;  // one chord or burst a second
    const int chords[4][4] = { { 48, 52, 55, 60 }, { 45, 48, 52, 57 }, { 41, 45, 48, 53 }, { 43, 47, 50, 55 } };
    std::mt19937 noise (1);
    std::uniform_real_distribution<float> uniform (-1.0f, 1.0f);
    std::vector<double> times;
    times.reserve ((size_t) blocks);
    std::vector<float> recorded;
    recorded.reserve ((size_t) blocks * (size_t) options.block * 2);
    bool badSamples = false;
    float peak = 0.0f;
    double sumSquares = 0.0;

    for (int b = 0; b < blocks; ++b)
    {
        const int64 start = (int64) b * options.block;
        buffer.clear();
        midi.clear();
        if (description.isInstrument)
        {
            for (int64 s = start; s < start + options.block; ++s)
            {
                const auto offset = (int) (s - start);
                const auto& chord = chords[(s / beat) % 4];
                if (s % beat == 0)
                    for (int note : chord)
                        midi.addEvent (MidiMessage::noteOn (1, note, (uint8) 100), offset);
                if (s % beat == beat * 3 / 4)
                    for (int note : chord)
                        midi.addEvent (MidiMessage::noteOff (1, note), offset);
            }
        }
        else
        {
            for (int i = 0; i < options.block; ++i)
            {
                const int64 s = start + i;
                const auto sinceBurst = (double) (s % beat) / options.rate;
                const auto burst = sinceBurst < 0.05 ? uniform (noise) * 0.5f : 0.0f;
                const auto tone = (s / beat) % 2 == 0
                    ? 0.2f * (float) std::sin (MathConstants<double>::twoPi * 220.0 * (double) s / options.rate)
                    : 0.0f;
                for (int c = 0; c < channels; ++c)
                    buffer.setSample (c, i, burst + tone);
            }
        }

        // Every instance gets the same input; the first one's output is kept.
        // The copies are made before the clock starts.
        for (size_t p = 1; p < plugins.size(); ++p)
        {
            for (int c = 0; c < channels; ++c)
                extraBuffers[p].copyFrom (c, 0, buffer, c, 0, options.block);
            extraMidi[p].clear();
            extraMidi[p].addEvents (midi, 0, options.block, 0);
        }
        const auto t0 = std::chrono::steady_clock::now();
        plugins.front()->processBlock (buffer, midi);
        for (size_t p = 1; p < plugins.size(); ++p)
            plugins[p]->processBlock (extraBuffers[p], extraMidi[p]);
        times.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());

        for (int i = 0; i < options.block; ++i)
            for (int c = 0; c < 2; ++c)
            {
                const auto v = buffer.getSample (std::min (c, outChannels - 1), i);
                if (! std::isfinite (v))
                    badSamples = true;
                peak = std::max (peak, std::abs (v));
                sumSquares += (double) v * v;
                recorded.push_back (v);
            }
    }

    for (auto& plugin : plugins)
        plugin->releaseResources();

    const auto budget = 1.0e6 * options.block / options.rate;
    std::vector<double> sorted (times.begin() + std::min<size_t> (8, times.size()), times.end());
    std::sort (sorted.begin(), sorted.end());
    double total = 0.0;
    for (auto t : sorted)
        total += t;
    const auto mean = sorted.empty() ? 0.0 : total / (double) sorted.size();
    const auto pct = [&] (double q) { return sorted.empty() ? 0.0 : sorted[(size_t) (q * (double) (sorted.size() - 1))]; };
    int overruns = 0;
    for (auto t : times)
        overruns += t > budget ? 1 : 0;
    std::printf ("cpu (%d instance(s), one core, block %d at %.0f Hz = %.0f us budget):\n",
                 options.instances, options.block, options.rate, budget);
    std::printf ("  first block %.0f us; then mean %.0f us (%.1f%%), p99 %.0f us (%.1f%%), max %.0f us (%.1f%%)\n",
                 times.empty() ? 0.0 : times.front(), mean, 100.0 * mean / budget, pct (0.99),
                 100.0 * pct (0.99) / budget, pct (1.0), 100.0 * pct (1.0) / budget);
    std::printf ("  blocks over budget: %d of %d\n", overruns, (int) times.size());
    const auto rms = std::sqrt (sumSquares / std::max<size_t> (1, recorded.size()));
    std::printf ("output: peak %.3f, rms %.4f%s\n", peak, rms, badSamples ? ", NaN/Inf present" : "");

    checkState (first);
    if (auto* bridged = dynamic_cast<Vst2BridgeInstance*> (&first))
    {
        const auto trip = bridged->measureRoundTrips (2000);
        std::printf ("bridge round trip (empty command): mean %.1f us, p99 %.1f us, max %.1f us%s\n",
                     trip[0], trip[1], trip[2], bridged->bridgeDied() ? "; the bridge died" : "");
    }
    if (auto* v2 = dynamic_cast<Vst2Instance*> (&first); v2 != nullptr && v2->unansweredHostOpcodes().isNotEmpty())
        std::printf ("host opcodes asked but not answered: %s\n", v2->unansweredHostOpcodes().toRawUTF8());

    if (options.wav != File())
    {
        writeWav (options.wav, recorded, 2, options.rate);
        std::printf ("wrote %s\n", options.wav.getFullPathName().toRawUTF8());
    }

    // VST3 components are torn down on the message thread.
    MessageManager::callSync ([&] { plugins.clear(); });
    return 0;
}

// Load the no-op GUI libraries (make_stubs.py) from stubs/ beside the
// binary. Once a library with a soname is loaded, a plug-in that lists that
// soname in NEEDED binds to it, so no LD_LIBRARY_PATH is needed.
void loadStubs()
{
    const auto folder = File::getSpecialLocation (File::currentExecutableFile).getSiblingFile ("stubs");
    int loaded = 0;
    for (const auto& stub : folder.findChildFiles (File::findFiles, false, "lib*.so.*"))
    {
        if (dlopen (stub.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_GLOBAL) != nullptr)
            ++loaded;
        else
            std::printf ("stub %s: %s\n", stub.getFileName().toRawUTF8(), dlerror());
    }
    if (loaded > 0)
        std::printf ("%d GUI library stubs loaded from %s\n", loaded, folder.getFullPathName().toRawUTF8());
}

int usage()
{
    std::printf ("usage: vst_probe scan DIR_OR_BUNDLE\n"
                 "       vst_probe run BUNDLE [--index N] [--rate HZ] [--block FRAMES]\n"
                 "                            [--seconds S] [--instances N] [--wav OUT.wav]\n");
    return 2;
}

int dispatch (const StringArray& args)
{
    if (args.size() >= 2 && args[0] == "scan")
        return scan (File::getCurrentWorkingDirectory().getChildFile (args[1]));
    if (args.size() < 2 || args[0] != "run")
        return usage();

    RunOptions options;
    options.bundle = File::getCurrentWorkingDirectory().getChildFile (args[1]);
    for (int i = 2; i + 1 < args.size(); i += 2)
    {
        const auto& key = args[i];
        const auto& value = args[i + 1];
        if (key == "--index") options.index = value.getIntValue();
        else if (key == "--rate") options.rate = value.getDoubleValue();
        else if (key == "--block") options.block = value.getIntValue();
        else if (key == "--seconds") options.seconds = value.getDoubleValue();
        else if (key == "--instances") options.instances = std::max (1, value.getIntValue());
        else if (key == "--wav") options.wav = File::getCurrentWorkingDirectory().getChildFile (value);
        else return usage();
    }
    return run (options);
}
} // namespace

int main (int argc, char** argv)
{
    StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);
    loadStubs();

    auto* messages = MessageManager::getInstance();
    messages->setCurrentThreadAsMessageThread();
    int result = 0;
    std::thread worker ([&]
    {
        result = dispatch (args);
        messages->stopDispatchLoop();
    });
    messages->runDispatchLoop();
    worker.join();
    MessageManager::deleteInstance();
    std::fflush (stdout);
    return result;
}
