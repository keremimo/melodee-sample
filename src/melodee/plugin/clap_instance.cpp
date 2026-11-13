// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/clap_instance.h"
#include "melodee/plugin/plugin_files.h"

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

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
        if (std::strcmp (id, CLAP_EXT_THREAD_CHECK) == 0) return &threads;
        if (std::strcmp (id, CLAP_EXT_LOG) == 0) return &logger;
        if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &params;
        if (std::strcmp (id, CLAP_EXT_LATENCY) == 0) return &latency;
        if (std::strcmp (id, CLAP_EXT_TAIL) == 0) return &tail;
        if (std::strcmp (id, CLAP_EXT_STATE) == 0) return &state;
        if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPorts;
        if (std::strcmp (id, CLAP_EXT_NOTE_PORTS) == 0) return &notePorts;
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

} // namespace melodee
