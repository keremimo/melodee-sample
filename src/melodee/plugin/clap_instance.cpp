// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/clap_instance.h"
#include "melodee/plugin/plugin_files.h"

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <map>

namespace melodee {
using namespace juce;

namespace
{
// Set while this thread is in a plug-in's processBlock(): its audio thread (thread-check).
thread_local bool inAudio = false;
struct AudioScope
{
    AudioScope() : was (inAudio) { inAudio = true; }
    ~AudioScope() { inAudio = was; }
    const bool was;
};

constexpr char stateTag[4] = { 'M', 'C', 'L', 'P' };
constexpr uint32_t maxEvents = 1024;

String text (const char* s) { return s != nullptr ? String::fromUTF8 (s).trim() : String(); }

bool hasFeature (const clap_plugin_descriptor& d, const char* feature)
{
    if (d.features != nullptr)
        for (const char* const* f = d.features; *f != nullptr; ++f)
            if (std::strcmp (*f, feature) == 0)
                return true;
    return false;
}

AudioChannelSet channelSet (uint32_t channels)
{
    return channels == 1 ? AudioChannelSet::mono()
         : channels == 2 ? AudioChannelSet::stereo()
                         : AudioChannelSet::discreteChannels ((int) channels);
}
} // namespace

// One dlopen'd library and its clap_entry, init() once for however many plug-ins are made from it.
struct ClapInstance::Library
{
    void* handle = nullptr;
    const clap_plugin_entry* entry = nullptr;
    int users = 0;

    static std::mutex& lock() { static std::mutex m; return m; }
    static std::vector<std::unique_ptr<Library>>& all() { static std::vector<std::unique_ptr<Library>> v; return v; }

    static Library* acquire (const File& file, String& error)
    {
        std::lock_guard<std::mutex> held (lock());
        void* handle = dlopen (file.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) { error = dlerror(); return nullptr; }
        for (auto& l : all())
            if (l->handle == handle) { dlclose (handle); ++l->users; return l.get(); } // the same object again
        auto* entry = static_cast<const clap_plugin_entry*> (dlsym (handle, "clap_entry"));
        if (entry == nullptr) { error = "No clap_entry"; dlclose (handle); return nullptr; }
        if (! clap_version_is_compatible (entry->clap_version)) { error = "CLAP version not supported"; dlclose (handle); return nullptr; }
        if (! entry->init (file.getFullPathName().toRawUTF8())) { error = "The plug-in would not start"; dlclose (handle); return nullptr; }
        auto l = std::make_unique<Library>();
        l->handle = handle;
        l->entry = entry;
        l->users = 1;
        all().push_back (std::move (l));
        return all().back().get();
    }

    static void release (Library* library)
    {
        std::lock_guard<std::mutex> held (lock());
        if (--library->users > 0) return;
        library->entry->deinit();
        dlclose (library->handle);
        auto& v = all();
        v.erase (std::remove_if (v.begin(), v.end(), [&] (const auto& l) { return l.get() == library; }), v.end());
    }

    const clap_plugin_factory* factory() const
    {
        return static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    }
};

// What the plug-in is given as its host. Made before the plug-in (which may call it from create_plugin on) and
// before the ClapInstance (which needs the plug-in's ports first), so what the plug-in asks is kept here.
struct ClapInstance::Host
{
    clap_host host {};
    ClapInstance* owner = nullptr;
    std::atomic<bool> callback { false }, rescan { false }, latencyChanged { false };

    Host()
    {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "Melodee";
        host.vendor = "Ellic Studio";
        host.url = "";
        host.version = "1";
        host.get_extension = &extension;
        host.request_restart = [] (const clap_host*) {}; // not while it plays: a reload (picked again) restarts it
        host.request_process = [] (const clap_host*) {}; // it is processed every block
        host.request_callback = [] (const clap_host* h) { of (h).callback = true; };
    }
    static Host& of (const clap_host* h) { return *static_cast<Host*> (h->host_data); }

    static const void* extension (const clap_host*, const char* id)
    {
        static const clap_host_thread_check threads {
            [] (const clap_host*) { return ! inAudio; },
            [] (const clap_host*) { return inAudio; },
        };
        static const clap_host_log logger { [] (const clap_host*, clap_log_severity severity, const char* message) {
            if (severity >= CLAP_LOG_ERROR && ! inAudio)
                std::fprintf (stderr, "clap: %s\n", message != nullptr ? message : "");
        } };
        static const clap_host_params params {
            [] (const clap_host* h, clap_param_rescan_flags flags) {
                if ((flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT | CLAP_PARAM_RESCAN_ALL)) != 0)
                    of (h).rescan = true;
            },
            [] (const clap_host*, clap_id, clap_param_clear_flags) {},
            [] (const clap_host*) {}, // request_flush: its next block takes the changes
        };
        static const clap_host_latency latency { [] (const clap_host* h) { of (h).latencyChanged = true; } };
        static const clap_host_tail tail { [] (const clap_host*) {} };
        static const clap_host_state state { [] (const clap_host*) {} };
        static const clap_host_audio_ports audioPorts {
            [] (const clap_host*, uint32_t) { return false; },
            [] (const clap_host*, uint32_t) {},
        };
        static const clap_host_note_ports notePorts {
            [] (const clap_host*) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; },
            [] (const clap_host*, uint32_t) {},
        };
        static const clap_host_preset_load presetLoad {
            [] (const clap_host*, uint32_t, const char* location, const char*, int32_t, const char* message) {
                std::fprintf (stderr, "clap: preset %s: %s\n", location != nullptr ? location : "(inside)",
                              message != nullptr ? message : "");
            },
            [] (const clap_host*, uint32_t, const char*, const char*) {}, // PRESETS marks the one picked here
        };
        if (std::strcmp (id, CLAP_EXT_THREAD_CHECK) == 0) return &threads;
        if (std::strcmp (id, CLAP_EXT_LOG) == 0) return &logger;
        if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &params;
        if (std::strcmp (id, CLAP_EXT_LATENCY) == 0) return &latency;
        if (std::strcmp (id, CLAP_EXT_TAIL) == 0) return &tail;
        if (std::strcmp (id, CLAP_EXT_STATE) == 0) return &state;
        if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPorts;
        if (std::strcmp (id, CLAP_EXT_NOTE_PORTS) == 0) return &notePorts;
        if (std::strcmp (id, CLAP_EXT_PRESET_LOAD) == 0 || std::strcmp (id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0) return &presetLoad;
        return nullptr;
    }
};

struct ClapInstance::Event
{
    union
    {
        clap_event_header header;
        clap_event_note note;
        clap_event_midi midi;
        clap_event_param_value param;
        clap_event_transport transport;
    };
};

// A CLAP parameter as JUCE's: 0..1 over its range. The value is the one the plug-in last gave (or this host last
// sent); a new one waits for the plug-in's next block.
class ClapInstance::Parameter final : public AudioPluginInstance::HostedParameter
{
public:
    Parameter (ClapInstance& owner, const clap_param_info& info, double current)
        : owner (owner), id (info.id), cookie (info.cookie), flags (info.flags),
          minimum (info.min_value), maximum (info.max_value), initial (normalise (info.default_value)),
          label (text (info.name)), value ((float) normalise (current)) {}

    double normalise (double plain) const { return maximum > minimum ? std::clamp ((plain - minimum) / (maximum - minimum), 0.0, 1.0) : 0.0; }
    double plain (double normalised) const
    {
        double v = minimum + std::clamp (normalised, 0.0, 1.0) * (maximum - minimum);
        return (flags & CLAP_PARAM_IS_STEPPED) != 0 ? std::round (v) : v;
    }

    float getValue() const override { return value.load (std::memory_order_relaxed); }
    void setValue (float v) override
    {
        value.store (v, std::memory_order_relaxed);
        pending.store (true, std::memory_order_release);
        owner.anyPending.store (true, std::memory_order_release);
    }
    float getDefaultValue() const override { return (float) initial; }
    String getName (int maximumLength) const override { return label.substring (0, maximumLength); }
    String getLabel() const override { return {}; }
    String getParameterID() const override { return String (id); }
    bool isAutomatable() const override { return (flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY)) == 0; }
    bool isDiscrete() const override { return (flags & CLAP_PARAM_IS_STEPPED) != 0; }
    bool isBoolean() const override { return isDiscrete() && maximum - minimum == 1.0; }
    int getNumSteps() const override
    {
        return isDiscrete() && maximum > minimum ? (int) std::lround (maximum - minimum) + 1 : AudioProcessor::getDefaultNumParameterSteps();
    }

    String getText (float v, int maximumLength) const override
    {
        const double p = plain (v);
        if (owner.params != nullptr && owner.params->value_to_text != nullptr && ! inAudio)
        {
            std::lock_guard<std::recursive_mutex> held (owner.mainLock);
            char out[256] = {};
            if (owner.params->value_to_text (owner.plugin, id, p, out, sizeof (out)))
                return text (out).substring (0, maximumLength);
        }
        return String (p, isDiscrete() ? 0 : 3).substring (0, maximumLength);
    }
    float getValueForText (const String& t) const override
    {
        double p = t.getDoubleValue();
        if (owner.params != nullptr && owner.params->text_to_value != nullptr && ! inAudio)
        {
            std::lock_guard<std::recursive_mutex> held (owner.mainLock);
            owner.params->text_to_value (owner.plugin, id, t.toRawUTF8(), &p);
        }
        return (float) normalise (p);
    }

    ClapInstance& owner;
    const clap_id id;
    void* const cookie;
    const uint32_t flags;
    const double minimum, maximum, initial;
    const String label;
    std::atomic<float> value;
    std::atomic<bool> pending { false };
};

// ---------------------------------------------------------------------------------------------------------------

std::vector<ClapInstance::Info> ClapInstance::list (const File& file, String& error)
{
    std::vector<Info> out;
    auto* library = Library::acquire (file, error);
    if (library == nullptr) return out;
    if (auto* factory = library->factory())
    {
        for (uint32_t i = 0, n = factory->get_plugin_count (factory); i < n; ++i)
            if (auto* d = factory->get_plugin_descriptor (factory, i); d != nullptr && d->id != nullptr)
                out.push_back ({ d->id, text (d->name).toStdString(), text (d->vendor).toStdString(),
                                 hasFeature (*d, CLAP_PLUGIN_FEATURE_INSTRUMENT) });
    }
    if (out.empty()) error = "No plug-in in the library";
    Library::release (library);
    return out;
}

std::unique_ptr<ClapInstance> ClapInstance::load (const File& file, int32_t id, double rate, int block, String& error)
{
    auto* library = Library::acquire (file, error);
    if (library == nullptr) return {};
    auto fail = [&] (const char* why, const clap_plugin* made = nullptr) {
        if (made != nullptr) made->destroy (made);
        Library::release (library);
        error = why;
        return std::unique_ptr<ClapInstance>();
    };
    auto* factory = library->factory();
    if (factory == nullptr) return fail ("No plug-in factory");
    const clap_plugin_descriptor* chosen = nullptr;
    for (uint32_t i = 0, n = factory->get_plugin_count (factory); i < n && chosen == nullptr; ++i)
        if (auto* d = factory->get_plugin_descriptor (factory, i); d != nullptr && d->id != nullptr)
            if (id == 0 || clap_id_hash (d->id) == id)
                chosen = d;
    if (chosen == nullptr) return fail (id == 0 ? "No plug-in in the library" : "No longer in its library");

    auto host = std::make_unique<Host>();
    const clap_plugin* plugin = factory->create_plugin (factory, &host->host, chosen->id);
    if (plugin == nullptr) return fail ("Could not make the plug-in");
    if (! plugin->init (plugin)) return fail ("The plug-in would not start", plugin);

    Info info { chosen->id, text (chosen->name).toStdString(), text (chosen->vendor).toStdString(),
                hasFeature (*chosen, CLAP_PLUGIN_FEATURE_INSTRUMENT) };

    // Audio ports: the main one each way is the bus the rack plays through; the others get buffers of their own.
    std::vector<Port> inputs, outputs;
    BusesProperties buses;
    if (auto* ports = static_cast<const clap_plugin_audio_ports*> (plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS)))
    {
        for (bool input : { true, false })
        {
            auto& list = input ? inputs : outputs;
            for (uint32_t i = 0, n = ports->count (plugin, input); i < n; ++i)
            {
                clap_audio_port_info p {};
                if (! ports->get (plugin, i, input, &p)) p.channel_count = 0;
                list.push_back ({ std::min<uint32_t> (p.channel_count, 32) });
            }
            if (! list.empty() && list[0].channels > 0)
                buses = input ? buses.withInput ("Input", channelSet (list[0].channels), true)
                              : buses.withOutput ("Output", channelSet (list[0].channels), true);
        }
    }
    // Note ports: the first input one. Notes as CLAP notes when it prefers them (or takes nothing else).
    int notePort = -1;
    bool clapNotes = false, midiNotes = false;
    if (auto* ports = static_cast<const clap_plugin_note_ports*> (plugin->get_extension (plugin, CLAP_EXT_NOTE_PORTS)))
    {
        clap_note_port_info p {};
        if (ports->count (plugin, true) > 0 && ports->get (plugin, 0, true, &p))
        {
            notePort = 0;
            midiNotes = (p.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
            clapNotes = (p.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0
                     && (p.preferred_dialect == CLAP_NOTE_DIALECT_CLAP || ! midiNotes);
            if (! clapNotes && ! midiNotes) notePort = -1; // MIDI 2 only
        }
    }
    return std::unique_ptr<ClapInstance> (new ClapInstance (std::move (host), library, plugin, info, file, buses,
                                                            std::move (inputs), std::move (outputs), notePort,
                                                            clapNotes, midiNotes, rate, block));
}

ClapInstance::ClapInstance (std::unique_ptr<Host> h, Library* l, const clap_plugin* p, const Info& i, const File& f,
                            BusesProperties buses, std::vector<Port> inputs, std::vector<Port> outputs, int note,
                            bool clap, bool midi, double r, int b)
    : AudioPluginInstance (buses), host (std::move (h)), library (l), plugin (p), file (f), info (i),
      rate (r), block (b), inputPorts (std::move (inputs)), outputPorts (std::move (outputs)),
      notePort (note), clapNotes (clap), midiNotes (midi)
{
    host->owner = this;
    name = String::fromUTF8 (info.name.c_str());
    if (name.isEmpty()) name = file.getFileNameWithoutExtension();
    params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
    state = static_cast<const clap_plugin_state*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
    latency = static_cast<const clap_plugin_latency*> (plugin->get_extension (plugin, CLAP_EXT_LATENCY));
    tail = static_cast<const clap_plugin_tail*> (plugin->get_extension (plugin, CLAP_EXT_TAIL));
    presetLoad = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
    if (presetLoad == nullptr)
        presetLoad = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD_COMPAT));
    events.resize (maxEvents);
    std::lock_guard<std::recursive_mutex> held (mainLock);
    readParameters();
}

ClapInstance::~ClapInstance()
{
    {
        std::lock_guard<std::recursive_mutex> held (mainLock);
        releaseResources();
        plugin->destroy (plugin);
    }
    Library::release (library);
}

void ClapInstance::readParameters()
{
    if (params == nullptr) return;
    for (uint32_t i = 0, n = params->count (plugin); i < n; ++i)
    {
        clap_param_info p {};
        if (! params->get_info (plugin, i, &p)) continue;
        double current = p.default_value;
        params->get_value (plugin, p.id, &current);
        auto parameter = std::make_unique<Parameter> (*this, p, current);
        auto* raw = parameter.get();
        if ((p.flags & CLAP_PARAM_IS_BYPASS) != 0 && bypass == nullptr) bypass = raw;
        byId.push_back ({ p.id, (int) parameters.size() });
        parameters.push_back (raw);
        addHostedParameter (std::move (parameter));
    }
    std::sort (byId.begin(), byId.end());
}

void ClapInstance::refreshValues()
{
    if (params == nullptr) return;
    for (auto* p : parameters)
    {
        if (p->pending.load (std::memory_order_acquire)) continue; // one of ours still to go: that is the value
        double v = 0;
        if (params->get_value (plugin, p->id, &v))
            p->value.store ((float) p->normalise (v), std::memory_order_relaxed);
    }
}

void ClapInstance::readLatency()
{
    if (latency != nullptr && active)
        setLatencySamples ((int) std::min<uint32_t> (latency->get (plugin), 1u << 20)); // roadmap P4
}

void ClapInstance::idle()
{
    std::lock_guard<std::recursive_mutex> held (mainLock);
    if (host->callback.exchange (false)) plugin->on_main_thread (plugin);
    if (host->rescan.exchange (false)) refreshValues();
    if (host->latencyChanged.exchange (false)) readLatency();
}

AudioProcessorParameter* ClapInstance::getBypassParameter() const { return bypass; }

void ClapInstance::fillInPluginDescription (PluginDescription& d) const
{
    d.name = d.descriptiveName = name;
    d.pluginFormatName = "CLAP";
    d.category = info.instrument ? "Synth" : "Effect";
    d.manufacturerName = String::fromUTF8 (info.vendor.c_str());
    d.fileOrIdentifier = file.getFullPathName();
    d.uniqueId = d.deprecatedUid = clap_id_hash (info.id);
    d.isInstrument = info.instrument;
    d.numInputChannels = getTotalNumInputChannels();
    d.numOutputChannels = getTotalNumOutputChannels();
}

void ClapInstance::prepareToPlay (double newRate, int newBlock)
{
    std::lock_guard<std::recursive_mutex> held (mainLock);
    releaseResources();
    rate = newRate;
    block = std::max (newBlock, 1);
    setRateAndBufferSizeDetails (rate, block);

    size_t channels = 0;
    for (auto& p : inputPorts) channels += p.channels;
    for (auto& p : outputPorts) channels += p.channels;
    audio.assign (channels * (size_t) block, 0.0f);
    inputPointers.clear();
    outputPointers.clear();
    size_t at = 0;
    for (auto& p : inputPorts) for (uint32_t c = 0; c < p.channels; ++c, ++at) inputPointers.push_back (audio.data() + at * (size_t) block);
    for (auto& p : outputPorts) for (uint32_t c = 0; c < p.channels; ++c, ++at) outputPointers.push_back (audio.data() + at * (size_t) block);

    active = plugin->activate (plugin, rate, 1, (uint32_t) block);
    steadyTime = 0;
    readLatency();
}

void ClapInstance::stopProcessing()
{
    if (! processing) return;
    AudioScope audioThread; // it no longer runs on the audio thread: this thread stands in for it
    plugin->stop_processing (plugin);
    processing = false;
}

void ClapInstance::releaseResources()
{
    std::lock_guard<std::recursive_mutex> held (mainLock);
    stopProcessing();
    if (active) plugin->deactivate (plugin);
    active = false;
}

bool ClapInstance::pushOut (const clap_output_events* list, const clap_event_header* e)
{
    auto* self = static_cast<ClapInstance*> (list->ctx);
    if (e->space_id != CLAP_CORE_EVENT_SPACE_ID || e->type != CLAP_EVENT_PARAM_VALUE) return true;
    const auto& v = *reinterpret_cast<const clap_event_param_value*> (e);
    auto it = std::lower_bound (self->byId.begin(), self->byId.end(), std::make_pair (v.param_id, -1));
    if (it != self->byId.end() && it->first == v.param_id)
    {
        auto* p = self->parameters[(size_t) it->second];
        if (! p->pending.load (std::memory_order_acquire))
            p->value.store ((float) p->normalise (v.value), std::memory_order_relaxed);
    }
    return true;
}

void ClapInstance::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    const int frames = std::min (buffer.getNumSamples(), block);
    if (! active || frames <= 0) { buffer.clear(); return; }
    AudioScope audioThread;
    if (! processing) processing = plugin->start_processing (plugin);
    if (! processing) { buffer.clear(); return; }

    // Events, in time order: the transport, parameter changes at 0, then the notes and MIDI.
    eventCount = 0;
    auto add = [this] () -> Event* { return eventCount < maxEvents ? &events[eventCount++] : nullptr; };
    if (auto* e = add())
    {
        auto& t = e->transport;
        t = {};
        t.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_IS_PLAYING;
        t.tempo = 120.0;
        t.song_pos_beats = (clap_beattime) std::llround ((double) steadyTime / rate * 2.0 * (double) CLAP_BEATTIME_FACTOR);
        t.tsig_num = t.tsig_denom = 4;
    }
    if (anyPending.exchange (false, std::memory_order_acq_rel))
        for (auto* p : parameters)
            if (p->pending.exchange (false, std::memory_order_acq_rel))
                if (auto* e = add())
                {
                    auto& v = e->param;
                    v = {};
                    v.header = { sizeof (clap_event_param_value), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
                    v.param_id = p->id;
                    v.cookie = p->cookie;
                    v.note_id = -1; v.port_index = -1; v.channel = -1; v.key = -1;
                    v.value = p->plain (p->value.load (std::memory_order_relaxed));
                }
    if (notePort >= 0)
    {
        for (const auto m : midi)
        {
            if (m.numBytes < 1 || m.numBytes > 3 || m.samplePosition >= frames) continue;
            const uint32_t time = (uint32_t) std::max (m.samplePosition, 0);
            const uint8_t status = m.data[0], kind = status & 0xf0, channel = status & 0x0f;
            const uint8_t d1 = m.numBytes > 1 ? m.data[1] : 0, d2 = m.numBytes > 2 ? m.data[2] : 0;
            const bool noteOn = kind == 0x90 && d2 > 0, noteOff = kind == 0x80 || (kind == 0x90 && d2 == 0);
            const bool allOff = kind == 0xb0 && (d1 == 120 || d1 == 123);
            if (clapNotes && (noteOn || noteOff || allOff))
            {
                if (auto* e = add())
                {
                    auto& n = e->note;
                    n = {};
                    uint16_t type = allOff ? CLAP_EVENT_NOTE_CHOKE : noteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
                    n.header = { sizeof (clap_event_note), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 };
                    n.note_id = -1;
                    n.port_index = (int16_t) notePort;
                    n.channel = allOff ? -1 : (int16_t) channel;
                    n.key = allOff ? -1 : (int16_t) (d1 & 0x7f);
                    n.velocity = allOff ? 0.0 : (d2 & 0x7f) / 127.0;
                }
                if (! allOff || ! midiNotes) continue; // all notes off goes on as MIDI too, for a plug-in's own handling
            }
            if (! midiNotes) continue; // a CLAP-notes-only plug-in: no CC, bend or pressure
            if (auto* e = add())
            {
                auto& x = e->midi;
                x = {};
                x.header = { sizeof (clap_event_midi), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
                x.port_index = (uint16_t) notePort;
                x.data[0] = status; x.data[1] = d1; x.data[2] = d2;
            }
        }
    }

    clap_input_events in { this,
        [] (const clap_input_events* l) { return static_cast<const ClapInstance*> (l->ctx)->eventCount; },
        [] (const clap_input_events* l, uint32_t i) -> const clap_event_header* {
            auto* self = static_cast<const ClapInstance*> (l->ctx);
            return i < self->eventCount ? &self->events[i].header : nullptr;
        } };
    clap_output_events out { this, &ClapInstance::pushOut };

    // Audio: the main input port from the buffer, every other input silent.
    std::array<clap_audio_buffer, 16> inBuffers {}, outBuffers {};
    const uint32_t inCount = (uint32_t) std::min<size_t> (inputPorts.size(), inBuffers.size());
    const uint32_t outCount = (uint32_t) std::min<size_t> (outputPorts.size(), outBuffers.size());
    size_t at = 0;
    for (uint32_t i = 0; i < inCount; ++i)
    {
        inBuffers[i].data32 = inputPointers.data() + at;
        inBuffers[i].channel_count = inputPorts[i].channels;
        for (uint32_t c = 0; c < inputPorts[i].channels; ++c, ++at)
        {
            if (i == 0 && (int) c < buffer.getNumChannels() && (int) c < getTotalNumInputChannels())
                std::memcpy (inputPointers[at], buffer.getReadPointer ((int) c), sizeof (float) * (size_t) frames);
            else
                std::memset (inputPointers[at], 0, sizeof (float) * (size_t) frames);
        }
    }
    at = 0;
    for (uint32_t i = 0; i < outCount; ++i)
    {
        outBuffers[i].data32 = outputPointers.data() + at;
        outBuffers[i].channel_count = outputPorts[i].channels;
        at += outputPorts[i].channels;
    }

    clap_process process {};
    process.steady_time = steadyTime;
    process.frames_count = (uint32_t) frames;
    process.transport = &events[0].transport;
    process.audio_inputs = inBuffers.data();
    process.audio_outputs = outBuffers.data();
    process.audio_inputs_count = inCount;
    process.audio_outputs_count = outCount;
    process.in_events = &in;
    process.out_events = &out;
    const auto status = plugin->process (plugin, &process);

    const int mainOut = outCount > 0 ? (int) outputPorts[0].channels : 0;
    for (int c = 0; c < buffer.getNumChannels(); ++c)
    {
        if (c < mainOut && status != CLAP_PROCESS_ERROR) buffer.copyFrom (c, 0, outputPointers[(size_t) c], frames);
        else buffer.clear (c, 0, frames);
    }
    steadyTime += frames;
}

double ClapInstance::getTailLengthSeconds() const
{
    if (tail == nullptr || ! active) return 0.0;
    const uint32_t samples = tail->get (plugin);
    if (samples == UINT32_MAX) return 60.0; // "infinite"
    return samples > 1 ? (double) samples / rate : 0.0;
}

// Saved as "MCLP", then the plug-in's own state.
void ClapInstance::getStateInformation (MemoryBlock& out)
{
    out.reset();
    out.append (stateTag, sizeof (stateTag));
    if (state == nullptr) return;
    std::lock_guard<std::recursive_mutex> held (mainLock);
    clap_ostream stream { &out, [] (const clap_ostream* s, const void* data, uint64_t size) -> int64_t {
        static_cast<MemoryBlock*> (s->ctx)->append (data, (size_t) size);
        return (int64_t) size;
    } };
    if (! state->save (plugin, &stream)) out.setSize (sizeof (stateTag));
}

void ClapInstance::setStateInformation (const void* data, int size)
{
    if (state == nullptr || size < (int) sizeof (stateTag) || std::memcmp (data, stateTag, sizeof (stateTag)) != 0) return;
    struct Reader { const uint8* at; uint64_t left; } reader { static_cast<const uint8*> (data) + sizeof (stateTag), (uint64_t) size - sizeof (stateTag) };
    clap_istream stream { &reader, [] (const clap_istream* s, void* to, uint64_t want) -> int64_t {
        auto& r = *static_cast<Reader*> (s->ctx);
        const uint64_t n = std::min (want, r.left);
        std::memcpy (to, r.at, (size_t) n);
        r.at += n;
        r.left -= n;
        return (int64_t) n;
    } };
    std::lock_guard<std::recursive_mutex> held (mainLock);
    if (state->load (plugin, &stream)) refreshValues();
}

// ---------------------------------------------------------------------------------------------------------------
// Presets: preset discovery (clap/factory/preset-discovery.h) to find them, preset-load to load one.

namespace
{
namespace fs = std::filesystem;

// One file's presets as its provider named them (the ones for this plug-in), and what it was when read.
struct FileFound
{
    uint64_t size = 0;
    int64_t time = 0;
    std::vector<std::pair<std::string, std::string>> presets; // name, load key
};

int64_t fileTime (const fs::path& p, std::error_code& e)
{
    return (int64_t) fs::last_write_time (p, e).time_since_epoch().count();
}

// The cache file: a first line naming it, then each file read ("F", path, size, time) and its presets after it
// ("P", name, key), tab separated. Names never hold a tab or a line break (clean() makes them spaces).
constexpr char cacheHeader[] = "melodee clap presets 1";

std::string clean (const char* s)
{
    std::string out = text (s).toStdString();
    for (auto& c : out) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

std::vector<std::string> fields (const std::string& line)
{
    std::vector<std::string> out (1);
    for (char c : line)
        if (c == '\t') out.emplace_back();
        else out.back() += c;
    return out;
}

std::map<std::string, FileFound> readCache (const File& cache)
{
    std::map<std::string, FileFound> out;
    if (cache == File()) return out;
    std::ifstream in (cache.getFullPathName().toStdString());
    std::string line;
    if (! std::getline (in, line) || line != cacheHeader) return out;
    FileFound* file = nullptr;
    while (std::getline (in, line))
    {
        auto f = fields (line);
        if (f[0] == "F" && f.size() == 4)
        {
            file = &out[f[1]];
            file->size = std::strtoull (f[2].c_str(), nullptr, 10);
            file->time = std::strtoll (f[3].c_str(), nullptr, 10);
        }
        else if (f[0] == "P" && f.size() == 3 && file != nullptr)
            file->presets.emplace_back (f[1], f[2]);
    }
    return out;
}

void writeCache (const File& cache, const std::map<std::string, FileFound>& files)
{
    if (cache == File()) return;
    std::error_code e;
    const fs::path to (cache.getFullPathName().toStdString()), part (to.string() + ".part");
    fs::create_directories (to.parent_path(), e);
    {
        std::ofstream out (part, std::ios::trunc);
        out << cacheHeader << '\n';
        for (const auto& [path, file] : files)
        {
            out << "F\t" << path << '\t' << file.size << '\t' << file.time << '\n';
            for (const auto& [name, key] : file.presets) out << "P\t" << name << '\t' << key << '\n';
        }
        if (! out) { fs::remove (part, e); return; }
    }
    fs::rename (part, to, e); // whole or not at all
}

// What a provider declares in init(): the files its presets are in and where to look for them.
struct Indexer
{
    struct Location { std::string name, path; bool inside = false; };
    std::vector<std::string> extensions; // lower case, no dot; "" any file
    std::vector<Location> locations;
    clap_preset_discovery_indexer indexer {};

    Indexer()
    {
        indexer.clap_version = CLAP_VERSION;
        indexer.name = "Melodee";
        indexer.vendor = "Ellic Studio";
        indexer.url = "";
        indexer.version = "1";
        indexer.indexer_data = this;
        indexer.declare_filetype = [] (const clap_preset_discovery_indexer* i, const clap_preset_discovery_filetype* t) {
            if (t == nullptr) return false;
            auto ext = text (t->file_extension).toLowerCase().trimCharactersAtStart (".").toStdString();
            static_cast<Indexer*> (i->indexer_data)->extensions.push_back (ext);
            return true;
        };
        indexer.declare_location = [] (const clap_preset_discovery_indexer* i, const clap_preset_discovery_location* l) {
            if (l == nullptr) return false;
            const bool inside = l->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
            if (! inside && (l->kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || l->location == nullptr)) return false;
            static_cast<Indexer*> (i->indexer_data)->locations.push_back ({ text (l->name).toStdString(),
                                                                           inside ? std::string() : std::string (l->location), inside });
            return true;
        };
        indexer.declare_soundpack = [] (const clap_preset_discovery_indexer*, const clap_preset_discovery_soundpack*) { return true; };
        indexer.get_extension = [] (const clap_preset_discovery_indexer*, const char*) -> const void* { return nullptr; };
    }

    bool wanted (const fs::path& p) const
    {
        const auto name = p.filename().string();
        if (name.empty() || name[0] == '.') return false; // a Mac's ._ files beside each on the card
        if (extensions.empty()) return true;
        auto ext = String (p.extension().string()).toLowerCase().trimCharactersAtStart (".").toStdString();
        for (const auto& want : extensions)
            if (want.empty() || want == ext) return true;
        return false;
    }
};

// What get_metadata() says of one file: each preset, and whether it is for this plug-in (it names no plug-in, or
// names this one).
struct Receiver
{
    struct Found { std::string name, key; bool named = false, ours = false; };
    const std::string& id;
    std::vector<Found> found;
    clap_preset_discovery_metadata_receiver receiver {};

    explicit Receiver (const std::string& pluginId) : id (pluginId)
    {
        receiver.receiver_data = this;
        receiver.on_error = [] (const clap_preset_discovery_metadata_receiver*, int32_t, const char*) {};
        receiver.begin_preset = [] (const clap_preset_discovery_metadata_receiver* r, const char* name, const char* key) {
            of (r).found.push_back ({ clean (name), key != nullptr ? clean (key) : std::string() });
            return true;
        };
        receiver.add_plugin_id = [] (const clap_preset_discovery_metadata_receiver* r, const clap_universal_plugin_id* p) {
            auto& self = of (r);
            if (self.found.empty() || p == nullptr) return;
            auto& f = self.found.back();
            f.named = true;
            if (p->abi != nullptr && p->id != nullptr && std::strcmp (p->abi, "clap") == 0 && p->id == self.id) f.ours = true;
        };
        receiver.set_soundpack_id = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
        receiver.set_flags = [] (const clap_preset_discovery_metadata_receiver*, uint32_t) {};
        receiver.add_creator = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
        receiver.set_description = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
        receiver.set_timestamps = [] (const clap_preset_discovery_metadata_receiver*, clap_timestamp, clap_timestamp) {};
        receiver.add_feature = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
        receiver.add_extra_info = [] (const clap_preset_discovery_metadata_receiver*, const char*, const char*) {};
    }
    static Receiver& of (const clap_preset_discovery_metadata_receiver* r) { return *static_cast<Receiver*> (r->receiver_data); }

    // The presets for this plug-in, a file's single unnamed one by the file's name.
    std::vector<std::pair<std::string, std::string>> ours (const fs::path& file) const
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& f : found)
            if (! f.named || f.ours)
                out.emplace_back (f.name.empty() ? file.stem().string() : f.name, f.key);
        return out;
    }
};

// Case-blind, with "/" before any other character, so a folder's own presets come before a folder beside it.
bool sortsBefore (const std::string& a, const std::string& b)
{
    auto rank = [] (char c) { return c == '/' ? 0 : std::tolower ((unsigned char) c) + 1; };
    for (size_t n = 0; n < a.size() && n < b.size(); ++n)
        if (rank (a[n]) != rank (b[n])) return rank (a[n]) < rank (b[n]);
    return a.size() < b.size();
}
} // namespace

std::vector<ClapInstance::Preset> ClapInstance::findPresets (const File& cache) const
{
    auto* factory = static_cast<const clap_preset_discovery_factory*> (library->entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (factory == nullptr)
        factory = static_cast<const clap_preset_discovery_factory*> (library->entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
    if (factory == nullptr) return {};

    const auto started = Time::getMillisecondCounterHiRes();
    const auto cached = readCache (cache);
    std::map<std::string, FileFound> after;
    size_t read = 0;
    struct Found { Preset preset; size_t location; };
    std::vector<Found> found;
    std::vector<std::string> locationNames;

    for (uint32_t p = 0, providers = factory->count (factory); p < providers; ++p)
    {
        auto* d = factory->get_descriptor (factory, p);
        if (d == nullptr || d->id == nullptr) continue;
        Indexer indexer;
        auto* provider = factory->create (factory, &indexer.indexer, d->id);
        if (provider == nullptr) continue;
        if (! provider->init (provider)) { provider->destroy (provider); continue; }

        for (const auto& location : indexer.locations)
        {
            const size_t at = locationNames.size();
            auto shown = String (location.name);
            if (shown.startsWithIgnoreCase (name + " ")) shown = shown.substring (name.length() + 1); // "Surge XT Factory Presets"
            locationNames.push_back ((shown.isEmpty() ? String ("Presets") : shown).replaceCharacter ('/', '-').toStdString());

            Receiver receiver (info.id);
            if (location.inside)
            {
                if (provider->get_metadata (provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receiver.receiver))
                    for (auto& [n, key] : receiver.ours ({}))
                        found.push_back ({ { n, {}, {}, key }, at });
                continue;
            }
            // A link (Surge XT's data beside its copy in RAM) read where it leads, so the cache and the files PRESETS
            // loads stay the same from one opening to the next.
            std::error_code e;
            fs::path root = fs::canonical (location.path, e);
            if (e) continue;
            std::vector<fs::path> files;
            const bool single = fs::is_regular_file (root, e);
            const fs::path base = single ? root.parent_path() : root; // where its folders count from
            if (single) files.push_back (root);
            else if (fs::is_directory (root, e))
                for (auto it = fs::recursive_directory_iterator (root, fs::directory_options::skip_permission_denied, e);
                     ! e && it != fs::recursive_directory_iterator(); it.increment (e))
                {
                    std::error_code f;
                    const auto leaf = it->path().filename().string();
                    if (! leaf.empty() && leaf[0] == '.') { if (it->is_directory (f)) it.disable_recursion_pending(); continue; }
                    if (it->is_regular_file (f) && indexer.wanted (it->path())) files.push_back (it->path());
                }
            for (const auto& path : files)
            {
                std::error_code f, g;
                FileFound now;
                now.size = (uint64_t) fs::file_size (path, f);
                now.time = fileTime (path, g);
                if (f || g) continue;
                auto known = cached.find (path.string());
                if (known != cached.end() && known->second.size == now.size && known->second.time == now.time)
                    now.presets = known->second.presets;
                else
                {
                    receiver.found.clear();
                    if (provider->get_metadata (provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.string().c_str(), &receiver.receiver))
                        now.presets = receiver.ours (path);
                    ++read;
                }
                std::string where = path.parent_path().lexically_relative (base).generic_string();
                if (where == ".") where.clear();
                for (const auto& [n, key] : now.presets)
                    found.push_back ({ { n, where, path.string(), key }, at });
                after[path.string()] = std::move (now);
            }
        }
        provider->destroy (provider);
    }

    // More than one location holding presets: each a folder of its own, by its name.
    std::vector<bool> holds (locationNames.size(), false);
    for (const auto& f : found) holds[f.location] = true;
    if (std::count (holds.begin(), holds.end(), true) > 1)
        for (auto& f : found)
            f.preset.folder = locationNames[f.location] + (f.preset.folder.empty() ? "" : "/" + f.preset.folder);
    std::stable_sort (found.begin(), found.end(), [] (const Found& a, const Found& b) {
        if (a.location != b.location) return a.location < b.location;
        if (a.preset.folder != b.preset.folder) return sortsBefore (a.preset.folder, b.preset.folder);
        return sortsBefore (a.preset.name, b.preset.name);
    });
    if (read > 0 || after.size() != cached.size()) writeCache (cache, after);

    std::vector<Preset> out;
    out.reserve (found.size());
    for (auto& f : found) out.push_back (std::move (f.preset));
    std::fprintf (stderr, "clap: %s: %zu presets, %zu files read, %.0f ms\n", name.toRawUTF8(), out.size(), read,
                  Time::getMillisecondCounterHiRes() - started);
    return out;
}

void ClapInstance::setPresets (std::vector<Preset> found)
{
    std::lock_guard<std::recursive_mutex> held (mainLock);
    presets = std::move (found);
    currentPreset = -1;
}

int ClapInstance::getNumPrograms()
{
    std::lock_guard<std::recursive_mutex> held (mainLock);
    return std::max (1, (int) presets.size());
}

int ClapInstance::getCurrentProgram()
{
    std::lock_guard<std::recursive_mutex> held (mainLock);
    return std::max (currentPreset, 0);
}

const String ClapInstance::getProgramName (int index)
{
    std::lock_guard<std::recursive_mutex> held (mainLock);
    return index >= 0 && index < (int) presets.size() ? String::fromUTF8 (presets[(size_t) index].name.c_str()) : String();
}

// The plug-in takes it in its own time (Surge XT: at its next block).
void ClapInstance::setCurrentProgram (int index)
{
    std::lock_guard<std::recursive_mutex> held (mainLock);
    if (presetLoad == nullptr || index < 0 || index >= (int) presets.size()) return;
    const auto& p = presets[(size_t) index];
    const bool inside = p.location.empty();
    if (presetLoad->from_location (plugin, inside ? CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN : CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                   inside ? nullptr : p.location.c_str(), p.key.empty() ? nullptr : p.key.c_str()))
    {
        currentPreset = index;
        refreshValues();
    }
}

} // namespace melodee
