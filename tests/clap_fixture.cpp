// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Two small CLAP plug-ins in one library, for tests/clap_host.cpp: Test Sine
// (an instrument: a sine per note, Level and Wave parameters, CC 1 sets Level
// and says so as an output event) and Test Gain (an effect). Each counts the
// calls it got on the wrong thread, by the host's thread-check, and
// fixture_counts() reports them.
#include <clap/clap.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {
std::atomic<int> wrongThread {0}, mainCallbacks {0}, inits {0};

struct Plugin {
    clap_plugin plugin {};
    const clap_host* host = nullptr;
    const clap_host_thread_check* threads = nullptr;
    bool instrument = false;
    double rate = 44100, level = 0.5, wave = 0, gain = 1;
    std::map<int, double> phases; // key: phase of its sine
    int outLevel = -1;            // a Level to report as an output event, 0..127, -1 none

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

// Parameters: Sine's Level (id 7) and Wave (id 9, stepped 0-2); Gain's Gain (id 3, 0-2).
uint32_t paramCount(const clap_plugin* p) { of(p).main(); return of(p).instrument ? 2 : 1; }
bool paramInfo(const clap_plugin* p, uint32_t i, clap_param_info* info) {
    of(p).main();
    *info = {};
    if (of(p).instrument && i == 0) { info->id = 7; std::strcpy(info->name, "Level"); info->max_value = 1; info->default_value = 0.5; }
    else if (of(p).instrument && i == 1) { info->id = 9; std::strcpy(info->name, "Wave"); info->max_value = 2; info->flags = CLAP_PARAM_IS_STEPPED; }
    else if (!of(p).instrument && i == 0) { info->id = 3; std::strcpy(info->name, "Gain"); info->max_value = 2; info->default_value = 1; }
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
    if (s.outLevel >= 0) {
        clap_event_param_value v {};
        v.header = {sizeof(v), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        v.param_id = 7; v.note_id = -1; v.port_index = -1; v.channel = -1; v.key = -1; v.value = s.level;
        proc->out_events->try_push(proc->out_events, &v.header);
        s.outLevel = -1;
    }
    float** out = proc->audio_outputs[0].data32;
    for (uint32_t f = 0; f < proc->frames_count; ++f) {
        float x = 0;
        if (s.instrument) {
            for (auto& [key, phase] : s.phases) {
                x += float(s.level * std::sin(phase));
                phase += 2 * M_PI * 440.0 * std::pow(2.0, (key - 69) / 12.0) / s.rate;
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

const void* extension(const clap_plugin*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notePorts;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latencyExt;
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
    p.on_main_thread = [](const clap_plugin* p) { of(p).main(); ++mainCallbacks; };
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
    [](const char*) { ++inits; return true; },
    [] { --inits; },
    [](const char* id) -> const void* { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &factory; },
};
// Calls on the wrong thread, on_main_thread calls, and init() calls not yet matched by deinit().
CLAP_EXPORT void fixture_counts(int* wrong, int* callbacks, int* initialised) {
    *wrong = wrongThread; *callbacks = mainCallbacks; *initialised = inits;
}
}
