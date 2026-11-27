// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/plugin_rack.h"
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine_host.h"
#include "melodee/plugin/clap_instance.h"
#include "melodee/plugin/companion_host.h"
#include "melodee/plugin/companion_package.h"
#include "melodee/plugin/companion_screen.h"
#include "melodee/plugin/idle_gate.h"
#include "melodee/plugin/page_file.h"
#include "melodee/plugin/page_library.h"
#include "melodee/plugin/plugin_skin.h"
#include "melodee/plugin/plugin_pages.h"
#include "melodee/plugin/plugin_runner.h"
#include "melodee/plugin/plugin_support.h"
#include "melodee/plugin/rate_bridge.h"
#include "melodee/plugin/nam.h"
#include "melodee/plugin/vst2_bridge_instance.h"
#include "melodee/plugin/vst2_instance.h"
#include "melodee/plugin/xpl_preset.h"
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <melodee_screen.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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

// What a plug-in may load or run, so copied off the card (mounted noexec): a library (x.so, libx.so.1), or an ARM
// program without an extension.
bool runs(const fs::path& path) {
    auto name = path.filename().string();
    for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0) return true;
    if (name.find(".so.") != std::string::npos) return true;
    return !path.has_extension() && arm_elf(path.string()) != 0;
}

// The folders in an MPC plug-in folder holding something that runs(), each relative to it.
std::vector<fs::path> folders_that_run(const fs::path& from) {
    std::vector<fs::path> out;
    std::error_code e;
    for (auto it = fs::recursive_directory_iterator(from, e); !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        std::error_code f;
        if (it->is_directory(f) || !runs(it->path())) continue;
        for (auto dir = it->path().lexically_relative(from).parent_path(); !dir.empty(); dir = dir.parent_path())
            if (std::find(out.begin(), out.end(), dir) == out.end()) out.push_back(dir);
    }
    return out;
}

// A plug-in's files into `to`: a VST3 bundle without other machines' builds, an MPC plug-in folder (its folders with
// no library linked, so its data stays on the card: OxZygen's samples are 160 MB), a .so alone; the folders beside it
// linked.
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
    bool mpc_folder = file.format == PluginFormat::Vst2 || file.format == PluginFormat::Vst2Arm32;
    std::vector<fs::path> copied = mpc_folder ? folders_that_run(from) : std::vector<fs::path> {};
    for (auto it = fs::recursive_directory_iterator(from, e); !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        auto rel = fs::relative(it->path(), from, e);
        auto first = rel.begin(), second = std::next(first);
        // Contents/<other machine>: not needed here.
        if (file.format == PluginFormat::Vst3 && *first == "Contents" && second != rel.end() && *second != "aarch64-linux" && *second != "Resources") {
            if (it->is_directory(e)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(e)) {
            if (mpc_folder && std::find(copied.begin(), copied.end(), rel) == copied.end()) {
                fs::create_directory_symlink(it->path(), target / rel, e);
                it.disable_recursion_pending();
            } else {
                fs::create_directories(target / rel, e);
            }
        } else {
            fs::copy_file(it->path(), target / rel, fs::copy_options::overwrite_existing, e);
        }
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

// Where a plug-in's own page file is (docs/PLUGIN_PAGES.md): in its VST3 bundle's Resources, its LV2 bundle or MPC
// plug-in folder, else beside its .clap or .so as <name>.melodee-pages.json.
fs::path package_page_file(const PluginFile& file) {
    fs::path at(file.path);
    std::error_code e;
    if (file.format == PluginFormat::Vst3) return at / "Contents" / "Resources" / "melodee-pages.json";
    if (fs::is_directory(at, e)) return at / "melodee-pages.json";
    return at.parent_path() / (at.stem().string() + ".melodee-pages.json");
}

std::string text_of(const fs::path& file, size_t most) {
    std::error_code e;
    if (!fs::is_regular_file(file, e) || fs::file_size(file, e) > most) return {};
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

// Who a plug-in is, as page files and companion screens name it (plugin/page_file.h).
PluginIdentity identity_of(juce::AudioPluginInstance& instance, ClapInstance* clap, const PluginFile& file) {
    juce::PluginDescription description;
    instance.fillInPluginDescription(description);
    PluginIdentity who;
    who.name = description.name.toStdString();
    who.vendor = description.manufacturerName.toStdString();
    if (clap) who.clap = clap->clapId();
    if (file.format == PluginFormat::Vst2 || file.format == PluginFormat::Vst2Arm32) who.vst2 = description.uniqueId;
    if (file.format == PluginFormat::Lv2) who.lv2 = description.fileOrIdentifier.toStdString();
    return who;
}

// The pages of the first page file for this plug-in (plugin/page_file.h): the card's Melodee/Plugin Pages folder, its
// own package, Melodee's library (plugin/page_library.h). Empty: none. Its skin, where it has one, into `skin`
// (plugin/plugin_skin.h; images from the file's folder, so none from the library). `problems` gets, by file, what
// kept a file out or what it names that the plug-in lacks (the library's files say nothing). Rack's thread.
std::vector<PluginPage> file_pages(juce::AudioPluginInstance& instance, ClapInstance* clap, const PluginFile& file,
                                   const std::string& card_top, const std::vector<int>& shown,
                                   std::map<std::string, std::vector<std::string>>& problems,
                                   std::shared_ptr<const PluginSkin>& skin) {
    skin = nullptr;
    const PluginIdentity who = identity_of(instance, clap, file);
    std::vector<std::string> names, ids;
    for (auto* p : instance.getParameters()) {
        names.push_back(p->getName(64).trim().toStdString());
        auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(p);
        ids.push_back(hosted ? hosted->getParameterID().toStdString() : std::to_string(p->getParameterIndex()));
    }
    auto use = [&](const std::string& text, const std::string& source, bool tell, const fs::path& folder) -> std::vector<PluginPage> {
        PageFile page_file;
        std::string why;
        if (!read_page_file(text, page_file, why)) { if (tell) problems[source] = {why}; return {}; }
        if (!page_file_matches(page_file, who)) { if (tell) problems.erase(source); return {}; }
        std::vector<std::string> missing;
        auto pages = resolve_page_file(page_file, shown, names, ids, missing);
        if (!pages.empty()) skin = load_skin(page_file.skin, folder, shown, names, ids, missing);
        if (tell) { if (missing.empty()) problems.erase(source); else problems[source] = missing; }
        return pages;
    };
    if (!card_top.empty()) {
        std::error_code e;
        std::vector<fs::path> found;
        for (fs::directory_iterator it(fs::path(card_top) / "Melodee" / "Plugin Pages", e), end; !e && it != end; it.increment(e))
            if (it->path().extension() == ".json" && it->path().filename().string().rfind("._", 0) != 0) found.push_back(it->path());
        std::sort(found.begin(), found.end());
        for (const auto& path : found)
            if (auto pages = use(text_of(path, 64 << 10), path.filename().string(), true, path.parent_path()); !pages.empty()) return pages;
    }
    if (auto own = package_page_file(file); !text_of(own, 64 << 10).empty())
        if (auto pages = use(text_of(own, 64 << 10), own.string(), true, own.parent_path()); !pages.empty()) return pages;
    for (const char* json : page_library)
        if (auto pages = use(json, "", false, {}); !pages.empty()) return pages;
    return {};
}

// A plug-in's pages (plugin/plugin_pages.h, docs/PLUGIN_PAGES_PLAN.md): a page file's (`from_file`), else its own
// pages (CLAP remote-controls, else an Open MPC skin's Q-Link pages); then its parameter groups (VST3 units, LV2 port
// groups, CLAP modules) for the parameters those leave out (pages_with_groups()); with none, its list on one page.
// Rack's thread: a CLAP plug-in is asked for its pages now.
std::vector<PluginPage> plugin_pages(juce::AudioPluginInstance& instance, ClapInstance* clap, PluginFormat format,
                                     const std::string& folder, const std::vector<int>& shown,
                                     const std::vector<PluginPage>& from_file) {
    std::vector<PluginPage> own = from_file;
    if (own.empty() && clap)
        for (auto& r : clap->remotePages()) own.push_back({r.name.empty() ? r.section : r.name, std::move(r.params)});
    if (own.empty() && (format == PluginFormat::Vst2 || format == PluginFormat::Vst2Arm32)) own = qlink_pages(folder);
    keep_only(own, shown);
    std::vector<std::string> groups(shown.size());
    if (clap) {
        for (size_t i = 0; i < shown.size(); ++i) groups[i] = clap->module(shown[i]);
    } else {
        std::vector<std::pair<int, std::string>> found; // parameter index, its group's path
        std::function<void(const juce::AudioProcessorParameterGroup&, const std::string&)> walk =
            [&](const juce::AudioProcessorParameterGroup& group, const std::string& path) {
                for (const auto* node : group) {
                    if (auto* p = node->getParameter()) found.push_back({p->getParameterIndex(), path});
                    else if (auto* g = node->getGroup()) {
                        std::string name = g->getName().trim().toStdString();
                        walk(*g, name.empty() ? path : path.empty() ? name : path + "/" + name);
                    }
                }
            };
        walk(instance.getParameterTree(), "");
        for (size_t i = 0; i < shown.size(); ++i)
            for (const auto& [index, path] : found)
                if (index == shown[i]) { groups[i] = path; break; }
    }
    return pages_with_groups(std::move(own), shown, groups);
}

// The pages laid out in rows of three, with each page's picture found from the parameters' names (plugin_pages.h).
PluginLayout layout_of(juce::AudioPluginInstance& instance, const std::vector<PluginPage>& pages) {
    auto layout = lay_out(pages);
    std::vector<std::string> names;
    for (auto* p : instance.getParameters()) names.push_back(p->getName(64).trim().toStdString());
    find_pictures(layout, names);
    return layout;
}

// The programs PRESETS lists: those with a name. Plug-ins that keep a fixed
// number of slots name the unused ones "(empty)" (TONE3000), or leave them blank.
// An MPC plug-in folder's own presets (Presets/, its .xpl files: plugin/xpl_preset.h) as PRESETS rows, numbered
// from xpl_first, so never one of the plug-in's own program numbers.
constexpr int xpl_first = 1 << 20;
constexpr size_t xpl_most = 8u << 20; // the largest .xpl read
bool has_mpc_presets(const PluginFile& file) {
    std::error_code e;
    return (file.format == PluginFormat::Vst2 || file.format == PluginFormat::Vst2Arm32)
           && fs::is_directory(fs::path(file.path) / "Presets", e);
}
// Its .xpl files (folders below Presets/ kept as preset folders, in name order) after the plug-in's own programs
// `own`: at the top when it lists none of its own, else in a Presets folder. One made for another plug-in (`id`, its
// unique id; 0 not known: any) is left out, and so is one named as a program it lists already (same_preset_name()).
// `files`: each row's file, in the same order.
std::vector<PluginProgram> mpc_presets(const std::string& folder, const PluginProgramList& own, int32_t id,
                                       std::vector<std::string>& files) {
    fs::path root = fs::path(folder) / "Presets";
    std::vector<fs::path> found;
    std::error_code e;
    for (auto it = fs::recursive_directory_iterator(root, e); !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        std::error_code f;
        auto name = it->path().filename().string();
        bool dir = it->is_directory(f);
        if (name.empty() || name[0] == '.') { if (dir) it.disable_recursion_pending(); continue; }
        auto ext = it->path().extension().string();
        for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (!dir && ext == ".xpl") found.push_back(it->path().lexically_relative(root));
        if (found.size() >= 4096) break;
    }
    std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
        return juce::String(a.generic_string()).compareNatural(juce::String(b.generic_string())) < 0;
    });
    std::vector<PluginProgram> out;
    for (const auto& rel : found) {
        auto preset = xpl_preset_in(text_of(root / rel, xpl_most), false);
        if (id != 0 && !preset.for_plugin(id)) continue;
        std::string name = preset.name.empty() ? rel.stem().string() : preset.name;
        if (std::any_of(own.begin(), own.end(), [&](const PluginProgram& p) { return same_preset_name(p.name, name); })) continue;
        std::string where = own.empty() ? "" : "Presets", below = rel.parent_path().generic_string();
        if (!below.empty()) where = where.empty() ? below : where + "/" + below;
        out.push_back({xpl_first + int(files.size()), name, where});
        files.push_back((root / rel).string());
    }
    return out;
}

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

// Whether `inside` is `folder` or in it, part by part (so /media/card2 is not in /media/card).
bool under(const fs::path& folder, const fs::path& inside) {
    if (folder.empty()) return false;
    auto rel = inside.lexically_normal().lexically_relative(folder.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

} // namespace

// Melodee's transport as a plug-in hears it (plugin/host_time.h): set before each of its processBlocks.
class PluginClock final : public juce::AudioPlayHead {
public:
    juce::Optional<PositionInfo> getPosition() const override { return position; }
    PositionInfo position;
};

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
        instance->setPlayHead(&clock);
        idle.reset(is_instrument, this->plugin_rate, instance->getTailLengthSeconds());
        clap = dynamic_cast<ClapInstance*>(instance.get());
        screen_source = waker = dynamic_cast<ScreenSource*>(instance.get());
        has_screen = screen_source && screen_source->hasScreen();
    }
    // A companion screen in place of a screen of its own (rack's thread, before it is offered).
    void use_companion(std::unique_ptr<CompanionScreen> screen) {
        companion = std::move(screen);
        screen_source = companion.get();
        has_screen = true;
    }
    ~PluginHandle() override {
        if (!instance) return;
        instance->releaseResources();
        instance->setPlayHead(nullptr); // the clock goes before it
    }

    void midi(uint8_t status, uint8_t data1, uint8_t data2) override {
        if (pending_count < pending.size()) pending[pending_count++] = {status, data1, data2};
        idle.midi(status, data1, data2);
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
    // It hears the transport stopped at the top meanwhile: the engine host's clock belongs to the audio thread.
    void warm_up(unsigned blocks = 8) {
        std::array<float, EngineHost::block> l {}, r {};
        warming = true;
        for (unsigned b = 0; b < blocks; ++b) {
            l.fill(0.0f);
            r.fill(0.0f);
            render(l.data(), r.data(), EngineHost::block);
        }
        warming = false;
        load.store(0.0f, std::memory_order_relaxed); // what it costs to play counts from here
    }
    void render(float* left, float* right, unsigned frames) override {
        frames = std::min(frames, EngineHost::block);
        if (!frames) return;
        follow_clock(frames);
        // Asleep (plugin/idle_gate.h): not run at all. An effect's input is as quiet as its output; it gives silence.
        // MIDI for it wakes it, and so does a plug-in's own request to be run (CLAP's request_process, a screen input
        // its audio side takes) or a value waiting for it (a pot turned): else neither reaches it until a note does.
        double tail_seconds;
        if (!instrument && clap && clap->takeTailChange(tail_seconds)) idle.set_tail(plugin_rate, tail_seconds);
        const bool waiting = pending_count > 0 || (waker && waker->wantsProcess()) || poked.exchange(false, std::memory_order_acq_rel);
        bool sleeping = !warming && idle.asleep(waiting, now.playing, instrument ? nullptr : left, instrument ? nullptr : right, frames);
        asleep.store(sleeping, std::memory_order_relaxed);
        if (sleeping) {
            if (!instrument) { std::fill(left, left + frames, 0.0f); std::fill(right, right + frames, 0.0f); }
            if (watched.load(std::memory_order_relaxed)) {
                static const std::array<float, EngineHost::block> quiet {};
                measure(quiet.data(), quiet.data(), frames, true);
                measure(quiet.data(), quiet.data(), frames, false);
            }
            load.store(load.load(std::memory_order_relaxed) * 0.95f, std::memory_order_relaxed);
            return;
        }
        // Sample load 1 block in 16 instead of taking two clock reads per plug-in on every block.
        const bool sample_load = (++load_tick & 15) == 0;
        std::chrono::steady_clock::time_point t0;
        if (sample_load) t0 = std::chrono::steady_clock::now();
        if (bridge.converting()) bridge.render(left, right, frames, [&](float* l, float* r, unsigned n) { run(l, r, n, false); });
        else run(left, right, frames, instrument);
        if (sample_load) {
            double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            float share = float(took / (frames / sample_rate));
            load.store(load.load(std::memory_order_relaxed) * 0.8f + share * 0.2f, std::memory_order_relaxed);
        }
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
        const bool watching = watched.load(std::memory_order_relaxed) && !warming;
        if (watching && !instrument) measure(left, right, frames, true);
        midi_in.clear();
        for (unsigned n = 0; n < pending_count; ++n) {
            const auto& m = pending[n];
            uint8_t bytes[3] {m.status, m.data1, m.data2};
            int size = (m.status & 0xf0) == 0xc0 || (m.status & 0xf0) == 0xd0 ? 2 : 3;
            midi_in.addEvent(bytes, size, 0);
        }
        pending_count = 0;
        stamp_clock(frames);
        instance->processBlock(buffer, midi_in);
        if (outs <= 0) {
            if (!add) { std::fill(left, left + frames, 0.0f); std::fill(right, right + frames, 0.0f); }
            return;
        }
        const float* l = pointers[0];
        const float* r = outs >= 2 ? pointers[1] : pointers[0];
        if (watching) measure(l, r, frames, false);
        auto safe = [](float x) { return std::isfinite(x) ? std::clamp(x, -8.0f, 8.0f) : 0.0f; };
        float peak = 0; // taken while passing the samples by: avoids walking the output buffer a second time
        if (add) {
            for (unsigned f = 0; f < frames; ++f) {
                const float sl = safe(l[f]), sr = safe(r[f]);
                peak = std::max({peak, std::fabs(sl), std::fabs(sr)});
                left[f] += sl; right[f] += sr;
            }
        } else {
            for (unsigned f = 0; f < frames; ++f) {
                const float sl = safe(l[f]), sr = safe(r[f]);
                peak = std::max({peak, std::fabs(sl), std::fabs(sr)});
                left[f] = sl; right[f] = sr;
            }
        }
        idle.heardPeak(peak, frames);
    }

    std::unique_ptr<juce::AudioPluginInstance> instance;
    // Its companion screen (plugin/companion_screen.h), null none: after `instance`, so it goes first.
    std::unique_ptr<CompanionScreen> companion;
    ClapInstance* clap = nullptr;        // the instance, when it is a CLAP plug-in
    // The screen the screen thread draws: the instance's own (CLAP, VST2 that draw one), else its companion; and the
    // instance as the audio thread asks whether it wants its next block run.
    ScreenSource* screen_source = nullptr;
    ScreenSource* waker = nullptr;
    std::atomic<bool> poked {false};     // a companion's edit: its next block runs although it sleeps
    uint32_t generation = 0;             // its entry's when it was loaded
    const bool instrument;
    const double sample_rate;
    const double plugin_rate;            // what the plug-in itself runs at (an Open MPC one: 44.1 kHz)
    std::vector<int> shown;              // parameter indices any page may show: not hidden, not the bypass
    PluginLayout layout;                 // its pages in rows of three (plugin/plugin_pages.h), under the rack's lock
    std::string folder;                  // where it was picked from (an Open MPC folder holds its skin)
    std::vector<PluginPage> file_pages;  // a page file's pages (plugin/page_file.h), kept for a CLAP page change
    std::shared_ptr<const PluginSkin> skin; // its skin (plugin/plugin_skin.h), null none
    // Its own screen (plugin/plugin_screen.h): whether it offers one; once it has drawn (under the rack's lock), and
    // the parameters it puts on K1-K3 (indices in getParameters(), -1 none).
    bool has_screen = false;
    bool screen_drawn = false;
    std::array<int, 3> screen_controls {-1, -1, -1};
    bool qlinks = false;
    std::atomic<bool> transport {true};  // TRANSPORT: it hears Play and Stop (off: always stopped), set by the rack
    std::atomic<float> load {0};         // share of a block's time
    std::atomic<bool> asleep {false};    // its last render skipped (plugin/idle_gate.h)
    unsigned load_tick = 0;              // renders run, for 1-in-16 load sampling (audio thread)
    // Its sound as the page in view shows it (plugin_panel.h `scope`): written by the audio thread only while
    // `watched` (the panel's plug-in), a block's lowest and highest sample per entry, and peak levels in and out.
    std::atomic<bool> watched {false};
    std::array<std::atomic<uint32_t>, 64> scope {};
    std::atomic<uint32_t> scope_head {0};
    std::atomic<float> level_in {0}, level_out {0};
    void measure(const float* l, const float* r, unsigned frames, bool input) {
        float peak = 0, lo[2] {0, 0}, hi[2] {0, 0};
        for (unsigned f = 0; f < frames; ++f) {
            float a = std::isfinite(l[f]) ? l[f] : 0.0f, b = std::isfinite(r[f]) ? r[f] : 0.0f;
            lo[0] = std::min(lo[0], a); hi[0] = std::max(hi[0], a);
            lo[1] = std::min(lo[1], b); hi[1] = std::max(hi[1], b);
        }
        peak = std::max({-lo[0], hi[0], -lo[1], hi[1]});
        auto& level = input ? level_in : level_out;
        level.store(std::max(peak, level.load(std::memory_order_relaxed) * 0.9f), std::memory_order_relaxed);
        if (input) return;
        auto byte = [](float v) { return uint32_t(uint8_t(int8_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 127)))); };
        uint32_t packed = byte(lo[0]) | byte(hi[0]) << 8 | byte(lo[1]) << 16 | byte(hi[1]) << 24;
        uint32_t at = scope_head.load(std::memory_order_relaxed);
        scope[at % scope.size()].store(packed, std::memory_order_relaxed);
        scope_head.store(at + 1, std::memory_order_release);
    }
    std::string stage;                   // the RAM copy it runs from, removed with it
    PluginFormat format = PluginFormat::Vst3;
    PluginProgramList programs;          // its named programs, as it listed them on opening (a CLAP one's: once found)
    int program = -1;                    // the one picked last here (under the rack's lock)
    bool switching = false;              // a pick queued for the rack's thread (under the rack's lock)
    bool finding = false;                // a CLAP plug-in's presets still being looked for (under the rack's lock)
    std::vector<std::string> preset_files; // an MPC plug-in's .xpl presets, program xpl_first on (under the rack's lock)

private:
    // The engine host's clock at its block's start (engine/effects.h, EffectClock), counted on by the device frames
    // this plug-in has played of the block already: an instrument is played in parts, between the block's steps.
    void follow_clock(unsigned frames) {
        if (warming) { now = EffectClock {}; quarters = 0; per_frame = 0; return; }
        now = *effect_clock_now;
        if (!transport.load(std::memory_order_relaxed)) now.playing = false; // TRANSPORT off: the place, never Play
        if (now.block != block_seen) { block_seen = now.block; into_block = 0; }
        double quarter = std::max(now.quarter, 1.0);
        quarters = now.playing ? now.song + into_block / quarter : now.song;
        per_frame = now.playing ? sample_rate / plugin_rate / quarter : 0.0; // quarter notes a frame at its rate
        into_block += frames;
    }
    // Where this processBlock of `frames` (at the plug-in's rate) starts; the next one starts after it.
    void stamp_clock(unsigned frames) {
        using Head = juce::AudioPlayHead;
        double bar = 4.0 * now.sig_beats / now.sig_unit, bpm = now.bpm > 0 ? now.bpm : 120.0;
        double bars = std::floor(quarters / bar + 1e-9), seconds = quarters * 60.0 / bpm;
        Head::PositionInfo p;
        p.setIsPlaying(now.playing);
        p.setIsRecording(false);
        p.setIsLooping(false);
        p.setBpm(bpm);
        p.setTimeSignature(Head::TimeSignature {int(now.sig_beats), int(now.sig_unit)});
        p.setPpqPosition(quarters);
        p.setPpqPositionOfLastBarStart(bars * bar);
        p.setBarCount(int64_t(bars));
        p.setTimeInSeconds(seconds);
        p.setTimeInSamples(int64_t(std::llround(seconds * plugin_rate)));
        clock.position = p;
        quarters += frames * per_frame;
    }

    struct Midi { uint8_t status = 0, data1 = 0, data2 = 0; };
    PluginClock clock;
    EffectClock now;           // the engine host's clock as this render found it
    uint64_t block_seen = ~uint64_t(0);
    double into_block = 0;     // device frames of that block played already
    double quarters = 0;       // where the next processBlock starts, quarter notes from the song's top
    double per_frame = 0;      // and how far a frame at the plug-in's rate moves it
    bool warming = false;
    PluginRateBridge bridge;
    IdleGate idle;             // when it may sleep
    std::array<Midi, 512> pending {};
    unsigned pending_count = 0;
    int channels = 2;
    std::vector<float> memory;
    juce::MidiBuffer midi_in;
};

PluginRack::PluginRack(EngineHost& engine, std::string stage)
    : host(engine), stage_tag(std::move(stage)), companions(std::make_unique<CompanionHost>()) {
    racks.fetch_add(1);
    thread = std::thread([this] { run(); });
}

PluginRack::~PluginRack() {
    {
        std::lock_guard<std::mutex> held(screen_lock);
        screen_stop = true;
    }
    screen_wake.notify_all();
    if (screen_thread.joinable()) screen_thread.join(); // before the plug-ins it draws go
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
            entry.transport = project.plugins[e].transport;
            if (entry.live) entry.live->transport.store(entry.transport, std::memory_order_relaxed);
            entry.preset = project.plugins[e].preset;
            if (entry.live && entry.live->program < 0) entry.live->program = program_keyed(entry.live->programs, entry.preset);
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

std::vector<uint8_t> PluginRack::state(unsigned entry) const {
    if (entry >= max_plugins) return {};
    std::lock_guard<std::mutex> held(lock);
    const auto& e = entries[entry];
    if (!e.live || e.status != PluginStatus::Ready) return e.state;
    juce::MemoryBlock block;
    e.live->instance->getStateInformation(block);
    return {static_cast<const uint8_t*>(block.getData()), static_cast<const uint8_t*>(block.getData()) + block.getSize()};
}

void PluginRack::restore(unsigned entry, std::vector<uint8_t> state) {
    if (entry >= max_plugins) return;
    std::lock_guard<std::mutex> held(lock);
    entries[entry].state = std::move(state);
    entries[entry].reload = true; // plan_plugin keeps this state for whichever plug-in loads
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
    const auto& layout = h->layout;
    out.rows = uint16_t(layout.rows());
    out.row = uint16_t(out.rows ? std::min<unsigned>(row, out.rows - 1u) : 0);
    out.pages = uint16_t(layout.pages());
    if (out.pages) {
        out.page = uint16_t(layout.page_of(out.row));
        out.page_row = uint16_t(out.row - layout.first_rows[out.page]);
        out.page_rows = uint16_t(layout.rows_of(out.page));
        out.prev_page = layout.first_rows[out.page > 0 ? out.page - 1u : out.page];
        out.next_page = layout.first_rows[out.page + 1u < out.pages ? out.page + 1u : out.page];
        put(out.page_name, sizeof(out.page_name), layout.names[out.page]);
    }
    for (const auto& other : handles) other->watched.store(other.get() == h, std::memory_order_relaxed);
    out.measured = true;
    uint32_t head = h->scope_head.load(std::memory_order_acquire);
    for (size_t n = 0; n < out.scope.size(); ++n) out.scope[n] = h->scope[(head + n) % h->scope.size()].load(std::memory_order_relaxed);
    out.level_in = uint16_t(std::clamp(h->level_in.load(std::memory_order_relaxed), 0.0f, 1.0f) * 1000);
    out.level_out = uint16_t(std::clamp(h->level_out.load(std::memory_order_relaxed), 0.0f, 1.0f) * 1000);
    out.load = uint16_t(std::clamp(h->load.load(std::memory_order_relaxed) * 1000.0f, 0.0f, 9999.0f));
    out.programs = uint16_t(std::min<size_t>(h->programs.size(), 0xffff));
    const auto& params = h->instance->getParameters();
    auto read_param = [&](int index, const std::string& label, PluginPanelParam& card) {
        if (index < 0 || index >= params.size()) return; // a page's last row, not full
        auto* p = params[index];
        card.used = true;
        std::string name = label.empty() ? p->getName(int(sizeof(card.name) - 1)).trim().toStdString() : label;
        put(card.name, sizeof(card.name), name);
        float v = p->getValue();
        put(card.text, sizeof(card.text), tidy(p->getText(v, 64).trim().toStdString()));
        card.value = int16_t(std::lround(std::clamp(v, 0.0f, 1.0f) * plugin_param_max));
        card.initial = int16_t(std::lround(std::clamp(p->getDefaultValue(), 0.0f, 1.0f) * plugin_param_max));
        int steps = p->getNumSteps();
        card.steps = uint16_t(steps > 1 && steps < plugin_param_max ? steps : 0);
    };
    auto read = [&](size_t at, PluginPanelParam& card) { read_param(layout.at(at), at < layout.labels.size() ? layout.labels[at] : std::string(), card); };
    for (unsigned k = 0; k < 3; ++k) {
        size_t at = size_t(out.row) * 3 + k;
        if (at >= layout.slots.size()) break;
        read(at, out.params[k]);
    }
    // Two rows of the page in hand: its own and the next, or on the page's last row the one before.
    unsigned first = out.row;
    out.window_rows = uint8_t(out.page_rows >= 2 ? 2 : out.rows ? 1 : 0);
    if (out.page_rows >= 2 && out.page_row + 1u >= out.page_rows) first = out.row - 1u;
    out.window_first = uint16_t(first * 3u);
    for (unsigned k = 0; k < out.window_rows * 3u; ++k) {
        size_t at = size_t(out.window_first) + k;
        if (at >= layout.slots.size()) break;
        read(at, out.window[k]);
    }
    // Its own screen in view, once it has drawn: K1-K3 are the three it names (PluginPanel::screen).
    out.has_screen = h->has_screen;
    bool in_view = false;
    {
        std::lock_guard<std::mutex> seen(screen_lock);
        in_view = screen_now.entry == entry;
    }
    if (h->has_screen && h->screen_drawn && in_view) {
        out.screen = true;
        out.screen_keys = uint8_t(h->screen_source ? h->screen_source->screenKeys() : 0u);
        for (unsigned k = 0; k < 3; ++k) {
            out.params[k] = {};
            read_param(h->screen_controls[k], {}, out.params[k]);
        }
    }
    // The skin's page for this one (by its name), an effect's strip: its controls' parameters and where they are.
    if (h->skin && out.pages) {
        const PluginSkin::Page* page = nullptr;
        if (!out.instrument && h->skin->has_strip) { page = &h->skin->strip; out.skin_page = -2; }
        for (size_t n = 0; !page && n < h->skin->pages.size(); ++n)
            if (h->skin->pages[n].first == layout.names[out.page]) { page = &h->skin->pages[n].second; out.skin_page = int16_t(n); }
        if (page) {
            auto from = layout.slots.begin() + long(layout.first_rows[out.page]) * 3, to = from + long(layout.rows_of(out.page)) * 3;
            size_t at = 0;
            for (const auto& c : page->controls) {
                if (c.param < 0) continue;
                if (at >= out.skin_params.size()) break;
                read_param(c.param, {}, out.skin_params[at]);
                auto it = std::find(from, to, c.param);
                out.skin_slots[at++] = it != to ? int16_t(it - layout.slots.begin()) : int16_t(-1);
            }
        }
    }
    if (auto picture = picture_for(layout, out.page, out.row); picture.kind != PagePicture::None) {
        out.picture = uint8_t(picture.kind);
        for (unsigned n = 0; n < PagePictureSlots::most; ++n) {
            out.picture_slots[n] = int16_t(picture.slots[n]);
            if (picture.slots[n] >= 0) read(size_t(picture.slots[n]), out.picture_params[n]); // its card's label
            else read_param(picture.params[n], {}, out.picture_params[n]);                  // not on the page
        }
    }
}

std::shared_ptr<const PluginSkin> PluginRack::skin(unsigned entry) const {
    if (entry >= max_plugins) return nullptr;
    std::lock_guard<std::mutex> held(lock);
    const auto& e = entries[entry];
    return e.live && e.status == PluginStatus::Ready ? e.live->skin : nullptr;
}

void PluginRack::set_param(unsigned entry, unsigned shown_index, int value) {
    if (entry >= max_plugins) return;
    std::lock_guard<std::mutex> held(lock);
    auto* h = entries[entry].live;
    if (!h) return;
    int index = h->layout.at(shown_index);
    if (index < 0 || index >= h->instance->getParameters().size()) return;
    auto* p = h->instance->getParameters()[index];
    float v = std::clamp(value, 0, plugin_param_max) / float(plugin_param_max);
    p->setValue(v);
}

void PluginRack::set_screen_param(unsigned entry, unsigned k, int value) {
    if (entry >= max_plugins || k >= 3) return;
    std::lock_guard<std::mutex> held(lock);
    auto* h = entries[entry].live;
    if (!h || !h->has_screen) return;
    int index = h->screen_controls[k];
    const auto& params = h->instance->getParameters();
    if (index < 0 || index >= params.size()) return;
    params[index]->setValue(std::clamp(value, 0, plugin_param_max) / float(plugin_param_max));
    if (h->companion) h->poked.store(true, std::memory_order_release); // a companion's draws what it plays
}

void PluginRack::set_param_index(unsigned entry, uint32_t generation, int index, int value) {
    if (entry >= max_plugins) return;
    std::lock_guard<std::mutex> held(lock);
    const auto& e = entries[entry];
    auto* h = e.live;
    if (!h || e.status != PluginStatus::Ready || h->generation != generation) return; // replaced since it was asked
    const auto& params = h->instance->getParameters();
    if (index < 0 || index >= params.size()) return;
    params[index]->setValue(std::clamp(value, 0, plugin_param_max) / float(plugin_param_max));
    h->poked.store(true, std::memory_order_release);
}

uint32_t PluginRack::live_generation(unsigned entry) const {
    if (entry >= max_plugins) return 0;
    std::lock_guard<std::mutex> held(lock);
    const auto& e = entries[entry];
    return e.live && e.status == PluginStatus::Ready ? e.live->generation : 0;
}

void PluginRack::take_screen_requests(std::vector<ScreenRequest>& out) {
    std::vector<CompanionRequest> asked;
    std::lock_guard<std::mutex> held(lock);
    for (unsigned n = 0; n < max_plugins; ++n) {
        const auto& e = entries[n];
        if (!e.live || !e.live->companion || e.status != PluginStatus::Ready) continue;
        asked.clear();
        e.live->companion->take(asked);
        for (const auto& a : asked) out.push_back({uint8_t(n), e.live->generation, a.program, a.index, a.value});
    }
}

std::string PluginRack::screen_notice() {
    std::lock_guard<std::mutex> held(lock);
    if (screen_notices.empty()) return {};
    std::string first = screen_notices.front();
    screen_notices.erase(screen_notices.begin());
    return first;
}

std::string PluginRack::screens() const {
    std::string out;
    std::lock_guard<std::mutex> held(lock);
    for (unsigned n = 0; n < max_plugins; ++n) {
        const auto& e = entries[n];
        if (!e.live || !e.live->companion) continue;
        const auto& c = *e.live->companion;
        std::string folder = c.package().folder;
        for (char& ch : folder) if (ch == ',' || ch == ':' || ch == ' ') ch = '_';
        if (!out.empty()) out += ',';
        out += std::to_string(n) + ":" + folder + ":" + (c.hasScreen() ? "on" : "stopped");
    }
    return out.empty() ? "none" : out;
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
        for (const auto& p : e.live->programs) if (p.index == program) { e.preset = program_key(p); break; }
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
    std::string preset_file; // an MPC plug-in's .xpl
    {
        std::lock_guard<std::mutex> held(lock);
        auto& e = entries[entry];
        if (e.generation != generation || !e.live) return; // changed since: the plug-in it was for is gone
        instance = e.live->instance.get(); // freed only on this thread, so it outlives the switch
        if (program >= xpl_first && size_t(program - xpl_first) < e.live->preset_files.size())
            preset_file = e.live->preset_files[size_t(program - xpl_first)];
    }
    if (!preset_file.empty()) { // its state, as the plug-in's VST2 host saves one (picked again: loaded again)
        auto preset = xpl_preset_in(text_of(preset_file, xpl_most));
        if (!preset.state.empty()) instance->setStateInformation(preset.state.data(), int(preset.state.size()));
    } else if (program >= 0 && program < instance->getNumPrograms()) {
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
    std::string wanted_root = plugin_folder(top);
    {
        std::lock_guard<std::mutex> held(lock);
        if (top == card_top) return;
        card_top = top;
        root = wanted_root;
        // Entries named from the card's top (Melodee/Plugins/...) that could not open try again on this card at the next sync().
        for (auto& entry : entries)
            if (!entry.path.empty() && fs::path(entry.path).is_relative() && entry.status == PluginStatus::Failed) entry.reload = true;
    }
    browse("");
}

void PluginRack::browse(const std::string& where, const std::string& row, int32_t row_id) {
    Job job {Job::Kind::Browse, 0, 0, where, 0, row_id, row};
    push(std::move(job));
}

void PluginRack::card_changed() {
    std::vector<Known> had;
    {
        std::lock_guard<std::mutex> held(lock);
        had = known;
    }
    std::vector<std::string> gone; // looked at without the lock: the card may be slow
    for (const auto& k : had) {
        std::error_code e;
        auto time = fs::last_write_time(k.path, e);
        if (e || time != k.time) gone.push_back(k.path);
    }
    {
        std::lock_guard<std::mutex> held(lock);
        known.erase(std::remove_if(known.begin(), known.end(), [&](const Known& k) {
            return std::find(gone.begin(), gone.end(), k.path) != gone.end();
        }), known.end());
    }
    card_count.fetch_add(1, std::memory_order_acq_rel);
}

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
            std::string row = job.row, at;
            {
                std::lock_guard<std::mutex> held(lock);
                if (job.entry == 1) row = shown.folder; // going up: the row of the folder it came out of
                at = job.path.empty() ? root : job.path;
            }
            list(at, row, job.entry == 1 ? 0 : job.id);
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

void PluginRack::list(const std::string& at, const std::string& row, int32_t row_id) {
    std::string top, card;
    {
        std::lock_guard<std::mutex> held(lock);
        top = root;
        card = card_top;
    }
    View v;
    v.root = top;
    if (!top.empty()) {
        // Anywhere on the card; a folder off it lists the Plugins folder.
        std::error_code e;
        auto canonical_card = fs::weakly_canonical(card, e), canonical_top = fs::weakly_canonical(top, e),
             canonical_at = fs::weakly_canonical(at.empty() ? top : at, e);
        bool there = fs::exists(canonical_at, e); // deleted from the web page while it was listed
        v.folder = (there && under(canonical_card, canonical_at) ? canonical_at : canonical_top).string();
        v.top = v.folder == canonical_card.string();
        v.home = v.folder == canonical_top.string();
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
            // A release zip copied as it came into the Plugins folder: its plug-in folder takes its place. A zip
            // elsewhere on the card is someone's own, left as it is and not listed.
            bool plugins_here = under(canonical_top, v.folder);
            if (plugins_here) unzip_plugins(v.folder);
            v.files = list_plugins(v.folder, plugins_here);
            if (v.top) // what starts the app and the base's own files: no plug-ins, and libraries not to open
                v.files.erase(std::remove_if(v.files.begin(), v.files.end(), [](const PluginFile& f) {
                    auto name = fs::path(f.path).filename().string();
                    return name == "autorun" || name == "sample-base";
                }), v.files.end());
        }
    }
    if (!row.empty())
        for (size_t n = 0; n < v.files.size(); ++n)
            if (v.files[n].path == row && v.files[n].id == row_id) v.came_from = int(n);
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
    std::error_code when;
    found.time = fs::last_write_time(file.path, when);
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
    std::string top;
    {
        std::lock_guard<std::mutex> held(lock);
        top = card_top;
    }
    // A path relative to the card's top (a track Melodee's own synth was), or one in the card's old top-level
    // Plugins folder: on the card in now, in Melodee/Plugins.
    std::string path = card_plugin_path(top, job.path);
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
    bool nam = false;      // Neural Amp Modeler: its folders made (plugin/nam.h); its pages are the library's (page_library.h)
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
    if (!state.empty()) instance->setStateInformation(state.data(), int(state.size()));
    juce::PluginDescription description;
    instance->fillInPluginDescription(description);
    bool is_instrument = described_instrument(description) || (instance->getTotalNumInputChannels() == 0 && instance->getTotalNumOutputChannels() > 0);

    auto handle = std::make_unique<PluginHandle>(std::move(instance), is_instrument, device_rate, rate);
    handle->stage = stage.string();
    handle->format = file.format;
    handle->programs = named_programs(*handle->instance);
    // A CLAP plug-in's presets, or an MPC plug-in folder's .xpl files: looked for once it plays (find_presets()).
    handle->finding = file.format == PluginFormat::Clap || has_mpc_presets(file);
    handle->folder = file.path;
    handle->file_pages = file_pages(*handle->instance, handle->clap, file, top, handle->shown, page_problems, handle->skin);
    write_page_problems(top);
    auto pages = plugin_pages(*handle->instance, handle->clap, file.format, file.path, handle->shown, handle->file_pages);
    handle->qlinks = handle->file_pages.empty() && (file.format == PluginFormat::Vst2 || file.format == PluginFormat::Vst2Arm32)
                     && !pages.empty() && !pages[0].name.empty();
    handle->layout = layout_of(*handle->instance, pages);
    handle->generation = job.generation;
    if (!handle->has_screen) find_companion(*handle, file, top, stage.string()); // a screen of its own comes first
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
            raw->transport.store(entry.transport, std::memory_order_relaxed); // before the audio thread first runs it
            raw->program = program_keyed(raw->programs, entry.preset); // its state already plays it: the name only
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
    if (file.format == PluginFormat::Clap || has_mpc_presets(file)) push({Job::Kind::Presets, job.entry, job.generation, {}, 0, 0});
}

// Rack thread: a CLAP plug-in's presets, looked for once it plays (the first time, Surge XT's on the card take a
// while: thousands of files), then PRESETS' programs.
void PluginRack::find_presets(unsigned entry, uint32_t generation) {
    juce::AudioPluginInstance* instance = nullptr;
    ClapInstance* clap = nullptr;
    PluginProgramList own;
    std::string folder;
    {
        std::lock_guard<std::mutex> held(lock);
        auto& e = entries[entry];
        if (e.generation != generation || !e.live) return; // changed since: the plug-in it was for is gone
        instance = e.live->instance.get(); // freed only on this thread, so it outlives this
        clap = dynamic_cast<ClapInstance*>(instance);
        own = e.live->programs;
        folder = e.live->folder;
    }
    std::vector<PluginProgram> list;
    std::vector<std::string> files;
    if (clap) {
        auto found = clap->findPresets(clap_preset_cache(use_plugin_home(), clap->clapId()));
        list.reserve(found.size());
        for (size_t n = 0; n < found.size(); ++n) list.push_back({int(n), found[n].name, found[n].folder});
        clap->setPresets(std::move(found));
    } else { // an MPC plug-in folder: its own programs, then its .xpl files
        juce::PluginDescription description;
        instance->fillInPluginDescription(description);
        list.assign(own.begin(), own.end());
        auto found = mpc_presets(folder, own, description.uniqueId, files);
        list.insert(list.end(), found.begin(), found.end());
    }
    {
        std::lock_guard<std::mutex> held(lock);
        auto& e = entries[entry];
        if (e.generation == generation && e.live && e.live->instance.get() == instance) {
            e.live->programs = PluginProgramList(std::move(list));
            e.live->preset_files = std::move(files);
            e.live->finding = false;
            if (e.live->program < 0) e.live->program = program_keyed(e.live->programs, e.preset); // as at opening

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
    {
        std::lock_guard<std::mutex> calls(screen_calls); // the screen thread is not inside it
        if (owned->companion) owned->companion->close();
        if (juce_hosted(owned->format)) message_thread().run([&] { owned.reset(); });
        else owned.reset();
    }
    std::error_code e;
    if (!stage.empty()) fs::remove_all(stage, e);
    give_back_memory();
}

// What CLAP plug-ins asked of their main thread (plugin/clap_instance.h), done on this one. Handles are freed only on
// this thread, so they outlive the calls.
// The page files' problems (plugin/page_file.h) in the card's Melodee/Plugin Pages/problems.txt, a line each, when
// that folder is there; rewritten only when they changed, removed when there are none. Rack's thread.
void PluginRack::write_page_problems(const std::string& top) {
    if (top.empty()) return;
    std::string text;
    for (const auto& [source, lines] : page_problems)
        for (const auto& line : lines) text += source + ": " + line + "\n";
    if (text == page_problems_written) return;
    std::error_code e;
    fs::path folder = fs::path(top) / "Melodee" / "Plugin Pages";
    if (!fs::is_directory(folder, e)) return;
    if (text.empty()) fs::remove(folder / "problems.txt", e);
    else std::ofstream(folder / "problems.txt", std::ios::binary | std::ios::trunc) << text;
    page_problems_written = text;
}

// The same for companion screens, in Melodee/Plugin Screens/problems.txt. Rack's thread.
void PluginRack::write_screen_problems(const std::string& top) {
    if (top.empty()) return;
    std::string text;
    for (const auto& [source, lines] : screen_problems)
        for (const auto& line : lines) text += source + ": " + line + "\n";
    if (text == screen_problems_written) return;
    std::error_code e;
    fs::path folder = fs::path(top) / "Melodee" / "Plugin Screens";
    if (!fs::is_directory(folder, e)) return;
    if (text.empty()) fs::remove(folder / "problems.txt", e);
    else std::ofstream(folder / "problems.txt", std::ios::binary | std::ios::trunc) << text;
    screen_problems_written = text;
}

// A companion screen for a plug-in just opened that has no screen of its own (sdk/melodee-screen/melodee_companion.h):
// each folder in the card's Melodee/Plugin Screens with a screen.json, checked against the plug-in. The one that fits
// (not one that stopped earlier this run) gets a copy of its library in the plug-in's RAM stage (the card is mounted
// noexec) and becomes its screen; several that fit: none, and problems.txt says which. Rack's thread.
void PluginRack::find_companion(PluginHandle& handle, const PluginFile& file, const std::string& top, const std::string& stage) {
    if (top.empty()) return;
    std::error_code e;
    const fs::path root = fs::path(top) / "Melodee" / "Plugin Screens";
    if (!fs::is_directory(root, e)) return;
    CompanionTarget target;
    target.who = identity_of(*handle.instance, handle.clap, file);
    target.vst2 = file.format == PluginFormat::Vst2 || file.format == PluginFormat::Vst2Arm32;
    for (auto* p : handle.instance->getParameters()) target.names.push_back(p->getName(64).trim().toStdString());
    struct Found { std::string folder; CompanionManifest manifest; CompanionFit fit; };
    std::vector<Found> found;
    std::vector<fs::path> folders;
    for (fs::directory_iterator it(root, e), end; !e && it != end; it.increment(e))
        if (it->is_directory(e) && it->path().filename().string().rfind('.', 0) != 0) folders.push_back(it->path());
    std::sort(folders.begin(), folders.end());
    std::set<std::string> stopped;
    {
        std::lock_guard<std::mutex> held(lock);
        stopped = companion_stopped;
    }
    for (const auto& folder : folders) {
        const std::string name = folder.filename().string();
        const std::string json = text_of(folder / "screen.json", companion_manifest_most);
        if (json.empty()) continue; // not a screen's folder (yet)
        Found f {name, {}, CompanionFit::Other};
        std::string why;
        if (!read_companion_manifest(json, f.manifest, why)) { screen_problems[name] = {why}; continue; }
        f.fit = companion_fit(f.manifest, target, why);
        if (f.fit == CompanionFit::Other) continue;
        if (f.fit == CompanionFit::Wrong) { screen_problems[name] = {"not for this " + target.who.name + ": " + why}; continue; }
        if (stopped.count(name)) { screen_problems[name] = {"stopped earlier; used again once Melodee restarts"}; continue; }
        if (!fs::is_regular_file(folder / f.manifest.library, e)) { screen_problems[name] = {"no " + f.manifest.library}; continue; }
        found.push_back(std::move(f));
    }
    std::vector<CompanionFit> fits;
    for (const auto& f : found) fits.push_back(f.fit);
    const auto pick = pick_companion(fits);
    if (pick.ambiguous) {
        std::string names;
        for (const auto& f : found) names += (names.empty() ? "" : ", ") + f.folder;
        for (const auto& f : found) screen_problems[f.folder] = {"several screens fit " + target.who.name + " (" + names + "); none is used"};
    }
    if (pick.package >= 0) {
        auto& f = found[size_t(pick.package)];
        CompanionPackage package;
        package.folder = f.folder;
        package.manifest = std::move(f.manifest);
        const fs::path from = root / package.folder / package.manifest.library, to = fs::path(stage) / "companion" / package.manifest.library;
        const auto size = fs::file_size(from, e);
        if (e || size > (32u << 20)) screen_problems[package.folder] = {"its library is too large or unreadable"};
        else if (fs::create_directories(to.parent_path(), e), !fs::copy_file(from, to, fs::copy_options::overwrite_existing, e))
            screen_problems[package.folder] = {"its library could not be copied: " + e.message()};
        else {
            screen_problems.erase(package.folder);
            package.library = to.string();
            handle.use_companion(std::make_unique<CompanionScreen>(*handle.instance, *companions, std::move(package), ++companion_sessions));
        }
    }
    write_screen_problems(top);
}

void PluginRack::idle_plugins() {
    std::vector<PluginHandle*> clap; // freed only on this thread, so still there after the lock
    {
        std::lock_guard<std::mutex> held(lock);
        for (const auto& h : handles)
            if (h->format == PluginFormat::Clap && h->clap) clap.push_back(h.get());
    }
    bool moved = false;
    for (auto* h : clap) {
        h->clap->idle();
        if (!h->clap->remotePagesChanged()) continue; // its pages (a preset's own, say): read again
        auto layout = layout_of(*h->instance, plugin_pages(*h->instance, h->clap, h->format, h->folder, h->shown, h->file_pages));
        std::lock_guard<std::mutex> held(lock);
        h->layout = std::move(layout);
        moved = true;
    }
    if (moved) changed();
}

// --- A plug-in's own screen (plugin/plugin_screen.h, sdk/melodee-screen/melodee_screen.h) ---

void PluginRack::screen_view(const ScreenView& view) {
    bool poke = false;
    {
        std::lock_guard<std::mutex> held(screen_lock);
        if (screen_stop) return;
        if (view.entry != no_plugin && !screen_thread.joinable()) screen_thread = std::thread([this] { screen_run(); });
        poke = view != screen_now;
        screen_now = view;
        screen_poked = screen_poked || poke;
    }
    if (poke) screen_wake.notify_one();
}

uint8_t PluginRack::screen_keys(unsigned entry) const {
    if (entry >= max_plugins) return 0;
    std::lock_guard<std::mutex> held(lock);
    const auto& e = entries[entry];
    if (!e.live || e.status != PluginStatus::Ready || !e.live->has_screen || !e.live->screen_source) return 0;
    return uint8_t(e.live->screen_source->screenKeys() & (screen_key_mute | screen_key_erase));
}

void PluginRack::screen_input(unsigned entry, const ScreenInput& input) {
    {
        std::lock_guard<std::mutex> held(screen_lock);
        if (entry != screen_now.entry || screen_inputs.size() >= 64) return; // not the page in view; a flood: dropped
        screen_inputs.emplace_back(entry, input);
    }
    screen_wake.notify_one();
}

ScreenPicturePtr PluginRack::screen_picture() const {
    std::lock_guard<std::mutex> held(screen_lock);
    if (!screen_latest || screen_latest->entry != screen_now.entry || screen_latest->area != screen_now.area) return nullptr;
    return screen_latest;
}

// The screen thread. It owns the frame the plug-in draws into, which keeps that plug-in's last picture between draws;
// another plug-in in view, or the same one's other area, starts with a whole picture. It draws when the plug-in asks
// (redraw), after an input and when what the page tells it changes, at most every `gap`. A call the plug-in's other
// calls keep waiting is tried again a tick later, so a preset load on the rack's thread never holds this one up.
void PluginRack::screen_run() {
    using Clock = std::chrono::steady_clock;
    using std::chrono::milliseconds;
    constexpr milliseconds gap {16}, tick {16}, missing {100}, rest {500};
    const auto start = Clock::now();
    std::vector<uint32_t> canvas(size_t(picture_width) * picture_page_rows, 0);
    std::array<std::shared_ptr<ScreenPicture>, 3> pool {};
    std::deque<std::pair<unsigned, ScreenInput>> inputs;
    const PluginHandle* drawn = nullptr;
    unsigned drawn_entry = no_plugin;
    uint8_t drawn_area = 0;
    uint32_t serial = 0;
    bool whole = true, owed = false, ask_controls = false;
    Clock::time_point last = start - std::chrono::seconds(1);
    milliseconds wait = rest;
    // The companions' process, let go once no companion screen has been in view for a while (it costs memory).
    Clock::time_point companion_seen = start;
    auto companions_idle = [&] {
        if (Clock::now() - companion_seen < std::chrono::seconds(30)) return;
        std::lock_guard<std::mutex> calls(screen_calls);
        if (companions->running()) companions->stop();
    };
    for (;;) {
        ScreenView view;
        {
            std::unique_lock<std::mutex> held(screen_lock);
            screen_wake.wait_for(held, wait, [&] { return screen_stop || screen_poked || !screen_inputs.empty(); });
            if (screen_stop) return;
            owed = owed || screen_poked;
            screen_poked = false;
            view = screen_now;
            for (auto& in : screen_inputs) inputs.push_back(in);
            screen_inputs.clear();
        }
        inputs.erase(std::remove_if(inputs.begin(), inputs.end(), [&](const auto& in) { return in.first != view.entry; }), inputs.end());
        if (view.entry >= max_plugins) { wait = rest; drawn = nullptr; companions_idle(); continue; }
        wait = tick;
        PluginHandle* h = nullptr;
        bool companion = false;
        {
            std::lock_guard<std::mutex> held(lock);
            const auto& e = entries[view.entry];
            if (e.live && e.status == PluginStatus::Ready && e.live->has_screen) h = e.live;
            companion = h && h->companion;
        }
        if (companion) companion_seen = Clock::now();
        else companions_idle();
        std::lock_guard<std::mutex> calls(screen_calls); // destroy() frees no plug-in meanwhile
        if (h) { // still there, now that destroy() is kept out
            std::lock_guard<std::mutex> held(lock);
            const auto& e = entries[view.entry];
            if (e.live != h || !h->has_screen) h = nullptr;
        }
        if (!h || !h->screen_source) { wait = missing; continue; }
        auto* source = h->screen_source;
        // A companion that crashed, hung or misbehaved (plugin/companion_screen.h): its plug-in's pages again, and the
        // shell says why once.
        auto stopped = [&] {
            if (!h->companion || h->companion->hasScreen()) return false;
            const auto& package = h->companion->package();
            std::string why = h->companion->stop_reason();
            std::lock_guard<std::mutex> held(lock);
            if (h->has_screen) {
                h->has_screen = h->screen_drawn = false;
                companion_stopped.insert(package.folder);
                screen_notices.push_back(package.manifest.name + " stopped: " + why);
                changed();
            }
            return true;
        };
        if (stopped()) { drawn = nullptr; wait = missing; continue; }
        if (h != drawn || view.entry != drawn_entry || view.area != drawn_area) {
            drawn = h; drawn_entry = view.entry; drawn_area = view.area;
            whole = owed = ask_controls = true;
        }
        if (source->takeRedraw()) owed = true;
        while (!inputs.empty()) {
            const auto& in = inputs.front().second;
            static_assert(uint32_t(ScreenKey::Mute) == MELODEE_SCREEN_MUTE && uint32_t(ScreenKey::Erase) == MELODEE_SCREEN_ERASE
                          && screen_key_mute == MELODEE_SCREEN_KEY_MUTE && screen_key_erase == MELODEE_SCREEN_KEY_ERASE);
            melodee_screen_input_t raw {uint32_t(in.key), in.value, in.shift ? 1u : 0u};
            bool used = false;
            if (!source->screenInput(raw, used)) break; // busy: the next tick
            inputs.pop_front();
            owed = ask_controls = true;
        }
        if (stopped()) { drawn = nullptr; wait = missing; inputs.clear(); continue; }
        const auto now = Clock::now();
        if (owed && now - last < gap) {
            wait = std::chrono::duration_cast<milliseconds>(gap - (now - last)) + milliseconds(1);
        } else if (owed) {
            const unsigned height = view.area ? picture_strip_rows : picture_page_rows;
            if (whole) std::fill(canvas.begin(), canvas.end(), 0u);
            melodee_screen_frame_t frame {};
            frame.pixels = canvas.data();
            frame.width = picture_width;
            frame.height = height;
            frame.stride = picture_width;
            frame.area = view.area;
            frame.redraw_all = whole ? 1u : 0u;
            frame.colour = view.colour & 0xffffffu;
            frame.time_ms = uint64_t(std::chrono::duration_cast<milliseconds>(now - start).count());
            frame.playing = view.playing ? 1 : 0;
            frame.shift = view.shift ? 1 : 0;
            for (unsigned k = 0; k < 3; ++k) { frame.pots[k] = view.pots[k]; frame.pots_live[k] = view.live[k]; }
            bool changed = false;
            const bool done = source->screenDraw(frame, changed);
            if (stopped()) { drawn = nullptr; wait = missing; continue; }
            if (done) {
                last = now;
                owed = false;
                ask_controls = true;
                if (changed || whole) {
                    std::shared_ptr<ScreenPicture> picture;
                    for (auto& p : pool) // one no snapshot holds any more, nor screen_latest
                        if (!p || p.use_count() == 1) { if (!p) p = std::make_shared<ScreenPicture>(); picture = p; break; }
                    if (!picture) picture = std::make_shared<ScreenPicture>();
                    picture->entry = uint8_t(view.entry);
                    picture->area = view.area;
                    picture->serial = ++serial;
                    picture->width = uint16_t(picture_width);
                    picture->height = uint16_t(height);
                    const size_t count = size_t(picture_width) * height;
                    picture->pixels.resize(count);
                    for (size_t n = 0; n < count; ++n) picture->pixels[n] = canvas[n] | 0xff000000u; // opaque
                    {
                        std::lock_guard<std::mutex> held(screen_lock);
                        screen_latest = picture;
                    }
                    std::lock_guard<std::mutex> held(lock);
                    h->screen_drawn = true;
                }
                whole = false;
            }
        }
        if (ask_controls) {
            std::array<int, 3> controls {-1, -1, -1};
            if (source->screenControls(controls)) {
                ask_controls = false;
                std::lock_guard<std::mutex> held(lock);
                h->screen_controls = controls;
            }
        }
    }
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
