// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/plugin_rack.h"
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine_host.h"
#include "melodee/plugin/clap_instance.h"
#include "melodee/plugin/plugin_runner.h"
#include "melodee/plugin/plugin_support.h"
#include "melodee/plugin/rate_bridge.h"
#include "melodee/plugin/nam.h"
#include "melodee/plugin/tone3000.h"
#include "melodee/plugin/vst2_bridge_instance.h"
#include "melodee/plugin/vst2_instance.h"
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace melodee {
namespace fs = std::filesystem;

namespace {
constexpr int bridge_block_ms = 40; // a 32-bit plug-in that has not answered by then counts as crashed

// JUCE's message thread, for VST3 plug-ins: made and torn down on it.
class MessageThread {
public:
    void start() {
        std::lock_guard<std::mutex> once(starting); // two racks' threads (the live one's and a print host's) may ask at once
        if (thread.joinable()) return;
        std::mutex m;
        std::condition_variable up;
        bool ready = false;
        thread = std::thread([&] {
            auto* messages = juce::MessageManager::getInstance();
            messages->setCurrentThreadAsMessageThread();
            { std::lock_guard<std::mutex> held(m); ready = true; }
            up.notify_one();
            messages->runDispatchLoop();
        });
        std::unique_lock<std::mutex> held(m);
        up.wait(held, [&] { return ready; });
    }
    void stop() {
        std::lock_guard<std::mutex> once(starting);
        if (!thread.joinable()) return;
        juce::MessageManager::getInstance()->stopDispatchLoop();
        thread.join();
        juce::MessageManager::deleteInstance();
    }
    bool running() const { return thread.joinable(); }
    template <class F> void run(F&& f) { if (running()) juce::MessageManager::callSync(std::forward<F>(f)); else f(); }
private:
    std::thread thread;
    std::mutex starting;
};
MessageThread& message_thread() { static MessageThread t; return t; }
// Racks alive: the message thread stops with the last (a print host's rack goes while the live one plays on).
std::atomic<unsigned> racks {0};

void put(char* to, size_t room, const std::string& text) {
    std::snprintf(to, room, "%s", text.c_str());
}

// A plain number as a plug-in may write it ("75.000000", "0.010000"): without the zeros that say nothing.
std::string tidy(std::string text) {
    size_t dot = text.find('.');
    if (dot == std::string::npos || text.find_first_not_of("-+0123456789.") != std::string::npos) return text;
    size_t last = text.find_last_not_of('0');
    text.erase(last == dot ? dot : last + 1);
    return text.empty() || text == "-" ? "0" : text == "-0" ? "0" : text;
}

// Where plug-ins are copied to run from: RAM (/run on the MPC; the card is noexec).
fs::path stage_root() {
    if (const char* own = std::getenv("MELODEE_PLUGIN_STAGE")) return own;
    std::error_code error;
    if (fs::is_directory("/run", error) && access("/run", W_OK) == 0) return "/run/melodee-plugins/stage";
    return fs::temp_directory_path(error) / "melodee-plugins/stage";
}

// The folders beside a plug-in on the card, linked beside its copy: one that looks beside itself for its data finds
// it there (Surge XT: its SurgeXTData folder, patches and wavetables).
void link_neighbours(const fs::path& from, const fs::path& to) {
    std::error_code e;
    for (auto it = fs::directory_iterator(from.parent_path(), e); !e && it != fs::directory_iterator(); it.increment(e)) {
        std::error_code f;
        auto name = it->path().filename();
        if (it->path() == from || name.string().empty() || name.string()[0] == '.' || !it->is_directory(f)) continue;
        if (!fs::exists(to / name, f)) fs::create_directory_symlink(it->path(), to / name, f);
    }
}

// A plug-in's files into `to`: a VST3 bundle without other machines' builds, an Open MPC folder whole, a .so alone;
// the folders beside it linked.
bool stage_copy(const PluginFile& file, const fs::path& to, std::string& error) {
    std::error_code e;
    fs::create_directories(to, e);
    fs::path from(file.path);
    link_neighbours(from, to);
    if (!fs::is_directory(from, e)) {
        fs::copy_file(from, to / from.filename(), fs::copy_options::overwrite_existing, e);
        if (e) { error = "Could not copy: " + e.message(); return false; }
        return true;
    }
    fs::path target = to / from.filename();
    fs::create_directories(target, e);
    for (auto it = fs::recursive_directory_iterator(from, e); !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        auto rel = fs::relative(it->path(), from, e);
        auto first = rel.begin(), second = std::next(first);
        // Contents/<other machine>: not needed here.
        if (file.format == PluginFormat::Vst3 && *first == "Contents" && second != rel.end() && *second != "aarch64-linux" && *second != "Resources") {
            if (it->is_directory(e)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(e)) fs::create_directories(target / rel, e);
        else fs::copy_file(it->path(), target / rel, fs::copy_options::overwrite_existing, e);
        if (e) { error = "Could not copy: " + e.message(); return false; }
    }
    return !e;
}

// The staged copy of the plug-in's library (the same place inside `to`).
std::string staged_library(const PluginFile& file, const fs::path& to) {
    fs::path from(file.path);
    std::error_code e;
    if (!fs::is_directory(from, e)) return (to / from.filename()).string();
    return (to / from.filename() / fs::relative(file.library, from, e)).string();
}
std::string staged_path(const PluginFile& file, const fs::path& to) { return (to / fs::path(file.path).filename()).string(); }

// A VST3 or LV2 bundle or a CLAP library: listed by its file's name until it is opened, then by its plug-ins' own
// names.
bool named_inside(PluginFormat format) {
    return format == PluginFormat::Vst3 || format == PluginFormat::Lv2 || format == PluginFormat::Clap;
}

// The formats JUCE hosts: made, scanned and freed on its message thread.
bool juce_hosted(PluginFormat format) { return format == PluginFormat::Vst3 || format == PluginFormat::Lv2; }
// A host for one of them. An LV2 one is a lilv world of its own (the plug-ins it opens keep it), so two copies of the
// same bundle staged for two entries never meet in one world.
std::unique_ptr<juce::AudioPluginFormat> juce_format(PluginFormat format) {
    if (format == PluginFormat::Lv2) return std::make_unique<juce::LV2PluginFormatHeadless>();
    return std::make_unique<juce::VST3PluginFormatHeadless>();
}
// Whether a JUCE-described plug-in is an instrument. JUCE's LV2 host asks for lv2:GeneratorPlugin up the plug-in's
// class tree, which it cannot walk without lv2core's own description (every LV2 comes back as an effect, of category
// "Plugin"), so for LV2 one with audio out and no audio in counts as an instrument.
bool described_instrument(const juce::PluginDescription& d) {
    if (d.isInstrument) return true;
    return d.pluginFormatName == "LV2" && d.numInputChannels == 0 && d.numOutputChannels > 0;
}

// The plug-ins a staged bundle holds, by a host made for it on the message thread. An LV2 one makes its plug-ins
// only from the world that read their bundle, so the same host makes the one picked (then is freed there too).
std::unique_ptr<juce::AudioPluginFormat> juce_types(PluginFormat format, const std::string& path,
                                                   juce::OwnedArray<juce::PluginDescription>& types) {
    std::unique_ptr<juce::AudioPluginFormat> host;
    message_thread().start();
    message_thread().run([&] {
        host = juce_format(format);
        host->findAllTypesForFile(types, juce::String(path));
    });
    return host;
}
void free_juce_host(std::unique_ptr<juce::AudioPluginFormat>& host) { message_thread().run([&] { host.reset(); }); }

bool hidden(const juce::AudioProcessorParameter& p) {
    auto name = p.getName(64);
    return !p.isAutomatable() || name.startsWith("MIDI Ch.") || name.startsWith("MIDI CC");
}

// The programs PRESETS lists: those with a name. Plug-ins that keep a fixed
// number of slots name the unused ones "(empty)" (TONE3000), or leave them blank.
PluginProgramList named_programs(juce::AudioPluginInstance& instance) {
    std::vector<PluginProgram> out;
    for (int i = 0; i < instance.getNumPrograms(); ++i) {
        auto name = instance.getProgramName(i).trim();
        if (name.isEmpty() || name.equalsIgnoreCase("(empty)")) continue;
        out.push_back({i, name.toStdString(), {}});
    }
    return PluginProgramList(std::move(out));
}

// Where Melodee keeps what a CLAP plug-in's preset files said (plugin/clap_instance.h), in the plug-ins' home on the
// card: read again only where a file changed. None without a card.
juce::File clap_preset_cache(const std::string& home, const std::string& id) {
    if (home.empty()) return {};
    std::string name = id;
    for (auto& c : name) if (!std::isalnum((unsigned char) c) && c != '.' && c != '-' && c != '_') c = '_';
    return juce::File(juce::String((fs::path(home) / ".cache" / "melodee" / ("clap-presets-" + name + ".txt")).string()));
}

// Where plug-ins keep presets and settings: Melodee/Plugin Data on the card,
// as their home folder. They lay it out as on a Linux computer: most under
// .config (TONE3000: .config/TONE3000/Presets), which a Mac's Finder hides.
fs::path plugin_home(const std::string& card_top) {
    if (const char* own = std::getenv("MELODEE_PLUGIN_HOME")) return own;
    if (card_top.empty()) return {};
    return fs::path(card_top) / "Melodee" / "Plugin Data";
}

// A TONE3000 state as JUCE's VST3 host keeps it (VST3PluginState: the plug-in's
// component and controller states in Base64), every NAM block lite
// (plugin/tone3000.h).
void tone3000_lite_state(std::vector<uint8_t>& state) {
    auto xml = juce::AudioProcessor::getXmlFromBinary(state.data(), int(state.size()));
    if (!xml) return;
    bool changed = false;
    for (auto* part : xml->getChildIterator()) {
        juce::MemoryBlock data;
        if (!data.fromBase64Encoding(part->getAllSubText())) continue;
        if (!tone3000_lite(static_cast<uint8_t*>(data.getData()), data.getSize())) continue;
        part->deleteAllTextElements();
        part->addTextElement(data.toBase64Encoding());
        changed = true;
    }
    if (!changed) return;
    juce::MemoryBlock out;
    juce::AudioProcessor::copyXmlToBinary(*xml, out);
    state.assign(static_cast<const uint8_t*>(out.getData()), static_cast<const uint8_t*>(out.getData()) + out.getSize());
}
} // namespace

// What the audio thread runs (PluginRunner): the plug-in, its buffers made up
// front, and MIDI gathered between renders. Made on the rack's thread. A
// plug-in at a rate of its own (an Open MPC one at 44.1 kHz) is heard at the
// device's through a PluginRateBridge.
class PluginHandle final : public PluginRunner {
public:
    PluginHandle(std::unique_ptr<juce::AudioPluginInstance> plugin, bool is_instrument, double rate, double plugin_rate)
        : instance(std::move(plugin)), instrument(is_instrument), sample_rate(rate), plugin_rate(plugin_rate > 0 ? plugin_rate : rate) {
        channels = std::max({2, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels()});
        memory.assign(size_t(channels) * EngineHost::block, 0.0f);
        bridge.reset(unsigned(std::lround(plugin_rate)), unsigned(std::lround(rate)), is_instrument);
        midi_in.ensureSize(8192);
        auto* bypass = instance->getBypassParameter(); // the slot's own Bypass does that job
        for (auto& p : instance->getParameters()) if (!hidden(*p) && p != bypass) shown.push_back(p->getParameterIndex());
    }
    ~PluginHandle() override { if (instance) instance->releaseResources(); }

    void midi(uint8_t status, uint8_t data1, uint8_t data2) override {
        if (pending_count < pending.size()) pending[pending_count++] = {status, data1, data2};
    }
    void silence() override {
        for (uint8_t ch = 0; ch < 16; ++ch) {
            midi(uint8_t(0xb0 | ch), 120, 0); // all sound off
            midi(uint8_t(0xb0 | ch), 123, 0); // all notes off
        }
    }
    // Roadmap P4: what the plug-in reports (at its own rate, so scaled to the device's) and the rate bridge's.
    unsigned latency() const override {
        int own = std::max(instance->getLatencySamples(), 0);
        return unsigned(std::lround(own * (sample_rate / plugin_rate))) + bridge.latency();
    }
    // Not on the audio thread: blocks of silence through it before it is offered (PluginRack::load), so its code and
    // buffers are paged in, and whatever it does on its first calls is done, here rather than in the audio thread's
    // first blocks with it.
    void warm_up(unsigned blocks = 8) {
        std::array<float, EngineHost::block> l {}, r {};
        for (unsigned b = 0; b < blocks; ++b) {
            l.fill(0.0f);
            r.fill(0.0f);
            render(l.data(), r.data(), EngineHost::block);
        }
        load.store(0.0f, std::memory_order_relaxed); // what it costs to play counts from here
    }
    void render(float* left, float* right, unsigned frames) override {
        frames = std::min(frames, EngineHost::block);
        if (!frames) return;
        auto t0 = std::chrono::steady_clock::now();
        if (bridge.converting()) bridge.render(left, right, frames, [&](float* l, float* r, unsigned n) { run(l, r, n, false); });
        else run(left, right, frames, instrument);
        double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        float share = float(took / (frames / sample_rate));
        load.store(load.load(std::memory_order_relaxed) * 0.95f + share * 0.05f, std::memory_order_relaxed);
    }
    // One processBlock of `frames`: an effect's input in left/right; the output added there (`add`) or in their place.
    void run(float* left, float* right, unsigned frames, bool add) {
        int ins = instance->getTotalNumInputChannels(), outs = instance->getTotalNumOutputChannels();
        std::array<float*, 64> pointers {};
        for (int c = 0; c < channels && c < 64; ++c) pointers[size_t(c)] = memory.data() + size_t(c) * EngineHost::block;
        juce::AudioBuffer<float> buffer(pointers.data(), std::min(channels, 64), int(frames));
        buffer.clear();
        if (!instrument) {
            if (ins == 1) for (unsigned f = 0; f < frames; ++f) pointers[0][f] = 0.5f * (left[f] + right[f]);
            else if (ins >= 2) { std::copy(left, left + frames, pointers[0]); std::copy(right, right + frames, pointers[1]); }
        }
        midi_in.clear();
        for (unsigned n = 0; n < pending_count; ++n) {
            const auto& m = pending[n];
            uint8_t bytes[3] {m.status, m.data1, m.data2};
            int size = (m.status & 0xf0) == 0xc0 || (m.status & 0xf0) == 0xd0 ? 2 : 3;
            midi_in.addEvent(bytes, size, 0);
        }
        pending_count = 0;
        instance->processBlock(buffer, midi_in);
        if (outs <= 0) {
            if (!add) { std::fill(left, left + frames, 0.0f); std::fill(right, right + frames, 0.0f); }
            return;
        }
        const float* l = pointers[0];
        const float* r = outs >= 2 ? pointers[1] : pointers[0];
        auto safe = [](float x) { return std::isfinite(x) ? std::clamp(x, -8.0f, 8.0f) : 0.0f; };
        if (add) for (unsigned f = 0; f < frames; ++f) { left[f] += safe(l[f]); right[f] += safe(r[f]); }
        else for (unsigned f = 0; f < frames; ++f) { left[f] = safe(l[f]); right[f] = safe(r[f]); }
    }

    std::unique_ptr<juce::AudioPluginInstance> instance;
    const bool instrument;
    const double sample_rate;
    const double plugin_rate;            // what the plug-in itself runs at (an Open MPC one: 44.1 kHz)
    std::vector<int> shown;              // parameter indices the pages show, in order
    bool qlinks = false;
    bool nam = false;                    // Neural Amp Modeler: shorter card names (plugin/nam.h)
    std::atomic<float> load {0};         // share of a block's time
    std::string stage;                   // the RAM copy it runs from, removed with it
    PluginFormat format = PluginFormat::Vst3;
    PluginProgramList programs;          // its named programs, as it listed them on opening (a CLAP one's: once found)
    int program = -1;                    // the one picked last here (under the rack's lock)
    bool switching = false;              // a pick queued for the rack's thread (under the rack's lock)
    bool finding = false;                // a CLAP plug-in's presets still being looked for (under the rack's lock)

private:
    struct Midi { uint8_t status = 0, data1 = 0, data2 = 0; };
    PluginRateBridge bridge;
    std::array<Midi, 512> pending {};
    unsigned pending_count = 0;
    int channels = 2;
    std::vector<float> memory;
    juce::MidiBuffer midi_in;
};

PluginRack::PluginRack(EngineHost& engine, std::string stage) : host(engine), stage_tag(std::move(stage)) {
    racks.fetch_add(1);
    thread = std::thread([this] { run(); });
}

PluginRack::~PluginRack() {
    {
        std::lock_guard<std::mutex> held(lock);
        stopping = true;
    }
    wake.notify_all();
    if (thread.joinable()) thread.join();
    // The audio has stopped: whatever the host held is ours to free.
    std::vector<std::unique_ptr<PluginHandle>> all;
    {
        std::lock_guard<std::mutex> held(lock);
        all.swap(handles);
    }
    for (auto& h : all) {
        std::string stage = h->stage;
        if (juce_hosted(h->format)) message_thread().run([&] { h.reset(); });
        else h.reset();
        std::error_code e;
        if (!stage.empty()) fs::remove_all(stage, e);
    }
    if (racks.fetch_sub(1) == 1) message_thread().stop();
    give_back_memory();
}

void PluginRack::push(Job job) {
    {
        std::lock_guard<std::mutex> held(lock);
        jobs.push_back(std::move(job));
    }
    wake.notify_one();
}

bool PluginRack::referenced(const Project& project, unsigned entry) const { return plugin_used_by(project, entry); }

bool PluginRack::asleep(unsigned entry) const {
    std::lock_guard<std::mutex> held(lock);
    return entry < max_plugins && entries[entry].asleep;
}

void PluginRack::sync(const Project& project, uint32_t asleep) {
    double rate = dsp::sample_rate();
    std::vector<Job> wanted;
    bool moved = false;
    {
        std::lock_guard<std::mutex> held(lock);
        for (unsigned e = 0; e < max_plugins; ++e) {
            auto& entry = entries[e];
            std::string want = project.plugins[e].used() && referenced(project, e) ? project.plugins[e].path : "";
            int32_t id = want.empty() ? 0 : project.plugins[e].id;
            // Freeze plan F2d: used only by deep-frozen tracks, it sleeps (plugin/plugin_sleep.h).
            bool only_asleep = !want.empty() && asleep && !plugin_used_awake(project, e, asleep);
            auto step = plan_plugin(entry, want, id, only_asleep, rate, entry.live != nullptr);
            if (step == PluginStep::Keep) continue;
            moved = true;
            entry.error.clear();
            switch (step) {
            case PluginStep::Keep: case PluginStep::Changed: break;
            case PluginStep::Sleep: wanted.push_back({Job::Kind::Sleep, e, entry.generation, {}, 0}); break;
            case PluginStep::Unload: wanted.push_back({Job::Kind::Unload, e, entry.generation, {}, 0}); break;
            case PluginStep::Rerate: { // keep what it is now through the reload
                juce::MemoryBlock block;
                entry.live->instance->getStateInformation(block);
                entry.state.assign(static_cast<const uint8_t*>(block.getData()), static_cast<const uint8_t*>(block.getData()) + block.getSize());
                [[fallthrough]];
            }
            case PluginStep::Load: wanted.push_back({Job::Kind::Load, e, entry.generation, want, rate, id}); break;
            }
        }
    }
    if (!moved) return;
    changed();
    for (auto& job : wanted) push(std::move(job));
}

void PluginRack::set_states(PluginStates states) {
    std::lock_guard<std::mutex> held(lock);
    for (unsigned e = 0; e < max_plugins; ++e) {
        entries[e].state = std::move(states[e]);
        entries[e].reload = true; // a new project: every entry it uses loads again, with its state
    }
}

PluginStates PluginRack::states() const {
    PluginStates out;
    std::lock_guard<std::mutex> held(lock);
    for (unsigned e = 0; e < max_plugins; ++e) {
        const auto& entry = entries[e];
        if (entry.live && entry.status == PluginStatus::Ready) {
            juce::MemoryBlock block;
            entry.live->instance->getStateInformation(block);
            out[e].assign(static_cast<const uint8_t*>(block.getData()), static_cast<const uint8_t*>(block.getData()) + block.getSize());
        } else {
            out[e] = entry.state;
        }
    }
    return out;
}

void PluginRack::clone(unsigned entry, unsigned copy) {
    if (entry >= max_plugins || copy >= max_plugins) return;
    std::lock_guard<std::mutex> held(lock);
    auto& from = entries[entry];
    std::vector<uint8_t> state = from.state;
    if (from.live && from.status == PluginStatus::Ready) {
        juce::MemoryBlock block;
        from.live->instance->getStateInformation(block);
        state.assign(static_cast<const uint8_t*>(block.getData()), static_cast<const uint8_t*>(block.getData()) + block.getSize());
    }
    entries[copy].state = std::move(state);
}

std::string PluginRack::summary() const {
    static const char* const states[] {"none", "opening", "ready", "failed", "crashed"};
    std::string out;
    std::lock_guard<std::mutex> held(lock);
    for (unsigned e = 0; e < max_plugins; ++e) {
        const auto& entry = entries[e];
        if (entry.path.empty()) continue;
        auto status = entry.status;
        if (entry.asleep) { // freeze plan F2d
            std::string name = entry.file.name.empty() ? fs::path(entry.path).stem().string() : entry.file.name;
            for (auto& ch : name) if (ch == ' ' || ch == ',' || ch == ':') ch = '_';
            if (!out.empty()) out += ',';
            out += std::to_string(e) + ":asleep:" + name + ':' + format_tag(entry.file.format) + ":0";
            continue;
        }
        if (auto* h = entry.live)
            if (auto* bridged = dynamic_cast<Vst2BridgeInstance*>(h->instance.get()); bridged && bridged->bridgeDied()) status = PluginStatus::Crashed;
        std::string name = entry.file.name.empty() ? fs::path(entry.path).stem().string() : entry.file.name;
        for (auto& ch : name) if (ch == ' ' || ch == ',' || ch == ':') ch = '_';
        int load = entry.live ? int(entry.live->load.load(std::memory_order_relaxed) * 1000) : 0;
        if (!out.empty()) out += ',';
        out += std::to_string(e) + ':' + states[unsigned(status) % 5] + ':' + name + ':' + format_tag(entry.file.format) + ':' + std::to_string(load);
    }
    return out.empty() ? "none" : out;
}

PluginRack::Usage PluginRack::usage() const {
    Usage out;
    std::lock_guard<std::mutex> held(lock);
    for (const auto& entry : entries) {
        if (!entry.live || entry.status != PluginStatus::Ready) continue;
        ++out.loaded;
        out.load += entry.live->load.load(std::memory_order_relaxed);
    }
    return out;
}

void PluginRack::reload(unsigned entry) {
    if (entry >= max_plugins) return;
    std::lock_guard<std::mutex> held(lock);
    auto& e = entries[entry];
    if (e.live && e.status == PluginStatus::Ready) {
        auto* bridged = dynamic_cast<Vst2BridgeInstance*>(e.live->instance.get());
        if (!bridged || !bridged->bridgeDied()) { // a working one keeps what it is now; a dead bridge has nothing to say
            juce::MemoryBlock block;
            e.live->instance->getStateInformation(block);
            e.state.assign(static_cast<const uint8_t*>(block.getData()), static_cast<const uint8_t*>(block.getData()) + block.getSize());
        }
    }
    e.reload = true;
}

PluginStatus PluginRack::status(unsigned entry) const {
    if (entry >= max_plugins) return PluginStatus::None;
    std::lock_guard<std::mutex> held(lock);
    return entries[entry].status;
}

int PluginRack::instrument(unsigned entry) const {
    if (entry >= max_plugins) return -1;
    std::lock_guard<std::mutex> held(lock);
    const auto& e = entries[entry];
    if (e.live) return e.live->instrument ? 1 : 0;
    return e.file.instrument;
}

void PluginRack::drum_voices(unsigned entry, DrumVoices& out) const {
    out = {};
    if (entry >= max_plugins) return;
    out.entry = uint8_t(entry);
    std::lock_guard<std::mutex> held(lock);
    for (const auto& v : entries[entry].file.drums) {
        unsigned k = unsigned(v.key) - DrumVoices::first;
        if (v.key < DrumVoices::first || k >= DrumVoices::keys) continue;
        put(out.names[k].data(), out.names[k].size(), v.name);
    }
}

void PluginRack::panel(unsigned entry, unsigned row, PluginPanel& out) const {
    out = {};
    out.entry = uint8_t(entry);
    if (entry >= max_plugins) return;
    std::lock_guard<std::mutex> held(lock);
    const auto& e = entries[entry];
    out.status = e.status;
    put(out.name, sizeof(out.name), e.file.name.empty() ? fs::path(e.path).stem().string() : e.file.name);
    put(out.format, sizeof(out.format), e.path.empty() ? "" : format_tag(e.file.format));
    put(out.error, sizeof(out.error), e.error);
    out.instrument = e.file.instrument == 1;
    auto* h = e.live;
    if (!h || e.status != PluginStatus::Ready) return;
    if (auto* bridged = dynamic_cast<Vst2BridgeInstance*>(h->instance.get()); bridged && bridged->bridgeDied()) {
        out.status = PluginStatus::Crashed;
        return;
    }
    out.instrument = h->instrument;
    out.qlinks = h->qlinks;
    out.rows = uint16_t((h->shown.size() + 2) / 3);
    out.row = uint16_t(out.rows ? std::min<unsigned>(row, out.rows - 1u) : 0);
    out.load = uint16_t(std::clamp(h->load.load(std::memory_order_relaxed) * 1000.0f, 0.0f, 9999.0f));
    out.programs = uint16_t(std::min<size_t>(h->programs.size(), 0xffff));
    const auto& params = h->instance->getParameters();
    auto read = [&](size_t at, PluginPanelParam& card) {
        auto* p = params[h->shown[at]];
        card.used = true;
        std::string name = p->getName(int(sizeof(card.name) - 1)).trim().toStdString();
        put(card.name, sizeof(card.name), h->nam ? nam_card_name(name) : name);
        float v = p->getValue();
        put(card.text, sizeof(card.text), tidy(p->getText(v, 64).trim().toStdString()));
        card.value = int16_t(std::lround(std::clamp(v, 0.0f, 1.0f) * plugin_param_max));
        card.initial = int16_t(std::lround(std::clamp(p->getDefaultValue(), 0.0f, 1.0f) * plugin_param_max));
        int steps = p->getNumSteps();
        card.steps = uint16_t(steps > 1 && steps < plugin_param_max ? steps : 0);
    };
    for (unsigned k = 0; k < 3; ++k) {
        size_t at = size_t(out.row) * 3 + k;
        if (at >= h->shown.size()) break;
        read(at, out.params[k]);
    }
    out.window_first = uint16_t((out.row > 0 ? out.row - 1u : 0u) * 3u);
    for (unsigned k = 0; k < out.window.size(); ++k) {
        size_t at = size_t(out.window_first) + k;
        if (at >= h->shown.size()) break;
        read(at, out.window[k]);
    }
}

void PluginRack::set_param(unsigned entry, unsigned shown_index, int value) {
    if (entry >= max_plugins) return;
    std::lock_guard<std::mutex> held(lock);
    auto* h = entries[entry].live;
    if (!h || shown_index >= h->shown.size()) return;
    auto* p = h->instance->getParameters()[h->shown[shown_index]];
    float v = std::clamp(value, 0, plugin_param_max) / float(plugin_param_max);
    p->setValue(v);
}

PluginPrograms PluginRack::programs(unsigned entry) const {
    PluginPrograms out;
    out.entry = uint8_t(entry);
    if (entry >= max_plugins) return out;
    std::lock_guard<std::mutex> held(lock);
    const auto& e = entries[entry];
    if (!e.live || e.status != PluginStatus::Ready) return out;
    out.list = e.live->programs;
    out.current = e.live->program;
    out.loading = e.live->switching;
    out.finding = e.live->finding;
    return out;
}

void PluginRack::select_program(unsigned entry, int program) {
    if (entry >= max_plugins) return;
    Job job {Job::Kind::Program, entry, 0, {}, 0, program};
    {
        std::lock_guard<std::mutex> held(lock);
        auto& e = entries[entry];
        if (!e.live || e.status != PluginStatus::Ready) return;
        e.live->program = program;
        e.live->switching = true;
        job.generation = e.generation;
    }
    push(std::move(job));
    changed();
}

// Rack thread. A VST3 plug-in hears a program as its program parameter's
// value, and a JUCE plug-in ignores the value it already holds, so the one it
// is on (a freshly opened TONE3000 sits on 0 with nothing loaded) only loads
// after another has gone through first: the controller takes the change from
// JUCE's message thread (60 times a second) and the processor at its next
// block, hence the wait.
void PluginRack::switch_program(unsigned entry, uint32_t generation, int program) {
    juce::AudioPluginInstance* instance = nullptr;
    {
        std::lock_guard<std::mutex> held(lock);
        auto& e = entries[entry];
        if (e.generation != generation || !e.live) return; // changed since: the plug-in it was for is gone
        instance = e.live->instance.get(); // freed only on this thread, so it outlives the switch
    }
    if (program >= 0 && program < instance->getNumPrograms()) {
        // TONE3000 reads the preset's file now: one copied over it since it opened is made lite too.
        if (is_tone3000(instance->getName().toStdString())) tone3000_lite_presets(use_plugin_home());
        if (instance->getCurrentProgram() == program && instance->getNumPrograms() > 1 && !dynamic_cast<ClapInstance*>(instance)) {
            instance->setCurrentProgram(program == 0 ? 1 : 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        instance->setCurrentProgram(program);
    }
    {
        std::lock_guard<std::mutex> held(lock);
        auto& e = entries[entry];
        if (e.generation == generation && e.live && e.live->instance.get() == instance) e.live->switching = false;
    }
    changed();
}

// Rack thread, before a plug-in's library opens: $HOME on the card, made if
// missing. Set only when it moves, as other threads may
// read the environment.
std::string PluginRack::use_plugin_home() {
    std::string top;
    {
        std::lock_guard<std::mutex> held(lock);
        top = card_top;
    }
    fs::path home = plugin_home(top);
    if (home.empty()) return {};
    std::error_code e;
    fs::create_directories(home, e);
    if (!fs::is_directory(home, e)) return {};
    const char* now = std::getenv("HOME");
    if (!now || home != fs::path(now)) setenv("HOME", home.c_str(), 1);
    return home.string();
}

void PluginRack::set_card(const std::string& top) {
    std::string wanted_root = top.empty() ? std::string() : (fs::path(top) / "Plugins").string();
    {
        std::lock_guard<std::mutex> held(lock);
        if (top == card_top) return;
        card_top = top;
        root = wanted_root;
        // Entries named from the card's top (Plugins/...) that could not open try again on this card at the next sync().
        for (auto& entry : entries)
            if (!entry.path.empty() && fs::path(entry.path).is_relative() && entry.status == PluginStatus::Failed) entry.reload = true;
    }
    browse("");
}

void PluginRack::browse(const std::string& where) { push({Job::Kind::Browse, 0, 0, where, 0}); }

void PluginRack::browse_up() {
    std::string up;
    {
        std::lock_guard<std::mutex> held(lock);
        if (shown.top) return;
        up = fs::path(shown.folder).parent_path().string();
    }
    Job job {Job::Kind::Browse, 1, 0, up, 0}; // entry 1: going up (the row it came out of is found)
    push(std::move(job));
}

PluginRack::View PluginRack::view() const {
    std::lock_guard<std::mutex> held(lock);
    return shown;
}

void PluginRack::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> held(lock);
            wake.wait_for(held, std::chrono::milliseconds(50), [&] { return stopping || !jobs.empty(); });
            if (stopping) return;
            if (jobs.empty()) {
                held.unlock();
                free_retired();
                idle_plugins();
                continue;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        switch (job.kind) {
        case Job::Kind::Load: load(job); break;
        case Job::Kind::Unload: unload(job.entry, job.generation); break;
        case Job::Kind::Sleep: sleep(job.entry, job.generation); break;
        case Job::Kind::Browse: {
            std::string from;
            {
                std::lock_guard<std::mutex> held(lock);
                from = shown.folder;
            }
            std::string at = job.path.empty() ? root : job.path;
            list(at, job.entry == 1 ? 0 : -1);
            if (job.entry == 1) { // the row of the folder it came out of
                std::lock_guard<std::mutex> held(lock);
                for (size_t n = 0; n < shown.files.size(); ++n)
                    if (shown.files[n].path == from) shown.came_from = int(n);
            }
            probe_listed();
            break;
        }
        case Job::Kind::Probe: probe_listed(); break;
        case Job::Kind::Program: switch_program(job.entry, job.generation, job.id); break;
        case Job::Kind::Presets: find_presets(job.entry, job.generation); break;
        }
        free_retired();
    }
}

void PluginRack::list(const std::string& at, int) {
    std::string top;
    {
        std::lock_guard<std::mutex> held(lock);
        top = root;
    }
    View v;
    v.root = top;
    if (!top.empty()) {
        std::error_code e;
        auto canonical_top = fs::weakly_canonical(top, e), canonical_at = fs::weakly_canonical(at.empty() ? top : at, e);
        bool inside = canonical_at.string().rfind(canonical_top.string(), 0) == 0;
        v.folder = inside ? canonical_at.string() : canonical_top.string();
        v.top = v.folder == canonical_top.string();
        auto here = describe_plugin(v.folder);
        if (!here.folder && here.loadable()) { // a bundle of several, opened: a row for each plug-in in it
            Known found;
            {
                std::lock_guard<std::mutex> held(lock);
                for (const auto& k : known) if (k.path == here.path) found = k;
            }
            if (found.path.empty()) { // not opened yet: once now, to see what it holds
                found = probe(here);
                std::lock_guard<std::mutex> held(lock);
                known.push_back(found);
            }
            for (const auto& cl : found.classes) {
                PluginFile f = here;
                f.name = cl.name; f.instrument = cl.instrument; f.id = cl.id; f.plugins = unsigned(found.classes.size());
                v.files.push_back(f);
            }
            std::sort(v.files.begin(), v.files.end(), [](const PluginFile& a, const PluginFile& b) {
                return juce::String(a.name).compareNatural(juce::String(b.name)) < 0;
            });
        } else {
            unzip_plugins(v.folder); // a release zip copied as it came: its plug-in folder takes its place
            v.files = list_plugins(v.folder);
        }
    }
    {
        std::lock_guard<std::mutex> held(lock);
        for (auto& f : v.files)
            if (f.id == 0)
                for (const auto& k : known)
                    if (k.path == f.path) {
                        f.instrument = k.instrument; f.plugins = unsigned(k.classes.size());
                        if (!k.name.empty() && f.plugins <= 1) f.name = k.name;
                    }
        shown = std::move(v);
    }
    changed();
}

PluginRack::Known PluginRack::probe(const PluginFile& file) {
    Known found;
    found.path = file.path;
    int8_t kind = -1;
    std::string error;
    fs::path stage = stage_root() / (stage_tag + "probe-" + std::to_string(++stage_count));
    use_plugin_home();
    if (ensure_plugin_support() && stage_copy(file, stage, error)) {
        std::string library = staged_library(file, stage);
        juce::String why;
        juce::PluginDescription d;
        bool described = false;
        if (juce_hosted(file.format)) {
            juce::OwnedArray<juce::PluginDescription> types;
            auto host = juce_types(file.format, staged_path(file, stage), types);
            free_juce_host(host);
            for (auto* t : types) found.classes.push_back({t->uniqueId, t->name.toStdString(), int8_t(described_instrument(*t) ? 1 : 0)});
            // An LV2 bundle of one whose .ttl says lv2:InstrumentPlugin (plugin_files.cpp): an instrument whatever its ports.
            if (file.format == PluginFormat::Lv2 && file.instrument == 1 && types.size() == 1) { types[0]->isInstrument = true; found.classes[0].instrument = 1; }
            if (!types.isEmpty()) { d = *types[0]; described = true; }
        } else if (file.format == PluginFormat::Clap) {
            auto list = ClapInstance::list(juce::File(library), why);
            for (const auto& c : list) found.classes.push_back({clap_id_hash(c.id), c.name, int8_t(c.instrument ? 1 : 0)});
            if (!list.empty()) { d.name = list[0].name; d.isInstrument = list[0].instrument; described = true; }
        } else if (file.format == PluginFormat::Vst2) {
            if (auto p = Vst2Instance::load(juce::File(library), 44100, 128, why)) { p->fillInPluginDescription(d); described = true; }
        } else if (auto p = Vst2BridgeInstance::load(juce::File(library), juce::File(plugin_support_folder() + "/bridge32"), 44100, 128, why)) {
            p->fillInPluginDescription(d);
            described = true;
        }
        if (described) { kind = described_instrument(d) ? 1 : 0; if (named_inside(file.format)) found.name = d.name.toStdString(); }
    }
    std::error_code e;
    fs::remove_all(stage, e);
    found.instrument = kind < 0 ? int8_t(0) : kind; // one that will not open lists as an effect
    return found;
}

// Opens each listed plug-in whose kind is not known yet just long enough to ask, one at a time, so the picker can
// say INSTRUMENT or EFFECT, or how many plug-ins a bundle holds. Stops when other work comes in (it is picked up
// again with the next listing).
void PluginRack::probe_listed() {
    for (;;) {
        PluginFile file;
        {
            std::lock_guard<std::mutex> held(lock);
            if (stopping || !jobs.empty()) return;
            auto it = std::find_if(shown.files.begin(), shown.files.end(), [&](const PluginFile& f) {
                // A VST3 bundle or a CLAP is opened once even when its kind is known: it may hold several.
                if (!f.loadable() || f.id != 0 || (f.instrument >= 0 && !named_inside(f.format))) return false;
                return std::none_of(known.begin(), known.end(), [&](const Known& k) { return k.path == f.path; });
            });
            if (it == shown.files.end()) return;
            file = *it;
        }
        Known found = probe(file);
        {
            std::lock_guard<std::mutex> held(lock);
            known.push_back(found);
            for (auto& f : shown.files)
                if (f.path == file.path && f.id == 0) {
                    f.instrument = found.instrument; f.plugins = unsigned(found.classes.size());
                    if (!found.name.empty() && f.plugins <= 1) f.name = found.name;
                }
        }
        changed();
    }
}

void PluginRack::load(const Job& job) {
    std::string path = job.path;
    {
        std::lock_guard<std::mutex> held(lock);
        // A path relative to the card's top (a track Melodee's own synth was, Plugins/...): on the card in now.
        if (fs::path(path).is_relative() && !card_top.empty()) path = (fs::path(card_top) / path).string();
    }
    auto file = describe_plugin(path);
    std::vector<uint8_t> state;
    {
        std::lock_guard<std::mutex> held(lock);
        auto& entry = entries[job.entry];
        if (entry.generation != job.generation) return; // changed again since
        entry.file = file;
        state = entry.state;
    }
    auto fail = [&](const std::string& why) {
        {
            std::lock_guard<std::mutex> held(lock);
            auto& entry = entries[job.entry];
            if (entry.generation != job.generation) return;
            entry.status = PluginStatus::Failed;
            entry.error = why;
        }
        changed();
    };
    std::error_code missing;
    if (!fs::exists(path, missing)) { fail("Not on the card"); return; }
    if (!file.loadable()) { fail(file.folder ? "Not a plug-in" : file.why); return; }
    if (!ensure_plugin_support()) { fail("Plug-in support missing"); return; }
    std::string home = use_plugin_home();
    fs::path stage = stage_root() / (stage_tag + "e" + std::to_string(job.entry) + "-" + std::to_string(++stage_count));
    std::string error;
    if (!stage_copy(file, stage, error)) { std::error_code e; fs::remove_all(stage, e); fail(error); return; }
    std::string library = staged_library(file, stage);

    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::String why;
    double device_rate = job.rate > 0 ? job.rate : 44100.0;
    double rate = file.fixed_rate ? double(file.fixed_rate) : device_rate; // what the plug-in itself runs at
    int block = int(EngineHost::block);
    bool tone3000 = false; // its NAM blocks always lite (plugin/tone3000.h)
    bool nam = false;      // Neural Amp Modeler: Slim held at lite, its folders made (plugin/nam.h)
    if (juce_hosted(file.format)) {
        juce::OwnedArray<juce::PluginDescription> types;
        auto host = juce_types(file.format, staged_path(file, stage), types);
        const juce::PluginDescription* chosen = types.isEmpty() ? nullptr : types[0];
        if (job.id != 0) { // one of a bundle's several, by its id
            chosen = nullptr;
            for (auto* t : types) if (t->uniqueId == job.id) chosen = t;
        }
        if (!chosen) why = types.isEmpty() ? "No plug-in in the bundle" : "No longer in its bundle";
        else {
            tone3000 = is_tone3000(chosen->name.toStdString());
            if (tone3000) tone3000_lite_presets(home); // before it lists them
            nam = is_nam(chosen->name.toStdString());
            if (nam) nam_folders(home); // before it lists them
            auto make = [&] { instance = host->createInstanceFromDescription(*chosen, rate, block, why); };
            if (file.format == PluginFormat::Lv2) message_thread().run(make); // where its lilv world was made
            else make();
        }
        free_juce_host(host);
    } else if (file.format == PluginFormat::Clap) {
        instance = ClapInstance::load(juce::File(library), job.id, rate, block, why);
    } else if (file.format == PluginFormat::Vst2) {
        instance = Vst2Instance::load(juce::File(library), rate, block, why);
    } else {
        auto bridged = Vst2BridgeInstance::load(juce::File(library), juce::File(plugin_support_folder() + "/bridge32"), rate, block, why);
        if (bridged) bridged->setProcessTimeout(bridge_block_ms);
        instance = std::move(bridged);
    }
    if (!instance) { std::error_code e; fs::remove_all(stage, e); fail(why.isEmpty() ? "Could not open it" : why.toStdString()); return; }

    instance->enableAllBuses();
    // Only the main buses: a side chain or extra outputs stay off.
    for (bool input : {true, false})
        for (int b = 1; b < instance->getBusCount(input); ++b) instance->getBus(input, b)->enable(false);
    instance->setNonRealtime(false);
    instance->prepareToPlay(rate, block);
    if (!state.empty()) {
        if (tone3000) tone3000_lite_state(state); // a project saved before, or with a full block
        instance->setStateInformation(state.data(), int(state.size()));
    }
    if (nam) // a capture at lite whatever the state said; the plug-in takes it in warm_up(), before the audio thread
        for (auto* p : instance->getParameters())
            if (p->getName(64) == nam_slim_name() && p->getValue() != 0.0f) p->setValue(0.0f);
    juce::PluginDescription description;
    instance->fillInPluginDescription(description);
    bool is_instrument = described_instrument(description) || (instance->getTotalNumInputChannels() == 0 && instance->getTotalNumOutputChannels() > 0);

    auto handle = std::make_unique<PluginHandle>(std::move(instance), is_instrument, device_rate, rate);
    handle->stage = stage.string();
    handle->format = file.format;
    handle->programs = named_programs(*handle->instance);
    handle->finding = file.format == PluginFormat::Clap; // looked for once it plays: find_presets()
    if (file.format == PluginFormat::Vst2 || file.format == PluginFormat::Vst2Arm32) { // an Open MPC skin's Q-Link pages first, then the rest
        auto order = qlink_order(file.path);
        if (!order.empty()) {
            std::vector<int> ordered;
            for (int i : order) if (std::find(handle->shown.begin(), handle->shown.end(), i) != handle->shown.end()) ordered.push_back(i);
            for (int i : handle->shown) if (std::find(ordered.begin(), ordered.end(), i) == ordered.end()) ordered.push_back(i);
            handle->shown = std::move(ordered);
            handle->qlinks = true;
        }
    }
    if (nam) {
        std::vector<std::string> names;
        for (int i : handle->shown) names.push_back(handle->instance->getParameters()[i]->getName(64).toStdString());
        std::vector<int> ordered;
        for (size_t at : nam_page_order(names)) ordered.push_back(handle->shown[at]);
        handle->shown = std::move(ordered);
        handle->nam = true;
    }
    handle->warm_up(); // before the audio thread first runs it

    PluginHandle* raw = handle.get();
    bool stale = false;
    {
        std::lock_guard<std::mutex> held(lock);
        auto& entry = entries[job.entry];
        if (entry.generation != job.generation) stale = true;
        else {
            handles.push_back(std::move(handle));
            if (entry.live) retiring.push_back(entry.live);
            entry.live = raw;
            entry.status = PluginStatus::Ready;
            entry.file.instrument = is_instrument ? 1 : 0;
            if (description.name.isNotEmpty() && named_inside(file.format)) entry.file.name = description.name.toStdString();
        }
    }
    if (stale) { destroy(handle.release()); return; }
    if (!offer(job.entry, raw)) {
        std::lock_guard<std::mutex> held(lock);
        unoffered.emplace_back(job.entry, raw);
    }
    changed();
    if (file.format == PluginFormat::Clap) push({Job::Kind::Presets, job.entry, job.generation, {}, 0, 0});
}

// Rack thread: a CLAP plug-in's presets, looked for once it plays (the first time, Surge XT's on the card take a
// while: thousands of files), then PRESETS' programs.
void PluginRack::find_presets(unsigned entry, uint32_t generation) {
    ClapInstance* clap = nullptr;
    {
        std::lock_guard<std::mutex> held(lock);
        auto& e = entries[entry];
        if (e.generation != generation || !e.live) return; // changed since: the plug-in it was for is gone
        clap = dynamic_cast<ClapInstance*>(e.live->instance.get()); // freed only on this thread, so it outlives this
    }
    std::vector<PluginProgram> list;
    if (clap) {
        auto found = clap->findPresets(clap_preset_cache(use_plugin_home(), clap->clapId()));
        list.reserve(found.size());
        for (size_t n = 0; n < found.size(); ++n) list.push_back({int(n), found[n].name, found[n].folder});
        clap->setPresets(std::move(found));
    }
    {
        std::lock_guard<std::mutex> held(lock);
        auto& e = entries[entry];
        if (e.generation == generation && e.live && e.live->instance.get() == clap) {
            e.live->programs = PluginProgramList(std::move(list));
            e.live->finding = false;
        }
    }
    changed();
}

void PluginRack::unload(unsigned entry, uint32_t generation) {
    PluginHandle* old = nullptr;
    {
        std::lock_guard<std::mutex> held(lock);
        auto& e = entries[entry];
        if (e.generation != generation) return;
        old = e.live;
        e.live = nullptr;
        e.file = {};
        if (old) retiring.push_back(old);
    }
    if (old && !offer(entry, nullptr)) {
        std::lock_guard<std::mutex> held(lock);
        unoffered.emplace_back(entry, nullptr);
    }
    changed();
}

// Freeze plan F2d: the plug-in's state as it is now kept for when it wakes (one still opening keeps the state it was
// to open with), then let go. The audio thread hands it back and free_retired() frees it here, never on the MIDI or
// audio thread.
void PluginRack::sleep(unsigned entry, uint32_t generation) {
    PluginHandle* old = nullptr;
    {
        std::lock_guard<std::mutex> held(lock);
        auto& e = entries[entry];
        if (e.generation != generation || !e.asleep) return;
        old = e.live;
        e.live = nullptr;
        if (old) {
            juce::MemoryBlock block;
            old->instance->getStateInformation(block);
            e.state.assign(static_cast<const uint8_t*>(block.getData()), static_cast<const uint8_t*>(block.getData()) + block.getSize());
            retiring.push_back(old);
        }
    }
    if (old && !offer(entry, nullptr)) {
        std::lock_guard<std::mutex> held(lock);
        unoffered.emplace_back(entry, nullptr);
    }
    changed();
}

bool PluginRack::offer(unsigned entry, PluginHandle* handle) { return host.offer_plugin(entry, handle); }

void PluginRack::free_retired() {
    std::vector<std::pair<unsigned, PluginHandle*>> again;
    {
        std::lock_guard<std::mutex> held(lock);
        again.swap(unoffered);
    }
    for (auto& [entry, handle] : again)
        if (!offer(entry, handle)) {
            std::lock_guard<std::mutex> held(lock);
            unoffered.emplace_back(entry, handle);
        }
    PluginRunner* runner = nullptr;
    while (host.take_retired_plugin(runner)) {
        auto* handle = static_cast<PluginHandle*>(runner);
        {
            std::lock_guard<std::mutex> held(lock);
            auto it = std::find(retiring.begin(), retiring.end(), handle);
            if (it == retiring.end()) continue; // not one of ours to free (should not happen)
            retiring.erase(it);
        }
        destroy(handle);
    }
}

void PluginRack::destroy(PluginHandle* handle) {
    std::unique_ptr<PluginHandle> owned;
    {
        std::lock_guard<std::mutex> held(lock);
        auto it = std::find_if(handles.begin(), handles.end(), [&](const auto& h) { return h.get() == handle; });
        if (it != handles.end()) { owned = std::move(*it); handles.erase(it); }
        else owned.reset(handle); // never made it into the list (a stale load)
    }
    std::string stage = owned->stage;
    if (juce_hosted(owned->format)) message_thread().run([&] { owned.reset(); });
    else owned.reset();
    std::error_code e;
    if (!stage.empty()) fs::remove_all(stage, e);
    give_back_memory();
}

// What CLAP plug-ins asked of their main thread (plugin/clap_instance.h), done on this one. Handles are freed only on
// this thread, so they outlive the calls.
void PluginRack::idle_plugins() {
    std::vector<ClapInstance*> clap;
    {
        std::lock_guard<std::mutex> held(lock);
        for (const auto& h : handles)
            if (h->format == PluginFormat::Clap)
                if (auto* c = dynamic_cast<ClapInstance*>(h->instance.get())) clap.push_back(c);
    }
    for (auto* c : clap) c->idle();
}

// Freeze plan F2d: what a plug-in freed goes back to the system (glibc keeps freed heap for the process otherwise,
// so a deep freeze would save no RAM). The rack's thread: the audio thread never allocates, so it never waits here.
void start_plugin_messages() { message_thread().start(); }

void give_back_memory() {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

} // namespace melodee
