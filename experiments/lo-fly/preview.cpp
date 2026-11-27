// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
//
// lo-fly's panel without an MPC: hosts the plug-in through its clap_entry as Melodee would (params, notes, the screen
// extension), plays notes, presses Minus / Plus / the wheel / Push, and writes each screen as a PNG at twice its size.
//
//   preview OUTDIR      the screens
//   preview --presets   the presets preset discovery finds, each loaded once
//   preview --bench     render and draw times
#include <clap/clap.h>
#include <melodee_screen.h>

#include <filesystem>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" const clap_plugin_entry clap_entry;

namespace {

std::string library_path = "preview"; // what clap_entry.init() is given: this program, so Presets/ is beside it

// --- PNG (stored deflate blocks: no compression library needed) ---

std::uint32_t crc32(const std::uint8_t* d, std::size_t n, std::uint32_t c = 0)
{
    c = ~c;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    for (int s = 24; s >= 0; s -= 8)
        v.push_back(static_cast<std::uint8_t>(x >> s));
}
void chunk(std::vector<std::uint8_t>& png, const char* type, const std::vector<std::uint8_t>& data)
{
    put32(png, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    put32(png, crc32(body.data(), body.size()));
}
bool write_png(const std::string& path, const std::uint32_t* px, int w, int h, int scale)
{
    const int W = w * scale, H = h * scale;
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(H) * (1 + 3 * static_cast<std::size_t>(W)));
    for (int y = 0; y < H; ++y) {
        raw.push_back(0);
        for (int x = 0; x < W; ++x) {
            const std::uint32_t c = px[(y / scale) * w + x / scale];
            raw.push_back(static_cast<std::uint8_t>(c >> 16));
            raw.push_back(static_cast<std::uint8_t>(c >> 8));
            raw.push_back(static_cast<std::uint8_t>(c));
        }
    }
    std::vector<std::uint8_t> z { 0x78, 0x01 };
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (std::size_t at = 0; at < raw.size(); at += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<long>(at), raw.begin() + static_cast<long>(at + n));
    }
    put32(z, b << 16 | a);
    std::vector<std::uint8_t> png { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, static_cast<std::uint32_t>(W));
    put32(ihdr, static_cast<std::uint32_t>(H));
    ihdr.insert(ihdr.end(), { 8, 2, 0, 0, 0 });
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

// --- a host as Melodee is to the plug-in ---

struct Host {
    clap_host host {};
    melodee_screen_host_2_t screen {};
    clap_host_params params {};
    int redraws = 0;
    uint32_t keys = 0; // the printed buttons the page takes, as the plug-in told them last
};
Host* host_of(const void* h) { return static_cast<Host*>(static_cast<const clap_host*>(h)->host_data); }

union Event {
    clap_event_header header;
    clap_event_note note;
    clap_event_param_value param;
};
struct EventList {
    std::vector<Event> events;
    clap_input_events list { this, [](const clap_input_events* l) { return static_cast<uint32_t>(static_cast<EventList*>(l->ctx)->events.size()); },
        [](const clap_input_events* l, uint32_t i) -> const clap_event_header* { return &static_cast<EventList*>(l->ctx)->events[i].header; } };
    void note(uint16_t type, int key, double velocity, uint32_t time = 0)
    {
        Event e {};
        e.note = { { sizeof(clap_event_note), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 }, -1, 0, 0, static_cast<int16_t>(key), velocity };
        events.push_back(e);
    }
    void param(clap_id id, double value)
    {
        Event e {};
        e.param = { { sizeof(clap_event_param_value), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 }, id, nullptr, -1, -1, -1, -1, value };
        events.push_back(e);
    }
};

clap_id id_of(const char* key)
{
    std::uint32_t h = 2166136261u;
    for (const char* c = key; *c; ++c) {
        h ^= static_cast<std::uint8_t>(*c);
        h *= 16777619u;
    }
    return h & 0x7fffffffu;
}

struct Session {
    Host host;
    const clap_plugin* plugin = nullptr;
    const clap_plugin_params* params = nullptr;
    const melodee_screen_t* screen = nullptr;
    const clap_plugin_preset_load* presets = nullptr;
    std::vector<float> left, right;
    std::uint32_t block = 64;
    double rate = 48000;
    std::uint64_t time_ms = 1000;
    std::array<std::uint32_t, 320 * 218> pixels {};
    bool drawn = false;

    Session()
    {
        host.host = { CLAP_VERSION_INIT, &host, "Preview", "Melodee", "", "1.0",
            [](const clap_host* h, const char* id) -> const void* {
                auto* self = static_cast<Host*>(h->host_data);
                if (!std::strcmp(id, MELODEE_SCREEN_CLAP_HOST) || !std::strcmp(id, MELODEE_SCREEN_CLAP_HOST_2))
                    return &self->screen;
                if (!std::strcmp(id, CLAP_EXT_PARAMS))
                    return &self->params;
                return nullptr;
            },
            [](const clap_host*) {}, [](const clap_host*) {}, [](const clap_host*) {} };
        host.screen.redraw = [](const void* h, uint32_t) { host_of(h)->redraws++; };
        host.screen.keys = [](const void* h, uint32_t keys) { host_of(h)->keys = keys; };
        host.params.rescan = [](const clap_host*, clap_param_rescan_flags) {};
        host.params.clear = [](const clap_host*, clap_id, clap_param_clear_flags) {};
        host.params.request_flush = [](const clap_host*) {};
        clap_entry.init(library_path.c_str());
        const auto* factory = static_cast<const clap_plugin_factory*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
        plugin = factory->create_plugin(factory, &host.host, factory->get_plugin_descriptor(factory, 0)->id);
        plugin->init(plugin);
        params = static_cast<const clap_plugin_params*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        screen = static_cast<const melodee_screen_t*>(plugin->get_extension(plugin, MELODEE_SCREEN_CLAP));
        presets = static_cast<const clap_plugin_preset_load*>(plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
        plugin->activate(plugin, rate, 1, block);
        plugin->start_processing(plugin);
        left.resize(block);
        right.resize(block);
    }
    ~Session()
    {
        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    bool preset(const char* name) { return presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, name); }
    void set(const char* key, double value)
    {
        EventList l;
        l.param(id_of(key), value);
        params->flush(plugin, &l.list, nullptr);
    }
    // Runs `seconds` of audio; `first` events go into the first block.
    void run(double seconds, EventList* first = nullptr)
    {
        const int blocks = static_cast<int>(seconds * rate / block);
        EventList none;
        for (int b = 0; b < blocks; ++b) {
            float* outs[2] { left.data(), right.data() };
            clap_audio_buffer out {};
            out.data32 = outs;
            out.channel_count = 2;
            clap_event_transport transport {};
            transport.header = { sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
            transport.flags = CLAP_TRANSPORT_HAS_TEMPO;
            transport.tempo = 90;
            clap_output_events sink { nullptr, [](const clap_output_events*, const clap_event_header*) { return true; } };
            clap_process p {};
            p.frames_count = block;
            p.transport = &transport;
            p.audio_outputs = &out;
            p.audio_outputs_count = 1;
            p.in_events = b == 0 && first ? &first->list : &none.list;
            p.out_events = &sink;
            plugin->process(plugin, &p);
        }
        time_ms += static_cast<std::uint64_t>(seconds * 1000.0);
    }
    void input(uint32_t kind, int32_t value = 1, uint32_t shift = 0)
    {
        const melodee_screen_input_t in { kind, value, shift };
        screen->input(plugin, &in);
    }
    void page(int n) // from wherever it is: back to the first, then forward
    {
        for (int i = 0; i < 20; ++i)
            input(MELODEE_SCREEN_STEP, -1);
        for (int i = 0; i < n; ++i)
            input(MELODEE_SCREEN_STEP, 1);
    }
    melodee_screen_frame_t frame(std::array<int16_t, 3> pots = { -1, -1, -1 }, std::array<uint8_t, 3> live = { 1, 1, 1 }, bool all = true)
    {
        melodee_screen_frame_t f {};
        f.pixels = pixels.data();
        f.width = 320;
        f.height = strip ? MELODEE_SCREEN_STRIP_HEIGHT : MELODEE_SCREEN_PAGE_HEIGHT;
        f.stride = 320;
        f.area = strip ? MELODEE_SCREEN_STRIP : MELODEE_SCREEN_PAGE;
        f.redraw_all = all ? 1 : 0;
        f.colour = 0xe2817a;
        f.time_ms = time_ms;
        for (int k = 0; k < 3; ++k) {
            f.pots[k] = pots[static_cast<std::size_t>(k)];
            f.pots_live[k] = live[static_cast<std::size_t>(k)];
        }
        return f;
    }
    bool shot(const std::string& path, std::array<int16_t, 3> pots = { -1, -1, -1 }, std::array<uint8_t, 3> live = { 1, 1, 1 })
    {
        for (int i = 0; i < 12; ++i) { // as Melodee draws while the sound plays: the wobble line and the level move
            run(0.03);
            auto f = frame(pots, live, false);
            screen->draw(plugin, &f);
        }
        auto f = frame(pots, live);
        screen->draw(plugin, &f);
        return write_png(path, pixels.data(), 320, strip ? MELODEE_SCREEN_STRIP_HEIGHT : MELODEE_SCREEN_PAGE_HEIGHT, 2);
    }
    bool strip = false; // draw the 320 x 144 strip under the chain slots instead of the page
};

void patch(Session& s)
{
    s.set("lofi.character", 0);
    s.set("lofi.amount", 0.7);
    s.set("lofi.drive", 0.45);
    s.set("lofi.wear", 0.6);
    s.set("lofi.tone", -0.25);
    s.set("lofi.wobble", 0.55);
    s.set("lofi.speaker", 0.4);
    s.set("eng1.osc.wave", 2);
    s.set("eng1.osc.timbre", 0.35);
    s.set("eng1.hardware", 4);
    s.set("eng2.on", 1);
    s.set("eng2.osc.wave", 1);
    s.set("eng2.hardware", 3);
    s.set("eng2.level", -9);
    s.set("noise.on", 1);
    s.set("noise.texture", 1);
    s.set("noise.level", -20);
    s.set("filter.type", 1);
    s.set("filter.cutoff", 1800);
    s.set("filter.resonance", 0.45);
    s.set("filter.env_amount", 0.3);
    s.set("fenv.attack", 0.02);
    s.set("fenv.decay", 0.6);
    s.set("fenv.sustain", 0.35);
    s.set("fenv.release", 0.8);
    s.set("amp.attack", 0.01);
    s.set("amp.decay", 1.2);
    s.set("amp.sustain", 0.7);
    s.set("amp.release", 1.5);
    s.set("fx1.type", 3); // Tape Echo
    s.set("fx1.mix", 0.3);
    s.set("fx2.type", 1); // Reverb
    s.set("fx2.mix", 0.25);
    s.set("fx2.send", 1);
    s.set("voice.glide", 0.04);
}

EventList chord(std::initializer_list<int> keys, uint16_t type = CLAP_EVENT_NOTE_ON)
{
    EventList l;
    for (int k : keys)
        l.note(type, k, 0.8);
    return l;
}

int shots(const std::string& dir)
{
    Session s;
    constexpr const char* sound = "Cassette Clouds"; // a factory preset: two engines, noise, Faded Tape, chorus, reverb
    if (!s.preset(sound)) {
        std::fprintf(stderr, "no preset %s\n", sound);
        return 1;
    }
    auto on = chord({ 48, 55, 60, 63, 67 });
    s.run(0.7, &on);
    int failed = 0;
    auto shot = [&](const char* name, std::array<int16_t, 3> pots = { -1, -1, -1 }, std::array<uint8_t, 3> live = { 1, 1, 1 }) {
        if (!s.shot(dir + "/" + name + ".png", pots, live)) {
            std::fprintf(stderr, "cannot write %s\n", name);
            ++failed;
        }
    };
    s.page(0);
    s.run(0.25);
    shot("01-lofi", { 89, 57, 30 }, { 1, 1, 0 }); // K3's pot has not reached Wear yet
    s.page(1);
    shot("02-tape");
    s.page(2);
    shot("03-engine-1");
    s.page(3);
    shot("04-engine-2");
    s.page(4);
    shot("05-noise");
    s.page(5);
    shot("06-filter");
    s.set("filter.type", 2);
    shot("06b-filter-lofi");
    s.preset(sound);
    s.page(6);
    shot("07-filter-env");
    s.page(7);
    shot("08-amp-env");
    // FX: the rack (Chorus, Reverb, two empty slots); the wheel to FX 2, then 3; Push on the empty slot: the effect
    // menu on None; three down, Tape Echo, added; MUTE switches it off, ERASE empties the slot; Push on Reverb: the menu
    // on Reverb; Minus back; SHIFT + Push: Reverb to Send.
    s.page(8);
    shot("09-fx");
    s.input(MELODEE_SCREEN_WHEEL, 1);
    shot("10-fx-slot-2");
    s.input(MELODEE_SCREEN_WHEEL, 1);
    shot("11-fx-empty");
    if (s.host.keys != 0) {
        std::fprintf(stderr, "an empty slot takes no keys\n");
        ++failed;
    }
    s.input(MELODEE_SCREEN_PUSH);
    shot("11b-fx-menu");
    s.input(MELODEE_SCREEN_WHEEL, 3);
    shot("11c-fx-menu-tape-echo");
    s.input(MELODEE_SCREEN_PUSH);
    shot("11d-fx-added");
    if (s.host.keys != (MELODEE_SCREEN_KEY_MUTE | MELODEE_SCREEN_KEY_ERASE)) {
        std::fprintf(stderr, "an effect in the lit slot takes MUTE and ERASE\n");
        ++failed;
    }
    s.input(MELODEE_SCREEN_MUTE);
    shot("11e-fx-muted");
    s.input(MELODEE_SCREEN_ERASE);
    shot("11f-fx-erased");
    s.input(MELODEE_SCREEN_WHEEL, -1);
    s.input(MELODEE_SCREEN_PUSH);
    shot("11g-fx-change");
    s.input(MELODEE_SCREEN_STEP, -1); // Minus: the menu closes
    s.input(MELODEE_SCREEN_PUSH, 1, 1);
    shot("11h-fx-send");
    s.preset(sound);
    s.page(9);
    shot("13-voice");
    // Push on the LOFI page: the LoFi Processor off, then on again; the wheel to the next character.
    s.page(0);
    s.input(MELODEE_SCREEN_PUSH);
    shot("14-lofi-off");
    s.input(MELODEE_SCREEN_PUSH);
    s.input(MELODEE_SCREEN_WHEEL, 3);
    auto off = chord({ 48, 55, 60, 63, 67 }, CLAP_EVENT_NOTE_OFF);
    s.run(0.05, &off);
    shot("15-lofi-wheel");
    s.preset("Arcade Daydream");
    s.page(2);
    shot("16-arcade-engine-1");
    s.page(8);
    s.input(MELODEE_SCREEN_WHEEL, -3);
    shot("17-arcade-fx-1");
    s.strip = true;
    s.page(0);
    shot("18-strip-lofi");
    s.page(2);
    shot("19-strip-engine-1");
    s.strip = false;
    return failed;
}

// The presets as Melodee's PRESETS finds them: each provider's locations, then each location's presets.
int list_presets()
{
    struct Found {
        std::string location, name, key, path;
        bool inside;
    };
    struct Indexer {
        clap_preset_discovery_indexer indexer {};
        std::vector<std::pair<std::string, std::string>> locations; // name, path ("" inside the plug-in)
        std::vector<std::string> types;
    } ix;
    ix.indexer = { CLAP_VERSION_INIT, "Preview", "Melodee", "", "1", &ix,
        [](const clap_preset_discovery_indexer* i, const clap_preset_discovery_filetype* t) {
            static_cast<Indexer*>(i->indexer_data)->types.push_back(t->file_extension);
            return true;
        },
        [](const clap_preset_discovery_indexer* i, const clap_preset_discovery_location* l) {
            static_cast<Indexer*>(i->indexer_data)->locations.push_back({ l->name, l->location ? l->location : "" });
            return true;
        },
        [](const clap_preset_discovery_indexer*, const clap_preset_discovery_soundpack*) { return true; },
        [](const clap_preset_discovery_indexer*, const char*) -> const void* { return nullptr; } };
    struct Receiver {
        clap_preset_discovery_metadata_receiver r {};
        std::vector<std::pair<std::string, std::string>> found; // name, key
        std::string feature;
    } rx;
    rx.r.receiver_data = &rx;
    rx.r.on_error = [](const clap_preset_discovery_metadata_receiver*, int32_t, const char* m) { std::fprintf(stderr, "  error: %s\n", m); };
    rx.r.begin_preset = [](const clap_preset_discovery_metadata_receiver* r, const char* n, const char* k) {
        static_cast<Receiver*>(r->receiver_data)->found.push_back({ n ? n : "", k ? k : "" });
        return true;
    };
    rx.r.add_plugin_id = [](const clap_preset_discovery_metadata_receiver*, const clap_universal_plugin_id*) {};
    rx.r.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver*, const char*) {};
    rx.r.set_flags = [](const clap_preset_discovery_metadata_receiver*, uint32_t) {};
    rx.r.add_creator = [](const clap_preset_discovery_metadata_receiver*, const char*) {};
    rx.r.set_description = [](const clap_preset_discovery_metadata_receiver*, const char*) {};
    rx.r.set_timestamps = [](const clap_preset_discovery_metadata_receiver*, clap_timestamp, clap_timestamp) {};
    rx.r.add_feature = [](const clap_preset_discovery_metadata_receiver*, const char*) {};
    rx.r.add_extra_info = [](const clap_preset_discovery_metadata_receiver*, const char*, const char*) {};

    // A user preset in a folder of its own, in the Presets folder beside this program (removed at the end).
    const auto folder = std::filesystem::path(library_path).parent_path() / "Presets" / "Preview";
    std::filesystem::create_directories(folder);
    const auto user = folder / "Preview Test.loflypreset";
    if (FILE* f = std::fopen(user.c_str(), "w")) {
        std::fputs("lofly-preset 1\nname Preview Test\ncategory Keys\nset lofi.character Tube Radio\nset filter.cutoff 900\n", f);
        std::fclose(f);
    }
    struct Remove {
        std::filesystem::path p;
        ~Remove()
        {
            std::error_code e;
            std::filesystem::remove_all(p, e);
            std::filesystem::remove(p.parent_path(), e); // Presets/ too, if nothing else is in it
        }
    } remove { folder };

    Session s; // calls clap_entry.init
    const auto* factory = static_cast<const clap_preset_discovery_factory*>(clap_entry.get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (!factory || factory->count(factory) != 1) {
        std::fprintf(stderr, "no preset discovery factory\n");
        return 1;
    }
    const auto* provider = factory->create(factory, &ix.indexer, factory->get_descriptor(factory, 0)->id);
    if (!provider || !provider->init(provider)) {
        std::fprintf(stderr, "provider failed\n");
        return 1;
    }
    std::vector<Found> all;
    for (const auto& [name, path] : ix.locations) {
        if (path.empty()) {
            rx.found.clear();
            provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &rx.r);
            for (const auto& [n, k] : rx.found)
                all.push_back({ name, n, k, "", true });
            continue;
        }
        std::error_code e;
        for (auto it = std::filesystem::recursive_directory_iterator(path, e); !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e))
            if (it->path().extension() == ".loflypreset") {
                rx.found.clear();
                provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, it->path().c_str(), &rx.r);
                for (const auto& [n, k] : rx.found)
                    all.push_back({ name, n, k, it->path().string(), false });
            }
    }
    provider->destroy(provider);
    int failed = 0;
    for (const auto& f : all) {
        const bool ok = f.inside ? s.presets->from_location(s.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, f.key.c_str())
                                 : s.presets->from_location(s.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, f.path.c_str(), f.key.empty() ? nullptr : f.key.c_str());
        auto on = chord({ 48, 55, 60, 64 });
        s.run(0.3, &on);
        auto off = chord({ 48, 55, 60, 64 }, CLAP_EVENT_NOTE_OFF);
        s.run(0.2, &off);
        float peak = 0.0f;
        for (float v : s.left)
            peak = std::max(peak, std::fabs(v));
        std::printf("%-14s %-24s %s, last block's peak %.1f dB\n", f.location.c_str(), f.name.c_str(), ok ? "loaded" : "FAILED",
            20.0 * std::log10(std::max(peak, 1e-6f)));
        failed += ok ? 0 : 1;
    }
    std::printf("%zu presets in %zu locations, %d failed to load\n", all.size(), ix.locations.size(), failed);
    return failed == 0 && !all.empty() ? 0 : 1;
}

int bench()
{
    Session s;
    patch(s);
    // Render: five notes held, then sixteen.
    for (int voices : { 5, 8, 16 }) {
        EventList on;
        for (int k = 0; k < voices; ++k)
            on.note(CLAP_EVENT_NOTE_ON, 40 + k * 3, 0.8);
        const double seconds = 10.0;
        const auto t0 = std::chrono::steady_clock::now();
        s.run(seconds, &on);
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("render %2d voices: %.1f%% of one core (%.3f s for %.0f s)\n", voices, 100.0 * took / seconds, took, seconds);
        auto off = chord({}, CLAP_EVENT_NOTE_OFF);
        for (int k = 0; k < voices; ++k)
            off.note(CLAP_EVENT_NOTE_OFF, 40 + k * 3, 0.0);
        s.run(3.0, &off);
    }
    // Draw: every page, full redraws.
    double worst = 0.0, total = 0.0;
    for (int p = 0; p < 13; ++p) {
        s.page(p);
        constexpr int n = 200;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            s.time_ms += 16;
            auto f = s.frame({ 64, 64, 30 }, { 1, 1, 0 });
            s.screen->draw(s.plugin, &f);
        }
        const double ms = 1000.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("  page %2d: %.3f ms\n", p, ms);
        worst = std::max(worst, ms);
        total += ms;
    }
    std::printf("draw: %.3f ms a page on average, %.3f ms the slowest page\n", total / 13.0, worst);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    library_path = argv[0];
    if (argc == 2 && !std::strcmp(argv[1], "--bench"))
        return bench();
    if (argc == 2 && !std::strcmp(argv[1], "--presets"))
        return list_presets();
    if (argc != 2) {
        std::fprintf(stderr, "usage: preview OUTDIR | --presets | --bench\n");
        return 2;
    }
    return shots(argv[1]) == 0 ? 0 : 1;
}
