// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Two small CLAP plug-ins in one library, for tests/clap_host.cpp: Test Sine
// (an instrument: a sine per note, Level and Wave parameters, CC 1 sets Level
// and says so as an output event) and Test Gain (an effect). Each counts the
// calls it got on the wrong thread, by the host's thread-check, and
// fixture_counts() reports them.
//
// Presets, as Surge XT keeps its own: files in folders beside the library
// (TestPresets, and TestUser once there), found through preset discovery and
// loaded through preset-load. A .tsp file holds a preset a line: its name, a
// tab, its value (Sine's Level, Gain's Gain), and maybe a tab and who it is
// for ("gain" Test Gain, "-" no one named; else Test Sine). A file of several
// gives each its line number as its load key. fixture_preset_reads() counts
// the files read.
//
// Both draw their own screen (sdk/melodee-screen/melodee_screen.h): a bar as
// wide as Sine's Level, or half Gain's Gain, in the track's colour over a dark
// page or strip.
#include <clap/clap.h>
#include <melodee_screen.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {
std::atomic<int> wrongThread {0}, mainCallbacks {0}, inits {0}, presetReads {0};
std::string libraryPath;
// Built with MELODEE_TEST_SEQUENCER (tests/transport_freeze.cpp), Sine also plays by itself while it hears the song
// play, as a plug-in with a sequencer of its own does: a smooth 11 ms bump on every sixteenth, placed by the song's
// place alone.
#ifdef MELODEE_TEST_SEQUENCER
constexpr bool playsItself = true;
#else
constexpr bool playsItself = false;
#endif

struct Plugin {
    clap_plugin plugin {};
    const clap_host* host = nullptr;
    const clap_host_thread_check* threads = nullptr;
    bool instrument = false;
    double rate = 44100, level = 0.5, wave = 0, gain = 1;
    std::map<int, double> phases; // key: phase of its sine
    std::atomic<uint32_t> tailFrames {0};
    int outLevel = -1;            // a Level to report as an output event, 0..127, -1 none
    int drawnBar = -1;            // its screen's bar as last drawn, pixels

    void main() { if (threads && !threads->is_main_thread(host)) ++wrongThread; }
    void audio() { if (threads && !threads->is_audio_thread(host)) ++wrongThread; }
};
Plugin& of(const clap_plugin* p) { return *static_cast<Plugin*>(p->plugin_data); }

const char* sineFeatures[] {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};
const char* gainFeatures[] {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor sineDescriptor {CLAP_VERSION_INIT, "org.melodee.test.sine", "Test Sine", "Melodee", "", "", "", "1",
                                            "", sineFeatures};
const clap_plugin_descriptor gainDescriptor {CLAP_VERSION_INIT, "org.melodee.test.gain", "Test Gain", "Melodee", "", "", "", "1",
                                            "", gainFeatures};

// Parameters: Sine's Level (id 7, module "Voice/Amp") and Wave (id 9, stepped 0-2, no module); Gain's Gain (id 3,
// 0-2, module "Out"). Sine has one remote-controls page, Main: Wave, Level, the rest of its eight unused; its
// on_main_thread says its pages changed.
uint32_t paramCount(const clap_plugin* p) { of(p).main(); return of(p).instrument ? 2 : 1; }
bool paramInfo(const clap_plugin* p, uint32_t i, clap_param_info* info) {
    of(p).main();
    *info = {};
    if (of(p).instrument && i == 0) { info->id = 7; std::strcpy(info->name, "Level"); std::strcpy(info->module, "Voice/Amp/"); info->max_value = 1; info->default_value = 0.5; }
    else if (of(p).instrument && i == 1) { info->id = 9; std::strcpy(info->name, "Wave"); info->max_value = 2; info->flags = CLAP_PARAM_IS_STEPPED; }
    else if (!of(p).instrument && i == 0) { info->id = 3; std::strcpy(info->name, "Gain"); std::strcpy(info->module, "Out"); info->max_value = 2; info->default_value = 1; }
    else return false;
    info->flags |= CLAP_PARAM_IS_AUTOMATABLE;
    return true;
}
double* field(Plugin& s, clap_id id) { return id == 7 ? &s.level : id == 9 ? &s.wave : id == 3 ? &s.gain : nullptr; }
bool paramValue(const clap_plugin* p, clap_id id, double* v) {
    of(p).main();
    auto* f = field(of(p), id);
    if (!f) return false;
    *v = *f;
    return true;
}
bool valueText(const clap_plugin* p, clap_id id, double v, char* out, uint32_t size) {
    of(p).main();
    if (id == 7) std::snprintf(out, size, "%d %%", int(std::lround(v * 100)));
    else std::snprintf(out, size, "%.2f", v);
    return true;
}
bool textValue(const clap_plugin* p, clap_id, const char* text, double* v) { of(p).main(); *v = std::atof(text); return true; }
void flush(const clap_plugin*, const clap_input_events*, const clap_output_events*) {}
const clap_plugin_params params {paramCount, paramInfo, paramValue, valueText, textValue, flush};

// State: Level, Wave and Gain as three doubles.
bool save(const clap_plugin* p, const clap_ostream* out) {
    of(p).main();
    double v[3] {of(p).level, of(p).wave, of(p).gain};
    return out->write(out, v, sizeof(v)) == int64_t(sizeof(v));
}
bool load(const clap_plugin* p, const clap_istream* in) {
    of(p).main();
    double v[3] {};
    if (in->read(in, v, sizeof(v)) != int64_t(sizeof(v))) return false;
    of(p).level = v[0]; of(p).wave = v[1]; of(p).gain = v[2];
    return true;
}
const clap_plugin_state state {save, load};

uint32_t audioCount(const clap_plugin* p, bool input) { return input && of(p).instrument ? 0 : 1; }
bool audioInfo(const clap_plugin* p, uint32_t i, bool input, clap_audio_port_info* info) {
    if (i != 0 || (input && of(p).instrument)) return false;
    *info = {};
    info->id = input ? 1 : 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports audioPorts {audioCount, audioInfo};

uint32_t noteCount(const clap_plugin* p, bool input) { return input && of(p).instrument ? 1 : 0; }
bool noteInfo(const clap_plugin*, uint32_t, bool, clap_note_port_info* info) {
    *info = {};
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    return true;
}
const clap_plugin_note_ports notePorts {noteCount, noteInfo};

uint32_t latency(const clap_plugin* p) { return of(p).instrument ? 0 : 32; }
const clap_plugin_latency latencyExt {latency};

struct PresetLine { std::string name; double value = 0; std::string forWhom; };
std::vector<PresetLine> presetFile(const char* path) {
    std::vector<PresetLine> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        PresetLine p;
        p.name = line.substr(0, tab);
        auto rest = line.substr(tab + 1);
        auto second = rest.find('\t');
        p.value = std::atof(rest.substr(0, second).c_str());
        if (second != std::string::npos) p.forWhom = rest.substr(second + 1);
        out.push_back(p);
    }
    return out;
}
std::string besideLibrary(const char* folder) { return libraryPath.substr(0, libraryPath.rfind('/') + 1) + folder; }
bool isFolder(const std::string& path) { struct stat st {}; return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

bool fromLocation(const clap_plugin* p, uint32_t kind, const char* location, const char* key) {
    auto& s = of(p);
    s.main();
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || location == nullptr) return false;
    auto lines = presetFile(location);
    size_t n = key != nullptr ? std::strtoul(key, nullptr, 10) : 0;
    if (n >= lines.size()) return false;
    (s.instrument ? s.level : s.gain) = lines[n].value;
    if (auto* told = static_cast<const clap_host_preset_load*>(s.host->get_extension(s.host, CLAP_EXT_PRESET_LOAD)))
        told->loaded(s.host, kind, location, key);
    return true;
}
const clap_plugin_preset_load presetLoad {fromLocation};

// One provider for both plug-ins: TestPresets ("Test Sine Factory") and TestUser ("User"), where they are.
const clap_preset_discovery_provider_descriptor providerDescriptor {CLAP_VERSION_INIT, "org.melodee.test.presets", "Test Presets", "Melodee"};
struct Provider {
    clap_preset_discovery_provider provider {};
    const clap_preset_discovery_indexer* indexer = nullptr;
};
bool providerInit(const clap_preset_discovery_provider* p) {
    auto* indexer = static_cast<Provider*>(p->provider_data)->indexer;
    clap_preset_discovery_filetype type {"Test preset", "", "tsp"};
    if (!indexer->declare_filetype(indexer, &type)) return false;
    for (auto [name, folder] : {std::pair {"Test Sine Factory", "TestPresets"}, std::pair {"User", "TestUser"}}) {
        auto path = besideLibrary(folder);
        if (!isFolder(path)) continue;
        clap_preset_discovery_location location {CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, name, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.c_str()};
        if (!indexer->declare_location(indexer, &location)) return false;
    }
    return true;
}
bool providerMetadata(const clap_preset_discovery_provider*, uint32_t kind, const char* location,
                      const clap_preset_discovery_metadata_receiver* r) {
    ++presetReads;
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || location == nullptr) return false;
    auto lines = presetFile(location);
    if (lines.empty()) { r->on_error(r, 0, "Not a test preset"); return false; }
    for (size_t n = 0; n < lines.size(); ++n) {
        auto key = std::to_string(n);
        if (!r->begin_preset(r, lines[n].name.c_str(), lines.size() > 1 ? key.c_str() : nullptr)) return false;
        if (lines[n].forWhom == "-") continue;
        clap_universal_plugin_id id {"clap", lines[n].forWhom == "gain" ? "org.melodee.test.gain" : "org.melodee.test.sine"};
        r->add_plugin_id(r, &id);
    }
    return true;
}
uint32_t providerCount(const clap_preset_discovery_factory*) { return 1; }
const clap_preset_discovery_provider_descriptor* providerAt(const clap_preset_discovery_factory*, uint32_t i) {
    return i == 0 ? &providerDescriptor : nullptr;
}
const clap_preset_discovery_provider* makeProvider(const clap_preset_discovery_factory*, const clap_preset_discovery_indexer* indexer,
                                                   const char* id) {
    if (std::strcmp(id, providerDescriptor.id)) return nullptr;
    auto* s = new Provider;
    s->indexer = indexer;
    s->provider.desc = &providerDescriptor;
    s->provider.provider_data = s;
    s->provider.init = providerInit;
    s->provider.destroy = [](const clap_preset_discovery_provider* p) { delete static_cast<Provider*>(p->provider_data); };
    s->provider.get_metadata = providerMetadata;
    s->provider.get_extension = [](const clap_preset_discovery_provider*, const char*) -> const void* { return nullptr; };
    return &s->provider;
}
const clap_preset_discovery_factory presetFactory {providerCount, providerAt, makeProvider};

void event(Plugin& s, const clap_event_header* e) {
    if (e->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    if (e->type == CLAP_EVENT_PARAM_VALUE) {
        auto& v = *reinterpret_cast<const clap_event_param_value*>(e);
        if (auto* f = field(s, v.param_id)) *f = v.value;
    } else if (e->type == CLAP_EVENT_NOTE_ON) {
        s.phases[reinterpret_cast<const clap_event_note*>(e)->key] = 0;
    } else if (e->type == CLAP_EVENT_NOTE_OFF) {
        s.phases.erase(reinterpret_cast<const clap_event_note*>(e)->key);
    } else if (e->type == CLAP_EVENT_NOTE_CHOKE) {
        auto& n = *reinterpret_cast<const clap_event_note*>(e);
        if (n.key < 0) s.phases.clear(); else s.phases.erase(n.key);
    } else if (e->type == CLAP_EVENT_MIDI) {
        auto& m = *reinterpret_cast<const clap_event_midi*>(e);
        if ((m.data[0] & 0xf0) == 0xb0 && m.data[1] == 1) { s.level = m.data[2] / 127.0; s.outLevel = m.data[2]; }
    }
}

clap_process_status process(const clap_plugin* p, const clap_process* proc) {
    auto& s = of(p);
    s.audio();
    for (uint32_t i = 0, n = proc->in_events->size(proc->in_events); i < n; ++i) event(s, proc->in_events->get(proc->in_events, i));
    if (!s.instrument) {
        // The Loud preset gains a two-second tail, applied on the audio thread.
        const uint32_t frames = s.gain > 1 ? uint32_t(2 * s.rate) : 0;
        if (frames != s.tailFrames.load()) {
            s.tailFrames.store(frames);
            if (auto* tail = static_cast<const clap_host_tail*>(s.host->get_extension(s.host, CLAP_EXT_TAIL)))
                tail->changed(s.host);
        }
    }
    if (s.outLevel >= 0) {
        clap_event_param_value v {};
        v.header = {sizeof(v), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        v.param_id = 7; v.note_id = -1; v.port_index = -1; v.channel = -1; v.key = -1; v.value = s.level;
        proc->out_events->try_push(proc->out_events, &v.header);
        s.outLevel = -1;
    }
    float** out = proc->audio_outputs[0].data32;
    const auto* t = proc->transport;
    const uint32_t needs = CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TEMPO;
    const bool rolling = playsItself && s.instrument && t && (t->flags & needs) == needs && t->tempo > 0;
    const double beat = rolling ? double(t->song_pos_beats) / CLAP_BEATTIME_FACTOR : 0, perFrame = rolling ? t->tempo / 60.0 / s.rate : 0;
    for (uint32_t f = 0; f < proc->frames_count; ++f) {
        float x = 0;
        if (s.instrument) {
            for (auto& [key, phase] : s.phases) {
                x += float(s.level * std::sin(phase));
                phase += 2 * M_PI * 440.0 * std::pow(2.0, (key - 69) / 12.0) / s.rate;
            }
            if (rolling) {
                const double sixteenths = (beat + f * perFrame) * 4;
                const double since = (sixteenths - std::floor(sixteenths)) / (4 * perFrame); // frames into this one
                if (since < 480) { const double bump = std::sin(M_PI * since / 480); x += float(0.25 * bump * bump); }
            }
            out[0][f] = out[1][f] = x;
        } else {
            float** in = proc->audio_inputs[0].data32;
            out[0][f] = float(in[0][f] * s.gain);
            out[1][f] = float(in[1][f] * s.gain);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

uint32_t remoteCount(const clap_plugin* p) { of(p).main(); return 1; }
bool remotePage(const clap_plugin* p, uint32_t i, clap_remote_controls_page* page) {
    of(p).main();
    if (i != 0) return false;
    *page = {};
    std::strcpy(page->section_name, "Sound");
    std::strcpy(page->page_name, "Main");
    page->page_id = 1;
    for (auto& id : page->param_ids) id = CLAP_INVALID_ID;
    page->param_ids[0] = 9;
    page->param_ids[1] = 7;
    return true;
}
const clap_plugin_remote_controls remoteControls {remoteCount, remotePage};

// Its own screen: the bar; the wheel turns Level (Gain: Gain) a hundredth of its range a detent (it asks for a draw
// and a block, and says its values moved), Push steps Wave, Minus / Plus are not its; Sine's K1 Level, K2 Wave, K3
// nothing; Gain's K1 Gain.
bool screenDraw(const void* p, const melodee_screen_frame_t* frame) {
    auto& s = of(static_cast<const clap_plugin*>(p));
    s.main();
    int bar = int(std::lround((s.instrument ? s.level : s.gain / 2) * frame->width));
    if (!frame->redraw_all && bar == s.drawnBar) return false;
    for (uint32_t y = 0; y < frame->height; ++y)
        for (uint32_t x = 0; x < frame->width; ++x)
            frame->pixels[y * frame->stride + x] = y < 10 && int(x) < bar ? frame->colour : 0x101010u;
    s.drawnBar = bar;
    return true;
}
bool screenInput(const void* p, const melodee_screen_input_t* in) {
    auto& s = of(static_cast<const clap_plugin*>(p));
    s.main();
    if (in->kind == MELODEE_SCREEN_WHEEL && s.instrument) s.level = std::clamp(s.level + in->value * 0.01, 0.0, 1.0);
    else if (in->kind == MELODEE_SCREEN_WHEEL) s.gain = std::clamp(s.gain + in->value * 0.02, 0.0, 2.0);
    else if (in->kind == MELODEE_SCREEN_PUSH && s.instrument) s.wave = std::fmod(s.wave + 1, 3);
    else if (in->kind == MELODEE_SCREEN_ERASE && s.instrument) s.level = 0;
    else return false;
    if (s.instrument) // Push: its page takes Erase from now on; Erase (Level to 0): no longer
        if (auto* told = static_cast<const melodee_screen_host_2_t*>(s.host->get_extension(s.host, MELODEE_SCREEN_CLAP_HOST_2)))
            told->keys(s.host, in->kind == MELODEE_SCREEN_PUSH ? uint32_t(MELODEE_SCREEN_KEY_ERASE) : 0u);
    if (auto* told = static_cast<const melodee_screen_host_t*>(s.host->get_extension(s.host, MELODEE_SCREEN_CLAP_HOST)))
        told->redraw(s.host, MELODEE_SCREEN_RUN);
    if (auto* hp = static_cast<const clap_host_params*>(s.host->get_extension(s.host, CLAP_EXT_PARAMS)))
        hp->rescan(s.host, CLAP_PARAM_RESCAN_VALUES);
    return true;
}
void screenControls(const void* p, uint32_t ids[3]) {
    auto& s = of(static_cast<const clap_plugin*>(p));
    s.main();
    ids[0] = s.instrument ? 7 : 3;
    ids[1] = s.instrument ? 9 : MELODEE_SCREEN_NO_CONTROL;
    ids[2] = MELODEE_SCREEN_NO_CONTROL;
}
const melodee_screen_t screen {screenDraw, screenInput, screenControls};

const void* extension(const clap_plugin* p, const char* id) {
    if (!std::strcmp(id, MELODEE_SCREEN_CLAP)) return &screen;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notePorts;
    static const clap_plugin_tail tail {[](const clap_plugin* plug) { return of(plug).tailFrames.load(); }};
    if (!std::strcmp(id, CLAP_EXT_TAIL) && !of(p).instrument) return &tail;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latencyExt;
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD)) return &presetLoad;
    if (!std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS) && of(p).instrument) return &remoteControls;
    return nullptr;
}

const clap_plugin* create(const clap_plugin_factory*, const clap_host* host, const char* id) {
    bool instrument = !std::strcmp(id, sineDescriptor.id);
    if (!instrument && std::strcmp(id, gainDescriptor.id)) return nullptr;
    auto* s = new Plugin;
    s->host = host;
    s->instrument = instrument;
    auto& p = s->plugin;
    p.desc = instrument ? &sineDescriptor : &gainDescriptor;
    p.plugin_data = s;
    p.init = [](const clap_plugin* p) {
        auto& s = of(p);
        s.threads = static_cast<const clap_host_thread_check*>(s.host->get_extension(s.host, CLAP_EXT_THREAD_CHECK));
        s.main();
        s.host->request_callback(s.host);
        return true;
    };
    p.destroy = [](const clap_plugin* p) { of(p).main(); delete &of(p); };
    p.activate = [](const clap_plugin* p, double rate, uint32_t, uint32_t) { of(p).main(); of(p).rate = rate; return true; };
    p.deactivate = [](const clap_plugin* p) { of(p).main(); };
    p.start_processing = [](const clap_plugin* p) { of(p).audio(); return true; };
    p.stop_processing = [](const clap_plugin* p) { of(p).audio(); };
    p.reset = [](const clap_plugin* p) { of(p).phases.clear(); };
    p.process = process;
    p.get_extension = extension;
    // Then asks to be run, as a plug-in does when its main thread has something for its audio side.
    p.on_main_thread = [](const clap_plugin* p) {
        auto& s = of(p);
        s.main();
        ++mainCallbacks;
        s.host->request_process(s.host);
        if (auto* remote = static_cast<const clap_host_remote_controls*>(s.host->get_extension(s.host, CLAP_EXT_REMOTE_CONTROLS)))
            remote->changed(s.host);
    };
    return &p;
}

uint32_t count(const clap_plugin_factory*) { return 2; }
const clap_plugin_descriptor* descriptor(const clap_plugin_factory*, uint32_t i) {
    return i == 0 ? &sineDescriptor : i == 1 ? &gainDescriptor : nullptr;
}
const clap_plugin_factory factory {count, descriptor, create};
} // namespace

extern "C" {
const clap_plugin_entry clap_entry { // exported by its declaration in clap/entry.h
    CLAP_VERSION_INIT,
    [](const char* path) { libraryPath = path; ++inits; return true; },
    [] { --inits; },
    [](const char* id) -> const void* {
        if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &factory;
        if (!std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID)) return &presetFactory;
        return nullptr;
    },
};
// Calls on the wrong thread, on_main_thread calls, and init() calls not yet matched by deinit().
CLAP_EXPORT void fixture_counts(int* wrong, int* callbacks, int* initialised) {
    *wrong = wrongThread; *callbacks = mainCallbacks; *initialised = inits;
}
// Preset files read through get_metadata().
CLAP_EXPORT int fixture_preset_reads() { return presetReads; }
}
