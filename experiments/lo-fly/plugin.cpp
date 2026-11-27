// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
//
// lo-fly as a CLAP instrument without JUCE, for Melodee on the MPC Sample: lo-fly's own engine (lofly::Synth, from
// the lo-fly repository's dsp/), every lo-fly parameter as a CLAP parameter, its state, and its panel on the MPC's
// screen through Melodee's screen extension (panel.h). In any other host it is the same instrument without an editor.
//
//   parameters   all of lofly::kParamSpecs; ID = FNV-1a of the key (store.h), values plain, text from lo-fly's own
//                format; module = the section
//   notes        CLAP notes (preferred) and MIDI: notes, bend, pressure, CC 1, sustain, other CCs as lo-fly controllers
//   state        text records: every parameter by key, the modulation settings, Step Gate patterns, content file paths,
//                the preset's name
//   presets      CLAP preset discovery and preset-load: lo-fly's factory presets (inside the plug-in, "Factory") and
//                .loflypreset files ("User": the Presets folder beside the plug-in; on a Mac also lo-fly's own folder)
//   pages        CLAP remote controls for Melodee's EDIT pages
#include "panel.h"
#include "store.h"

#include "lofly/audio_file.h"
#include "lofly/builtin_noise.h"
#include "lofly/fx_rack.h"
#include "lofly/modulation.h"
#include "lofly/param_format.h"
#include "lofly/presets.h"
#include "lofly/sfz.h"
#include "lofly/synth.h"
#include "lofly/wavetable.h"

#include <clap/clap.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lofly_melodee {
namespace {

using lofly::ParamId;

// --- parameter IDs ---

struct IdEntry {
    std::uint32_t id;
    int index;
};
const std::vector<IdEntry>& id_table()
{
    static const auto table = [] {
        std::vector<IdEntry> t;
        t.reserve(lofly::kNumParams);
        for (const auto& s : lofly::kParamSpecs)
            t.push_back({ clap_id(s.key), static_cast<int>(s.id) });
        std::sort(t.begin(), t.end(), [](const IdEntry& a, const IdEntry& b) { return a.id < b.id; });
        return t;
    }();
    return table;
}
std::optional<ParamId> param_of(::clap_id id)
{
    const auto& t = id_table();
    const auto it = std::lower_bound(t.begin(), t.end(), id, [](const IdEntry& e, std::uint32_t v) { return e.id < v; });
    if (it == t.end() || it->id != id)
        return std::nullopt;
    return static_cast<ParamId>(it->index);
}

// The section a parameter belongs to, from its key: "lofi.drive" -> "LoFi", "eng1.smp.a.root" -> "Engine 1/Slot A".
std::string module_of(std::string_view key)
{
    const auto head = key.substr(0, key.find('.'));
    auto numbered = [&](std::string_view prefix, const char* name) -> std::optional<std::string> {
        if (head.size() == prefix.size() + 1 && head.substr(0, prefix.size()) == prefix && head.back() >= '1' && head.back() <= '9')
            return std::string(name) + " " + head.back();
        return std::nullopt;
    };
    if (head == "master" || head == "voice")
        return "Voice";
    if (head == "lofi")
        return "LoFi";
    if (head == "vib")
        return "Vibrato";
    if (head == "noise")
        return "Noise";
    if (head == "filter")
        return "Filter";
    if (head == "fenv")
        return "Filter Env";
    if (head == "amp")
        return "Amp Env";
    if (head == "macro")
        return "Macros";
    if (head == "mpe")
        return "MPE";
    if (head == "fx")
        return "Effects";
    if (auto m = numbered("fx", "FX"))
        return *m;
    if (auto m = numbered("mod", "Mod"))
        return *m;
    if (auto m = numbered("eng", "Engine")) {
        if (std::count(key.begin(), key.end(), '.') >= 3 && key.substr(head.size(), 5) == ".smp.") {
            const char slot = static_cast<char>(std::toupper(static_cast<unsigned char>(key[head.size() + 5])));
            return *m + "/Slot " + slot;
        }
        return *m;
    }
    return "";
}

// --- the plug-in ---

struct Plugin {
    clap_plugin plugin {};
    const clap_host* host = nullptr;
    const clap_host_params* host_params = nullptr;
    const melodee_screen_host_t* screen_host = nullptr;
    const melodee_screen_host_2_t* screen_host_2 = nullptr; // Melodee that lets a page take MUTE and ERASE
    std::uint32_t keys_told = 0;                            // the keys it was told last
    const clap_host_preset_load* host_preset_load = nullptr;

    Store store;
    std::unique_ptr<lofly::Synth> synth = std::make_unique<lofly::Synth>();
    std::unique_ptr<Panel> panel;

    // Audio thread.
    std::array<float, lofly::kNumParams> applied {};
    std::vector<lofly::Event> events;
    std::vector<float> spare; // the right channel when the host gives one output channel
    double rate = 48000.0;
    std::uint32_t since_redraw = 0;
    std::uint32_t drawn_voices = 0;
    bool was_loud = false;
    std::atomic<bool> values_moved { false };
    std::atomic<bool> jump { false }; // a whole new sound (state, preset): every value at once, no glides

    // Main thread: what the engines play from files, the modulation settings and gate patterns.
    std::mutex content_lock;
    std::array<lofly::EngineContent, lofly::kNumEngines> content {};
    lofly::ModSettings mod {};
    lofly::FxPatterns patterns {};

    static constexpr std::size_t max_events = 2048;

    void apply_all()
    {
        for (const auto& s : lofly::kParamSpecs) {
            const float v = store.get(s.id);
            synth->setParam(s.id, v);
            applied[static_cast<std::size_t>(s.id)] = v;
        }
    }
};

Plugin& of(const clap_plugin* p) { return *static_cast<Plugin*>(p->plugin_data); }
Plugin& of(const void* p) { return of(static_cast<const clap_plugin*>(p)); }

const char* const features[] { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };
const clap_plugin_descriptor descriptor { CLAP_VERSION_INIT, "com.ellicstudio.lofly.melodee", "lo-fly", "Ellic Studio",
    "https://ellicstudio.com", "", "", "0.1.0", "A polyphonic lo-fi instrument, with its panel on the MPC Sample in Melodee", features };

// --- events ---

void param_event(Plugin& s, const clap_event_header* h)
{
    const auto* ev = reinterpret_cast<const clap_event_param_value*>(h);
    if (const auto id = param_of(ev->param_id)) {
        s.store.set(*id, static_cast<float>(ev->value));
        s.values_moved.store(true, std::memory_order_relaxed);
    }
}

void midi_event(Plugin& s, std::uint32_t at, const std::uint8_t data[3])
{
    if (s.events.size() + 4 > Plugin::max_events)
        return;
    const std::uint8_t kind = data[0] & 0xf0, ch = data[0] & 0x0f, d1 = data[1] & 0x7f, d2 = data[2] & 0x7f;
    using E = lofly::Event;
    switch (kind) {
    case 0x90:
        s.events.push_back(d2 > 0 ? E::noteOn(at, d1, static_cast<float>(d2) / 127.0f, ch) : E::noteOff(at, d1, 0.0f, ch));
        break;
    case 0x80: s.events.push_back(E::noteOff(at, d1, static_cast<float>(d2) / 127.0f, ch)); break;
    case 0xe0: {
        const int bend = (d2 << 7 | d1) - 8192;
        s.events.push_back(E::pitchBend(at, std::clamp(static_cast<float>(bend) / 8191.0f, -1.0f, 1.0f), ch));
        break;
    }
    case 0xd0: s.events.push_back(E::aftertouch(at, static_cast<float>(d1) / 127.0f, ch)); break;
    case 0xb0:
        if (d1 == 1)
            s.events.push_back(E::modWheel(at, static_cast<float>(d2) / 127.0f));
        else if (d1 == 64)
            s.events.push_back(E::sustainPedal(at, d2 >= 64));
        else if (d1 == 120)
            s.events.push_back(E::allSoundOff(at));
        else if (d1 == 123)
            s.events.push_back(E::allNotesOff(at));
        else if (d1 == 121) {
            s.events.push_back(E::pitchBend(at, 0.0f, ch));
            s.events.push_back(E::modWheel(at, 0.0f));
            s.events.push_back(E::aftertouch(at, 0.0f, ch));
            s.events.push_back(E::sustainPedal(at, false));
        } else
            s.events.push_back(E::controller(at, d1, static_cast<float>(d2) / 127.0f, ch));
        break;
    default: break;
    }
}

void read_events(Plugin& s, const clap_input_events* in, bool notes)
{
    for (std::uint32_t i = 0, n = in ? in->size(in) : 0; i < n; ++i) {
        const auto* h = in->get(in, i);
        if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID)
            continue;
        if (h->type == CLAP_EVENT_PARAM_VALUE) {
            param_event(s, h);
            continue;
        }
        if (!notes || s.events.size() >= Plugin::max_events)
            continue;
        switch (h->type) {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: {
            const auto* ev = reinterpret_cast<const clap_event_note*>(h);
            const auto ch = static_cast<std::uint8_t>(ev->channel >= 0 && ev->channel < 16 ? ev->channel : 0);
            if (ev->key < 0 || ev->key > 127) { // every key
                if (h->type != CLAP_EVENT_NOTE_ON)
                    s.events.push_back(h->type == CLAP_EVENT_NOTE_CHOKE ? lofly::Event::allSoundOff(h->time) : lofly::Event::allNotesOff(h->time));
                break;
            }
            const auto key = static_cast<std::uint8_t>(ev->key);
            if (h->type == CLAP_EVENT_NOTE_ON)
                s.events.push_back(lofly::Event::noteOn(h->time, key, static_cast<float>(std::clamp(ev->velocity, 0.0, 1.0)), ch));
            else
                s.events.push_back(lofly::Event::noteOff(h->time, key, static_cast<float>(std::clamp(ev->velocity, 0.0, 1.0)), ch));
            break;
        }
        case CLAP_EVENT_MIDI: midi_event(s, h->time, reinterpret_cast<const clap_event_midi*>(h)->data); break;
        default: break;
        }
    }
}

// --- process ---

clap_process_status process(const clap_plugin* p, const clap_process* pr)
{
    auto& s = of(p);
    if (pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count < 1)
        return CLAP_PROCESS_ERROR;
    if (const auto* t = pr->transport) {
        const double bpm = (t->flags & CLAP_TRANSPORT_HAS_TEMPO) ? t->tempo : 120.0;
        const double ppq = (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) ? static_cast<double>(t->song_pos_beats) / CLAP_BEATTIME_FACTOR : 0.0;
        const bool playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        s.synth->setTransport(bpm, ppq, playing);
        s.store.playing.store(playing, std::memory_order_relaxed);
    }
    s.events.clear();
    read_events(s, pr->in_events, true);
    for (std::size_t i = 0; i < s.applied.size(); ++i) {
        const float v = s.store.values[i].load(std::memory_order_relaxed);
        if (v != s.applied[i]) {
            s.synth->setParam(static_cast<ParamId>(i), v);
            s.applied[i] = v;
        }
    }
    if (s.jump.exchange(false, std::memory_order_acquire))
        s.synth->jumpToTargets();
    // Events past the block's end would never sound; lo-fly wants them sorted, as CLAP gives them.
    const auto frames = pr->frames_count;
    for (auto& e : s.events)
        e.offset = std::min(e.offset, frames > 0 ? frames - 1 : 0);

    const auto& out = pr->audio_outputs[0];
    float* left = out.data32[0];
    float* right = out.channel_count > 1 ? out.data32[1] : nullptr;
    if (!right) { // a mono output: lo-fly still renders stereo
        if (s.spare.size() < frames)
            return CLAP_PROCESS_ERROR;
        right = s.spare.data();
    }
    s.synth->process(left, right, static_cast<int>(frames), s.events);
    for (std::uint32_t c = 2; c < out.channel_count; ++c)
        std::memset(out.data32[c], 0, sizeof(float) * frames);

    float peak = 0.0f;
    for (std::uint32_t f = 0; f < frames; ++f)
        peak = std::max(peak, std::max(std::fabs(left[f]), std::fabs(right[f])));
    std::uint32_t voices = 0;
    for (int v = 0; v < lofly::Synth::kVoicePool; ++v)
        if (s.synth->voice(v).isActive())
            voices |= 1u << v;
    s.store.peak.store(peak, std::memory_order_relaxed);
    s.store.voices.store(voices, std::memory_order_relaxed);

    // Ask the screen for a draw, at most every 40 ms: when values moved, and on the pages that move with the sound.
    s.since_redraw += frames;
    if (s.screen_host && s.since_redraw >= static_cast<std::uint32_t>(s.rate * 0.04)) {
        const bool loud = peak > 1e-4f || voices != 0;
        const int page = s.store.page.load(std::memory_order_relaxed);
        const bool live_page = page == 0 || page == Panel::page_count() - 1;
        const bool moved = s.values_moved.exchange(false, std::memory_order_relaxed);
        if (moved || (live_page && (loud || s.was_loud || voices != s.drawn_voices))) {
            s.screen_host->redraw(s.host, 0);
            s.since_redraw = 0;
            s.drawn_voices = voices;
        }
        s.was_loud = loud;
    }
    return voices != 0 ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_CONTINUE_IF_NOT_QUIET;
}

// --- params ---

uint32_t params_count(const clap_plugin*) { return static_cast<uint32_t>(lofly::kNumParams); }
bool params_info(const clap_plugin*, uint32_t index, clap_param_info* info)
{
    if (index >= static_cast<uint32_t>(lofly::kNumParams))
        return false;
    const auto& s = lofly::kParamSpecs[index];
    *info = {};
    info->id = clap_id(s.key);
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (lofly::isStepped(s.kind))
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (s.kind == lofly::ParamKind::Choice)
        info->flags |= CLAP_PARAM_IS_ENUM;
    std::snprintf(info->name, sizeof(info->name), "%.*s", static_cast<int>(s.name.size()), s.name.data());
    std::snprintf(info->module, sizeof(info->module), "%s", module_of(s.key).c_str());
    info->min_value = s.min;
    info->max_value = s.max;
    info->default_value = s.def;
    return true;
}
bool params_value(const clap_plugin* p, ::clap_id id, double* out)
{
    const auto pid = param_of(id);
    if (!pid)
        return false;
    *out = of(p).store.get(*pid);
    return true;
}
bool params_value_to_text(const clap_plugin*, ::clap_id id, double value, char* out, uint32_t size)
{
    const auto pid = param_of(id);
    if (!pid || !out || size == 0)
        return false;
    const std::string text = lofly::formatDisplayValue(spec_of(*pid), static_cast<float>(value));
    std::snprintf(out, size, "%s", text.c_str());
    return true;
}
bool params_text_to_value(const clap_plugin*, ::clap_id id, const char* text, double* out)
{
    const auto pid = param_of(id);
    if (!pid || !text)
        return false;
    const auto v = lofly::parseDisplayValue(spec_of(*pid), text);
    if (!v)
        return false;
    *out = *v;
    return true;
}
void params_flush(const clap_plugin* p, const clap_input_events* in, const clap_output_events*) { read_events(of(p), in, false); }
const clap_plugin_params params_ext { params_count, params_info, params_value, params_value_to_text, params_text_to_value, params_flush };

// --- ports ---

uint32_t audio_count(const clap_plugin*, bool input) { return input ? 0 : 1; }
bool audio_get(const clap_plugin*, uint32_t index, bool input, clap_audio_port_info* info)
{
    if (input || index != 0)
        return false;
    *info = {};
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports audio_ext { audio_count, audio_get };

uint32_t note_count(const clap_plugin*, bool input) { return input ? 1 : 0; }
bool note_get(const clap_plugin*, uint32_t index, bool input, clap_note_port_info* info)
{
    if (!input || index != 0)
        return false;
    *info = {};
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "Notes");
    return true;
}
const clap_plugin_note_ports note_ext { note_count, note_get };

// --- state ---

void record(std::string& out, std::string_view name, std::string_view value)
{
    out.append(name);
    out += ' ';
    out += std::to_string(value.size());
    out += '\n';
    out.append(value);
    out += '\n';
}

bool state_save(const clap_plugin* p, const clap_ostream* stream)
{
    auto& s = of(p);
    std::string text = "lo-fly 1\n";
    char number[32];
    for (const auto& spec : lofly::kParamSpecs) {
        std::snprintf(number, sizeof(number), "%.9g", static_cast<double>(s.store.get(spec.id)));
        record(text, "p:" + std::string(spec.key), number);
    }
    {
        std::lock_guard<std::mutex> held(s.content_lock);
        record(text, "preset", s.store.preset());
        record(text, "modulation", lofly::serialize(s.mod));
        for (int g = 0; g < lofly::kFxSlots; ++g)
            record(text, "gate" + std::to_string(g + 1), lofly::patternToText(s.patterns.gates[static_cast<std::size_t>(g)]));
        for (int e = 0; e < lofly::kNumEngines; ++e) {
            const auto& c = s.content[static_cast<std::size_t>(e)];
            const std::string engine = "eng" + std::to_string(e + 1);
            record(text, engine + ".instrument", c.instrument ? c.instrument->path : "");
            for (int slot = 0; slot < lofly::kSamplerSlots; ++slot)
                record(text, engine + ".slot" + std::to_string(slot), c.slotPaths[static_cast<std::size_t>(slot)]);
        }
    }
    const char* at = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        const int64_t wrote = stream->write(stream, at, left);
        if (wrote <= 0)
            return false;
        at += wrote;
        left -= static_cast<std::size_t>(wrote);
    }
    return true;
}

bool state_load(const clap_plugin* p, const clap_istream* stream)
{
    auto& s = of(p);
    std::string text;
    char buffer[4096];
    for (;;) {
        const int64_t got = stream->read(stream, buffer, sizeof(buffer));
        if (got < 0)
            return false;
        if (got == 0)
            break;
        text.append(buffer, static_cast<std::size_t>(got));
    }
    if (text.rfind("lo-fly 1\n", 0) != 0)
        return false;
    std::vector<std::pair<std::string, std::string>> records;
    std::size_t at = 9;
    while (at < text.size()) {
        const auto space = text.find(' ', at), line = text.find('\n', at);
        if (space == std::string::npos || line == std::string::npos || space > line)
            return false;
        const std::size_t size = std::strtoull(text.c_str() + space + 1, nullptr, 10);
        if (line + 1 + size > text.size())
            return false;
        records.emplace_back(text.substr(at, space - at), text.substr(line + 1, size));
        at = line + 1 + size + 1;
    }
    // Parameters missing from the state (added to lo-fly since) take their defaults.
    for (const auto& spec : lofly::kParamSpecs)
        s.store.set(spec.id, spec.def);
    s.store.set_preset("Init");
    lofly::ModSettings mod {};
    lofly::FxPatterns patterns {};
    std::array<std::string, lofly::kNumEngines> instruments {};
    std::array<std::array<std::string, lofly::kSamplerSlots>, lofly::kNumEngines> slots {};
    for (const auto& [name, value] : records) {
        if (name.rfind("p:", 0) == 0) {
            if (const auto id = lofly::findParam(std::string_view(name).substr(2)))
                s.store.set(*id, std::strtof(value.c_str(), nullptr));
        } else if (name == "preset")
            s.store.set_preset(value);
        else if (name == "modulation")
            mod = lofly::parseModSettings(value).settings;
        else if (name.size() == 5 && name.rfind("gate", 0) == 0 && name[4] >= '1' && name[4] <= '4') {
            auto& gate = patterns.gates[static_cast<std::size_t>(name[4] - '1')];
            if (!lofly::patternFromText(value, gate))
                gate = lofly::defaultGatePattern();
        } else if (name.size() > 4 && name.rfind("eng", 0) == 0 && name[3] >= '1' && name[3] <= '2') {
            const auto e = static_cast<std::size_t>(name[3] - '1');
            const auto field = std::string_view(name).substr(4);
            if (field == ".instrument")
                instruments[e] = value;
            else if (field.size() == 6 && field.substr(0, 5) == ".slot" && field[5] >= '0' && field[5] < '0' + lofly::kSamplerSlots)
                slots[e][static_cast<std::size_t>(field[5] - '0')] = value;
        }
    }
    {
        std::lock_guard<std::mutex> held(s.content_lock);
        s.mod = mod;
        s.patterns = patterns;
        s.synth->setModSettings(s.mod);
        s.synth->setFxPatterns(s.patterns);
        for (int e = 0; e < lofly::kNumEngines; ++e) {
            auto& c = s.content[static_cast<std::size_t>(e)];
            c = {};
            const auto& path = instruments[static_cast<std::size_t>(e)];
            if (!path.empty())
                if (auto loaded = lofly::loadSfz(path); loaded.instrument)
                    c.instrument = std::move(loaded.instrument);
            for (int slot = 0; slot < lofly::kSamplerSlots; ++slot) {
                const auto& file = slots[static_cast<std::size_t>(e)][static_cast<std::size_t>(slot)];
                if (file.empty())
                    continue;
                if (auto loaded = lofly::readAudioFile(file); loaded.sample) {
                    c.slots[static_cast<std::size_t>(slot)] = std::make_shared<const lofly::SampleBuffer>(std::move(*loaded.sample));
                    c.slotPaths[static_cast<std::size_t>(slot)] = file;
                }
            }
            s.synth->setEngineContent(e, std::make_unique<lofly::EngineContent>(c));
        }
        s.synth->collectGarbage();
    }
    s.jump.store(true, std::memory_order_release);
    s.values_moved.store(true, std::memory_order_relaxed);
    if (s.host_params)
        s.host_params->rescan(s.host, CLAP_PARAM_RESCAN_VALUES);
    if (s.screen_host)
        s.screen_host->redraw(s.host, 0);
    return true;
}
const clap_plugin_state state_ext { state_save, state_load };

// --- presets ---

std::string& plugin_path() // the library's own path, from clap_entry.init()
{
    static std::string path;
    return path;
}

// The folders .loflypreset files are read from, those that exist.
std::vector<std::string> user_preset_folders()
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    auto add = [&](const fs::path& folder) {
        std::error_code e;
        if (fs::is_directory(folder, e))
            out.push_back(folder.string());
    };
    if (!plugin_path().empty()) // beside the plug-in: on the MPC, Melodee/Plugins/lo-fly/Presets
        add(fs::path(plugin_path()).parent_path() / "Presets");
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME")) // where lo-fly's own editor saves them
        add(fs::path(home) / "Library/Audio/Presets/Ellic Studio/lo-fly");
#endif
    return out;
}

std::optional<lofly::Preset> read_preset_file(const char* path, std::string* error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error)
            *error = "Cannot open the file";
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto parsed = lofly::parsePreset(text);
    if (!parsed.ok) {
        if (error)
            *error = "Not a lo-fly preset";
        return std::nullopt;
    }
    return std::move(parsed.preset);
}

// Main thread: the preset's values, modulation and gate patterns; the next block takes them at once.
void apply_preset(Plugin& s, const lofly::Preset& preset)
{
    for (const auto& spec : lofly::kParamSpecs)
        s.store.set(spec.id, preset.values[static_cast<std::size_t>(spec.id)]);
    {
        std::lock_guard<std::mutex> held(s.content_lock);
        s.mod = preset.modulation;
        s.patterns = preset.patterns;
        s.synth->setModSettings(s.mod);
        s.synth->setFxPatterns(s.patterns);
    }
    s.store.set_preset(preset.name);
    s.jump.store(true, std::memory_order_release);
    s.values_moved.store(true, std::memory_order_relaxed);
    if (s.host_params)
        s.host_params->rescan(s.host, CLAP_PARAM_RESCAN_VALUES);
    if (s.screen_host)
        s.screen_host->redraw(s.host, MELODEE_SCREEN_RUN);
    s.host->request_process(s.host);
}

bool preset_from_location(const clap_plugin* p, uint32_t kind, const char* location, const char* key)
{
    auto& s = of(p);
    std::optional<lofly::Preset> preset;
    std::string error = "No such preset";
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && key) {
        if (const auto* factory = lofly::findFactoryPreset(key))
            preset = *factory;
    } else if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location)
        preset = read_preset_file(location, &error);
    if (!preset) {
        if (s.host_preset_load)
            s.host_preset_load->on_error(s.host, kind, location, key, 0, error.c_str());
        return false;
    }
    apply_preset(s, *preset);
    if (s.host_preset_load)
        s.host_preset_load->loaded(s.host, kind, location, key);
    return true;
}
const clap_plugin_preset_load preset_load_ext { preset_from_location };

// Preset discovery: one provider, the factory presets inside the plug-in and the user's files.
const clap_preset_discovery_provider_descriptor provider_descriptor { CLAP_VERSION_INIT, "com.ellicstudio.lofly.melodee.presets",
    "lo-fly presets", "Ellic Studio" };
const clap_universal_plugin_id plugin_id { "clap", "com.ellicstudio.lofly.melodee" };

void describe(const clap_preset_discovery_metadata_receiver* r, const lofly::Preset& preset, uint32_t flags)
{
    r->add_plugin_id(r, &plugin_id);
    r->set_flags(r, flags);
    if (!preset.author.empty())
        r->add_creator(r, preset.author.c_str());
    if (!preset.comment.empty())
        r->set_description(r, preset.comment.c_str());
    if (!preset.category.empty())
        r->add_feature(r, preset.category.c_str());
}

struct Provider {
    clap_preset_discovery_provider provider {};
    const clap_preset_discovery_indexer* indexer = nullptr;
    std::vector<std::string> folders; // kept alive: the indexer holds their paths
};
const clap_preset_discovery_provider* create_provider(const clap_preset_discovery_factory*, const clap_preset_discovery_indexer* indexer,
    const char* id)
{
    if (!indexer || !id || std::strcmp(id, provider_descriptor.id) != 0)
        return nullptr;
    auto* self = new Provider;
    self->indexer = indexer;
    auto& p = self->provider;
    p.desc = &provider_descriptor;
    p.provider_data = self;
    p.init = [](const clap_preset_discovery_provider* p) {
        auto& self = *static_cast<Provider*>(p->provider_data);
        const auto* indexer = self.indexer;
        const clap_preset_discovery_filetype type { "lo-fly preset", "A lo-fly sound as text", lofly::kPresetExtension.data() + 1 };
        indexer->declare_filetype(indexer, &type);
        const clap_preset_discovery_location factory { CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "lo-fly Factory",
            CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr };
        indexer->declare_location(indexer, &factory);
        self.folders = user_preset_folders();
        for (const auto& folder : self.folders) {
            const clap_preset_discovery_location user { CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, "lo-fly User",
                CLAP_PRESET_DISCOVERY_LOCATION_FILE, folder.c_str() };
            indexer->declare_location(indexer, &user);
        }
        return true;
    };
    p.destroy = [](const clap_preset_discovery_provider* p) { delete static_cast<Provider*>(p->provider_data); };
    p.get_metadata = [](const clap_preset_discovery_provider*, uint32_t kind, const char* location,
                         const clap_preset_discovery_metadata_receiver* r) {
        if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
            for (const auto& preset : lofly::factoryPresets()) {
                if (!r->begin_preset(r, preset.name.c_str(), preset.name.c_str()))
                    break;
                describe(r, preset, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
            }
            return true;
        }
        if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || !location)
            return false;
        std::string error;
        const auto preset = read_preset_file(location, &error);
        if (!preset) {
            r->on_error(r, 0, error.c_str());
            return false;
        }
        if (r->begin_preset(r, preset->name.c_str(), nullptr)) // one preset a file: no load key
            describe(r, *preset, CLAP_PRESET_DISCOVERY_IS_USER_CONTENT);
        return true;
    };
    p.get_extension = [](const clap_preset_discovery_provider*, const char*) -> const void* { return nullptr; };
    return &p;
}
const clap_preset_discovery_factory preset_factory {
    [](const clap_preset_discovery_factory*) -> uint32_t { return 1; },
    [](const clap_preset_discovery_factory*, uint32_t i) -> const clap_preset_discovery_provider_descriptor* { return i == 0 ? &provider_descriptor : nullptr; },
    create_provider,
};

// --- remote controls (Melodee's EDIT pages) ---

struct RemotePage {
    const char* name;
    std::array<const char*, 8> keys;
};
constexpr RemotePage remote_pages[] {
    { "LoFi", { "lofi.on", "lofi.character", "lofi.amount", "lofi.drive", "lofi.wear", "lofi.tone", "lofi.wobble", "lofi.speaker" } },
    { "Voice", { "master.volume", "lofi.vintage", "voice.mode", "voice.count", "voice.glide", "voice.bend_range", "voice.tune", "voice.velocity" } },
    { "Engine 1", { "eng1.on", "eng1.type", "eng1.osc.wave", "eng1.osc.timbre", "eng1.octave", "eng1.coarse", "eng1.fine", "eng1.level" } },
    { "Engine 1 Mix", { "eng1.hardware", "eng1.pan", "eng1.eq.low", "eng1.eq.high", "eng1.inst.start", "eng1.smp.mode", "eng1.smp.slot", nullptr } },
    { "Engine 2", { "eng2.on", "eng2.type", "eng2.osc.wave", "eng2.osc.timbre", "eng2.octave", "eng2.coarse", "eng2.fine", "eng2.level" } },
    { "Engine 2 Mix", { "eng2.hardware", "eng2.pan", "eng2.eq.low", "eng2.eq.high", "eng2.inst.start", "eng2.smp.mode", "eng2.smp.slot", nullptr } },
    { "Noise", { "noise.on", "noise.mode", "noise.texture", "noise.transient", "noise.tune", "noise.level", "noise.stereo", "noise.pan" } },
    { "Filter", { "filter.on", "filter.type", "filter.mode", "filter.slope", "filter.cutoff", "filter.resonance", "filter.env_amount", "filter.keytrack" } },
    { "Envelopes", { "fenv.attack", "fenv.decay", "fenv.sustain", "fenv.release", "amp.attack", "amp.decay", "amp.sustain", "amp.release" } },
    { "Macros", { "macro.1", "macro.2", "macro.3", "macro.4", "vib.depth", "vib.rate", "vib.mod_wheel", "vib.fade" } },
    { "Effects", { "fx1.type", "fx1.on", "fx2.type", "fx2.on", "fx3.type", "fx3.on", "fx4.type", "fx4.on" } },
};
uint32_t remote_count(const clap_plugin*) { return static_cast<uint32_t>(std::size(remote_pages)); }
bool remote_get(const clap_plugin*, uint32_t index, clap_remote_controls_page* page)
{
    if (index >= std::size(remote_pages))
        return false;
    const auto& r = remote_pages[index];
    *page = {};
    std::snprintf(page->section_name, sizeof(page->section_name), "lo-fly");
    page->page_id = index;
    std::snprintf(page->page_name, sizeof(page->page_name), "%s", r.name);
    for (std::size_t k = 0; k < r.keys.size(); ++k)
        page->param_ids[k] = r.keys[k] && lofly::findParam(r.keys[k]) ? clap_id(r.keys[k]) : CLAP_INVALID_ID;
    page->is_for_preset = false;
    return true;
}
const clap_plugin_remote_controls remote_ext { remote_count, remote_get };

// --- Melodee's screen ---

void request_redraw(void* context)
{
    auto& s = *static_cast<Plugin*>(context);
    if (s.screen_host)
        s.screen_host->redraw(s.host, 0);
}
// The printed buttons the page takes, told to Melodee when they change (the FX page, its lit slot, the picker).
void tell_keys(Plugin& s)
{
    const std::uint32_t keys = s.panel->keys();
    if (s.screen_host_2 && keys != s.keys_told) {
        s.screen_host_2->keys(s.host, keys);
        s.keys_told = keys;
    }
}
bool screen_draw(const void* p, const melodee_screen_frame_t* frame)
{
    auto& s = of(p);
    s.synth->collectGarbage();
    const bool drew = s.panel->draw(*frame);
    tell_keys(s); // a preset or the state may have filled or emptied the lit slot
    return drew;
}
bool screen_input(const void* p, const melodee_screen_input_t* in)
{
    auto& s = of(p);
    bool changed = false;
    const bool used = s.panel->input(*in, changed);
    tell_keys(s);
    if (changed && s.host_params)
        s.host_params->rescan(s.host, CLAP_PARAM_RESCAN_VALUES); // Melodee reads the values again
    if (used && s.screen_host)
        s.screen_host->redraw(s.host, changed ? MELODEE_SCREEN_RUN : 0); // RUN: the next block takes the value
    return used;
}
void screen_controls(const void* p, uint32_t ids[3]) { of(p).panel->controls(ids); }
const melodee_screen_t screen_ext { screen_draw, screen_input, screen_controls };

const void* get_extension(const clap_plugin*, const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &params_ext;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &audio_ext;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &note_ext;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &state_ext;
    if (!std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS) || !std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT))
        return &remote_ext;
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT))
        return &preset_load_ext;
    if (!std::strcmp(id, MELODEE_SCREEN_CLAP))
        return &screen_ext;
    return nullptr;
}

// --- lifecycle ---

bool init(const clap_plugin* p)
{
    auto& s = of(p);
    s.host_params = static_cast<const clap_host_params*>(s.host->get_extension(s.host, CLAP_EXT_PARAMS));
    s.screen_host = static_cast<const melodee_screen_host_t*>(s.host->get_extension(s.host, MELODEE_SCREEN_CLAP_HOST)); // Melodee only
    s.screen_host_2 = static_cast<const melodee_screen_host_2_t*>(s.host->get_extension(s.host, MELODEE_SCREEN_CLAP_HOST_2));
    s.host_preset_load = static_cast<const clap_host_preset_load*>(s.host->get_extension(s.host, CLAP_EXT_PRESET_LOAD));
    if (!s.host_preset_load)
        s.host_preset_load = static_cast<const clap_host_preset_load*>(s.host->get_extension(s.host, CLAP_EXT_PRESET_LOAD_COMPAT));
    (void)lofly::builtinWavetables(); // built once, off the audio thread
    (void)lofly::builtinNoises();
    (void)id_table();
    s.panel = std::make_unique<Panel>(s.store, request_redraw, &s);
    s.events.reserve(Plugin::max_events);
    s.apply_all();
    return true;
}
void destroy(const clap_plugin* p) { delete &of(p); }
bool activate(const clap_plugin* p, double rate, uint32_t, uint32_t max_frames)
{
    auto& s = of(p);
    s.rate = rate;
    s.spare.assign(max_frames, 0.0f);
    s.apply_all();
    s.synth->prepare(rate, static_cast<int>(max_frames));
    return true;
}
void deactivate(const clap_plugin*) {}
bool start_processing(const clap_plugin*) { return true; }
void stop_processing(const clap_plugin*) {}
void reset(const clap_plugin* p)
{
    auto& s = of(p);
    const std::array<lofly::Event, 1> panic { lofly::Event::allSoundOff(0) };
    float silence[2][16] {};
    s.synth->process(silence[0], silence[1], 16, panic);
}
void on_main_thread(const clap_plugin* p) { of(p).synth->collectGarbage(); }

const clap_plugin* create(const clap_plugin_factory*, const clap_host* host, const char* id)
{
    if (!host || !id || std::strcmp(id, descriptor.id) != 0 || !clap_version_is_compatible(host->clap_version))
        return nullptr;
    auto* s = new Plugin;
    s->host = host;
    auto& p = s->plugin;
    p.desc = &descriptor;
    p.plugin_data = s;
    p.init = init;
    p.destroy = destroy;
    p.activate = activate;
    p.deactivate = deactivate;
    p.start_processing = start_processing;
    p.stop_processing = stop_processing;
    p.reset = reset;
    p.process = process;
    p.get_extension = get_extension;
    p.on_main_thread = on_main_thread;
    return &p;
}
uint32_t factory_count(const clap_plugin_factory*) { return 1; }
const clap_plugin_descriptor* factory_descriptor(const clap_plugin_factory*, uint32_t index) { return index == 0 ? &descriptor : nullptr; }
const clap_plugin_factory factory { factory_count, factory_descriptor, create };

} // namespace
} // namespace lofly_melodee

extern "C" {
CLAP_EXPORT const clap_plugin_entry clap_entry {
    CLAP_VERSION_INIT,
    [](const char* path) {
        lofly_melodee::plugin_path() = path ? path : "";
        return true;
    },
    [] {},
    [](const char* id) -> const void* {
        if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID))
            return &lofly_melodee::factory;
        if (!std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) || !std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT))
            return &lofly_melodee::preset_factory;
        return nullptr;
    },
};
}
