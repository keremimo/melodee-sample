// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/tools.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/sample_data.h"
#include "melodee/model/project_file.h"
#include "melodee/plugin/plugin_rack.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

// --melodee-print-spike MODE [KEY=VALUE...] (freeze plan F0): can a print be made by a second EngineHost that holds
// one track, and can prints be played back from the card?
//   equal   each track of the bench's busy project alone in a second host against the same track in the whole
//           project: its stem, sample for sample (seconds, rate)
//   speed   one track alone, rendered as fast as it goes at nice 10: how many times real time (track, seconds, rate,
//           plugin=PATH [effect=1])
//   plugin  a plug-in opened in two hosts at once: RAM each, time to open, both rendered and compared (plugin=PATH,
//           effect=1 for an effect on a kit track, seconds, rate)
//   stream  16 print streams (44.1 kHz float stereo) and 24 voices (int16 stereo) read from files on the card as the
//           stream pager would, in real time: read times, lowest buffer, underruns (dir, seconds, hold_ms)
//   memory  what one host costs (lean=1: a print host, F2a)
//   project the bench's busy project as a project file for the app (F2a's check on the MPC): its four samples written
//           as WAVs in <card>/Samples/<name>, the project in <card>/Melodee/Projects/<name>.mldp, track 16 a PLUGIN
//           track on `plugin` (a path from the card's top) playing an arpeggio (card, name, plugin)
// Nothing opens the audio device. Run on the MPC beside the running app (and a --melodee-dsp-bench for load).
namespace melodee {

void busy_project(Project& p, PatternBank& b); // dsp_bench.cpp
std::vector<SampleData> bench_samples();

namespace {

using Clock = std::chrono::steady_clock;
double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

long meminfo_kb(const char* key) {
    std::ifstream in("/proc/meminfo");
    std::string name;
    long value = 0;
    std::string unit;
    while (in >> name >> value >> unit) if (name == key) return value;
    return -1;
}
long rss_kb() {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) if (line.rfind("VmRSS:", 0) == 0) return std::atol(line.c_str() + 6);
    return -1;
}

struct Args {
    std::map<std::string, std::string> v;
    std::string get(const std::string& k, const std::string& d) const { auto it = v.find(k); return it == v.end() ? d : it->second; }
    double num(const std::string& k, double d) const { auto it = v.find(k); return it == v.end() ? d : std::atof(it->second.c_str()); }
};

// A host playing `p` from its first tick, as dsp_bench starts one.
std::unique_ptr<EngineHost> start_host(const Project& p, const PatternBank& b, unsigned rate, const std::vector<SampleData>& samples) {
    auto host = std::make_unique<EngineHost>();
    host->set_render_threads(0);
    host->set_overload_guard(false); // nothing stolen: the same notes play in both
    host->load(p, b);
    host->set_audio_format(rate, 384);
    Command c {Command::Type::Reload};
    c.pack = new ProjectPack(p, b);
    host->send(c);
    for (unsigned s = 0; s < samples.size(); ++s) host->offer_sample(s, &samples[s]);
    return host;
}
void drain(EngineHost& host) {
    const ProjectPack* pack = nullptr;
    while (host.take_done_pack(pack)) delete pack;
}

// Only track `t` of the project kept (its place, its chain, its patterns); the buses' effects and the master's off.
void keep_one(Project& p, PatternBank& b, unsigned t) {
    for (unsigned k = 0; k < track_count; ++k)
        if (k != t) { p.tracks[k] = Track {}; for (auto& q : b.at[k]) q = Pattern {}; }
    for (auto& bus : p.buses) bus.fx = Chain {{}, bus_slots};
}

// An instrument plug-in on track 1, an arpeggio over a bar; or (effect) the bench's first kit with the plug-in first
// in its chain.
void plugin_project(Project& p, PatternBank& b, const std::string& path, bool effect) {
    std::snprintf(p.plugins[0].path, sizeof(p.plugins[0].path), "%s", path.c_str());
    if (effect) {
        busy_project(p, b);
        keep_one(p, b, 0);
        p.tracks[0].fx.slots[0] = {plugin_effect_kind, false, {0, 0, 0, 0, 0, 0}};
        return;
    }
    auto& t = p.tracks[0];
    load_sound(t, EngineKind::Plugin, 0);
    t.plugin = 0;
    auto& q = b.at[0][0];
    const int keys[] {48, 52, 55, 60, 64, 60, 55, 52};
    for (unsigned s = 0; s < 16; ++s) {
        Event e;
        e.tick = s * sixteenth_ticks; e.length = uint16_t(sixteenth_ticks - 4); e.key = uint8_t(keys[s % 8] + (s / 8) * 2); e.velocity = uint8_t(80 + s * 2);
        q.add(e);
    }
    p.tempo = 1240;
}

std::string fs_name(const std::string& path) { auto at = path.find_last_of('/'); return at == std::string::npos ? path : path.substr(at + 1); }

struct Rig {
    std::unique_ptr<EngineHost> host;
    std::unique_ptr<PluginRack> rack;
};
// A host and its own rack with the plug-in project loaded; false when the plug-in did not open in `wait` seconds.
bool open_rig(Rig& r, const Project& p, const PatternBank& b, unsigned rate, const std::vector<SampleData>& samples, double& took, std::string& why) {
    static unsigned rigs = 0;
    std::string stage = std::string(std::getenv("MELODEE_PLUGIN_STAGE") ? std::getenv("MELODEE_PLUGIN_STAGE") : "/tmp/print-spike") + "-rig" + std::to_string(++rigs);
    setenv("MELODEE_PLUGIN_STAGE", stage.c_str(), 1); // each rack its own: the same entry's folder name would clash
    r.host = start_host(p, b, rate, samples);
    r.rack = std::make_unique<PluginRack>(*r.host);
    auto t0 = Clock::now();
    r.rack->sync(p);
    std::vector<int32_t> out(EngineHost::block * 2);
    while (since(t0) < 60) {
        r.host->render(out.data(), EngineHost::block, 0.7f);
        auto st = r.rack->status(0);
        if (st == PluginStatus::Ready) { took = since(t0); return true; }
        if (st == PluginStatus::Failed || st == PluginStatus::Crashed) { why = r.rack->summary(); return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    why = "timed out";
    return false;
}

// What one host costs: RSS before it, once made, once loaded with one kit track, after a second of playing.
// lean=1: a print host (EngineHost::Lean, freeze plan F2a) with the delay blocks its one track needs.
int memory(const Args& a) {
    unsigned rate = unsigned(a.num("rate", 48000));
    bool lean = a.num("lean", 0) != 0;
    auto samples = bench_samples();
    long r0 = rss_kb();
    auto host = lean ? std::make_unique<EngineHost>(EngineHost::Lean {unsigned(a.num("delay_blocks", 2))}) : std::make_unique<EngineHost>();
    long r1 = rss_kb();
    auto p = std::make_unique<Project>();
    auto b = std::make_unique<PatternBank>();
    busy_project(*p, *b);
    keep_one(*p, *b, 0);
    host->set_render_threads(0);
    host->load(*p, *b);
    host->set_audio_format(rate, 384);
    long r2 = rss_kb();
    Command c {Command::Type::Reload};
    c.pack = new ProjectPack(*p, *b);
    host->send(c);
    for (unsigned s = 0; s < samples.size(); ++s) host->offer_sample(s, &samples[s]);
    host->send({Command::Type::Play});
    std::vector<int32_t> out(EngineHost::block * 2);
    for (unsigned n = 0; n < rate / EngineHost::block; ++n) { host->render(out.data(), EngineHost::block, 0.7f); drain(*host); }
    long r3 = rss_kb();
    std::printf("MELODEE_PRINT_MEMORY lean=%d sizeof_host_mb=%.1f base_mb=%.1f made_mb=%.1f loaded_mb=%.1f playing_mb=%.1f\n", int(lean),
                sizeof(EngineHost) / 1048576.0, r0 / 1024.0, (r1 - r0) / 1024.0, (r2 - r1) / 1024.0, (r3 - r2) / 1024.0);
    return 0;
}

int project(const Args& a) {
    std::string card = a.get("card", ""), name = a.get("name", "Print Test");
    std::string plugin = a.get("plugin", "Plugins/Ellic Studio - VST - Melodee Poly");
    if (card.empty()) { std::fprintf(stderr, "print-spike project: card=DIR (the card's top)\n"); return 1; }
    auto p = std::make_unique<Project>();
    auto b = std::make_unique<PatternBank>();
    busy_project(*p, *b);
    std::string samples = card + "/Samples/" + name;
    mkdir((card + "/Samples").c_str(), 0755);
    mkdir(samples.c_str(), 0755);
    auto data = bench_samples();
    const char* names[] {"Tone", "Noise", "Sweep", "Thump"};
    for (unsigned s = 0; s < 4; ++s) {
        std::string path = samples + "/" + names[s] + ".wav";
        std::string error = write_wav(path, data[s]);
        if (!error.empty()) { std::printf("MELODEE_PRINT_PROJECT failed=%s\n", error.c_str()); return 1; }
        std::snprintf(p->samples[s].path, sizeof(p->samples[s].path), "%s", path.c_str());
        std::snprintf(p->samples[s].name, sizeof(p->samples[s].name), "%s", names[s]);
        p->samples[s].frames = data[s].frames;
    }
    // Track 16: the plug-in, its own arpeggio (plugin_project's) in place of the kit.
    auto poly = std::make_unique<Project>();
    auto poly_bank = std::make_unique<PatternBank>();
    plugin_project(*poly, *poly_bank, plugin, false);
    p->plugins[0] = poly->plugins[0];
    std::snprintf(p->plugins[0].name, sizeof(p->plugins[0].name), "%s", fs_name(plugin).c_str());
    p->tracks[15] = poly->tracks[0];
    b->at[15][0] = poly_bank->at[0][0];
    std::snprintf(p->name, sizeof(p->name), "%s", name.c_str());
    std::string dir = card + "/Melodee/Projects", file = dir + "/" + name + ".mldp";
    mkdir((card + "/Melodee").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    auto bytes = save_project(*p, *b);
    FILE* f = std::fopen(file.c_str(), "wb");
    if (!f || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) { std::printf("MELODEE_PRINT_PROJECT failed=%s\n", file.c_str()); return 1; }
    std::fclose(f);
    std::printf("MELODEE_PRINT_PROJECT file=%s bytes=%zu\n", file.c_str(), bytes.size());
    return 0;
}

int equal(const Args& a) {
    unsigned rate = unsigned(a.num("rate", 48000)), blocks = unsigned(a.num("seconds", 8) * rate / EngineHost::block);
    auto samples = bench_samples();
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    busy_project(*project, *bank);
    const unsigned B = EngineHost::block, rows = track_count * B;
    std::vector<float> whole(size_t(rows) * blocks), stems(rows), alone(rows);
    std::vector<int32_t> out(B * 2);
    {
        auto host = start_host(*project, *bank, rate, samples);
        host->send({Command::Type::Play});
        for (unsigned n = 0; n < blocks; ++n) { host->render(out.data(), B, 0.7f, nullptr, whole.data() + size_t(n) * rows); drain(*host); }
    }
    unsigned same = 0;
    for (unsigned t = 0; t < track_count; ++t) {
        auto p = std::make_unique<Project>(*project);
        auto b = std::make_unique<PatternBank>(*bank);
        keep_one(*p, *b, t);
        auto host = start_host(*p, *b, rate, samples);
        host->send({Command::Type::Play});
        double worst = 0, energy = 0;
        uint64_t differ = 0, first = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host->render(out.data(), B, 0.7f, nullptr, alone.data());
            drain(*host);
            const float* live = whole.data() + size_t(n) * rows + size_t(t) * B;
            for (unsigned f = 0; f < B; ++f) {
                double d = std::fabs(double(live[f]) - double(alone[size_t(t) * B + f]));
                energy += double(live[f]) * live[f];
                if (d > 0) { if (!differ) first = uint64_t(n) * B + f; ++differ; worst = std::max(worst, d); }
            }
        }
        same += differ == 0;
        std::printf("MELODEE_PRINT_EQUAL track=%u frames=%llu rms=%.5f differ=%llu first=%llu max_diff=%.3g\n", t + 1,
                    (unsigned long long)uint64_t(blocks) * B, std::sqrt(energy / (double(blocks) * B)), (unsigned long long)differ,
                    (unsigned long long)first, worst);
    }
    std::printf("MELODEE_PRINT_EQUAL identical=%u/%u\n", same, track_count);
    return same == track_count ? 0 : 1;
}

int speed(const Args& a) {
    unsigned rate = unsigned(a.num("rate", 48000)), t = unsigned(a.num("track", 1)) - 1;
    double seconds = a.num("seconds", 30);
    unsigned blocks = unsigned(seconds * rate / EngineHost::block);
    std::string plugin = a.get("plugin", "");
    auto samples = bench_samples();
    auto p = std::make_unique<Project>();
    auto b = std::make_unique<PatternBank>();
    Rig rig;
    double opened = 0;
    if (plugin.empty()) {
        busy_project(*p, *b);
        keep_one(*p, *b, t % track_count);
        rig.host = start_host(*p, *b, rate, samples);
    } else {
        plugin_project(*p, *b, plugin, a.num("effect", 0) != 0);
        std::string why;
        if (!open_rig(rig, *p, *b, rate, samples, opened, why)) { std::printf("MELODEE_PRINT_SPEED failed=%s\n", why.c_str()); return 1; }
    }
    if (a.num("idle", 0) != 0) { sched_param none {}; sched_setscheduler(0, SCHED_IDLE, &none); } // only what nothing else wants
    else setpriority(PRIO_PROCESS, 0, 10); // below the app and its audio thread, as a print engine would run
    rig.host->send({Command::Type::Play});
    std::vector<int32_t> out(EngineHost::block * 2);
    std::vector<double> took;
    took.reserve(blocks);
    rusage r0 {}, r1 {};
    getrusage(RUSAGE_SELF, &r0);
    auto t0 = Clock::now();
    for (unsigned n = 0; n < blocks; ++n) {
        auto b0 = Clock::now();
        rig.host->render(out.data(), EngineHost::block, 0.7f);
        took.push_back(since(b0) * 1e6);
        drain(*rig.host);
    }
    double wall = since(t0);
    getrusage(RUSAGE_SELF, &r1);
    double cpu = double(r1.ru_utime.tv_sec - r0.ru_utime.tv_sec + r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) +
                 double(r1.ru_utime.tv_usec - r0.ru_utime.tv_usec + r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1e6;
    std::sort(took.begin(), took.end());
    std::printf("MELODEE_PRINT_SPEED source=%s seconds=%.1f wall=%.2f times_real=%.2f cpu_pct=%.0f block_us_p50=%.0f p99=%.0f max=%.0f "
                "period_us=%.0f opened_s=%.2f rss_mb=%.1f\n",
                plugin.empty() ? ("track" + std::to_string(t + 1)).c_str() : plugin.c_str(), seconds, wall, seconds / wall, 100 * cpu / wall,
                took[took.size() / 2], took[took.size() * 99 / 100], took.back(), 1e6 * EngineHost::block / rate, opened, rss_kb() / 1024.0);
    return 0;
}

int plugin(const Args& a) {
    std::string path = a.get("plugin", "");
    if (path.empty()) { std::fprintf(stderr, "print-spike plugin: plugin=PATH\n"); return 1; }
    unsigned rate = unsigned(a.num("rate", 48000)), blocks = unsigned(a.num("seconds", 8) * rate / EngineHost::block);
    bool effect = a.num("effect", 0) != 0;
    auto samples = bench_samples();
    auto p = std::make_unique<Project>();
    auto b = std::make_unique<PatternBank>();
    plugin_project(*p, *b, path, effect);
    long avail0 = meminfo_kb("MemAvailable:"), rss0 = rss_kb();
    Rig one, two;
    double took1 = 0, took2 = 0;
    std::string why;
    std::fprintf(stderr, "spike: first\n");
    if (!open_rig(one, *p, *b, rate, samples, took1, why)) { std::printf("MELODEE_PRINT_PLUGIN first failed=%s\n", why.c_str()); return 1; }
    long avail1 = meminfo_kb("MemAvailable:"), rss1 = rss_kb();
    std::fprintf(stderr, "spike: second (first %.2f s)\n", took1);
    if (!open_rig(two, *p, *b, rate, samples, took2, why)) { std::printf("MELODEE_PRINT_PLUGIN second failed=%s\n", why.c_str()); return 1; }
    long avail2 = meminfo_kb("MemAvailable:"), rss2 = rss_kb();
    std::fprintf(stderr, "spike: rendering (second %.2f s)\n", took2);
    // Both from the start, block for block: the same notes into the same plug-in should come out the same.
    one.host->send({Command::Type::Play});
    two.host->send({Command::Type::Play});
    std::vector<int32_t> o1(EngineHost::block * 2), o2(EngineHost::block * 2);
    uint64_t differ = 0, frames = 0;
    double worst = 0, energy = 0;
    for (unsigned n = 0; n < blocks; ++n) {
        one.host->render(o1.data(), EngineHost::block, 0.7f);
        two.host->render(o2.data(), EngineHost::block, 0.7f);
        drain(*one.host); drain(*two.host);
        for (unsigned k = 0; k < EngineHost::block * 2; ++k) {
            double d = std::fabs(double(o1[k]) - double(o2[k])) / 2147483648.0;
            energy += (double(o1[k]) / 2147483648.0) * (double(o1[k]) / 2147483648.0);
            if (d > 0) { ++differ; worst = std::max(worst, d); }
        }
        frames += EngineHost::block;
        if (n % (rate / EngineHost::block) == 0) std::fprintf(stderr, "spike: %u s\n", n / (rate / EngineHost::block));
    }
    std::fprintf(stderr, "spike: done\n");
    std::printf("MELODEE_PRINT_PLUGIN plugin=%s effect=%d open1_s=%.2f open2_s=%.2f rss_first_mb=%.1f rss_second_mb=%.1f "
                "avail_first_mb=%.1f avail_second_mb=%.1f rms=%.5f differ=%llu/%llu max_diff=%.3g\n",
                path.c_str(), int(effect), took1, took2, (rss1 - rss0) / 1024.0, (rss2 - rss1) / 1024.0, (avail0 - avail1) / 1024.0,
                (avail1 - avail2) / 1024.0, std::sqrt(energy / double(frames * 2)), (unsigned long long)differ,
                (unsigned long long)(frames * 2), worst);
    return 0;
}

// The stream pager's job without the engine: each stream consumed in real time from a buffer of `hold` that one
// thread tops up from its file a chunk at a time, oldest-first.
int stream(const Args& a) {
    std::string dir = a.get("dir", "");
    if (dir.empty()) { std::fprintf(stderr, "print-spike stream: dir=DIR on the card\n"); return 1; }
    double seconds = a.num("seconds", 30), hold = a.num("hold_ms", 333) / 1000.0;
    unsigned prints = unsigned(a.num("prints", 16)), voices = unsigned(a.num("voices", 24));
    struct S { int fd = -1; uint64_t size = 0, at = 0; double rate = 0, bpf = 0, have = 0; };
    std::vector<S> streams;
    mkdir(dir.c_str(), 0755);
    std::vector<char> chunk(1 << 20);
    for (unsigned k = 0; k < chunk.size(); ++k) chunk[k] = char((k * 2654435761u) >> 24);
    auto make = [&](const std::string& name, double rate, double bpf) {
        std::string path = dir + "/" + name;
        uint64_t want = uint64_t((seconds + 2) * rate * bpf);
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || uint64_t(st.st_size) < want) {
            FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) return false;
            for (uint64_t w = 0; w < want; w += chunk.size()) std::fwrite(chunk.data(), 1, std::min<uint64_t>(chunk.size(), want - w), f);
            std::fclose(f);
        }
        S s;
        s.fd = open(path.c_str(), O_RDONLY);
        s.size = want; s.rate = rate; s.bpf = bpf;
        streams.push_back(s);
        return s.fd >= 0;
    };
    for (unsigned k = 0; k < prints; ++k) if (!make("print" + std::to_string(k) + ".raw", 44100, 8)) { std::printf("MELODEE_PRINT_STREAM no_file\n"); return 1; }
    for (unsigned k = 0; k < voices; ++k) if (!make("voice" + std::to_string(k) + ".raw", 48000, 4)) { std::printf("MELODEE_PRINT_STREAM no_file\n"); return 1; }
    sync();
    if (FILE* f = std::fopen("/proc/sys/vm/drop_caches", "w")) { std::fputs("3", f); std::fclose(f); } // read from the card, not RAM
    std::vector<char> buffer(4 << 20);
    std::vector<double> reads;
    double lowest = 1e9, bytes = 0;
    unsigned under = 0;
    // Each starts with its buffer full (a stream is opened ahead of its first frame).
    for (auto& s : streams) {
        size_t n = size_t(hold * s.rate * s.bpf);
        ssize_t got = pread(s.fd, buffer.data(), n, off_t(s.at));
        s.at += uint64_t(std::max<ssize_t>(got, 0)); s.have = hold;
    }
    auto t0 = Clock::now();
    double last = 0;
    while (true) {
        double now = since(t0);
        if (now >= seconds) break;
        double used = now - last;
        last = now;
        S* neediest = nullptr;
        for (auto& s : streams) {
            bool had = s.have > 0;
            s.have -= used;
            if (s.have <= 0) { under += had; s.have = 0; } // a stream running dry, once each time
            lowest = std::min(lowest, s.have);
            if (s.have < hold * 0.5 && (!neediest || s.have < neediest->have)) neediest = &s;
        }
        if (!neediest) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        size_t n = size_t(hold * 0.5 * neediest->rate * neediest->bpf);
        if (neediest->at + n > neediest->size) neediest->at = 0;
        auto r0 = Clock::now();
        ssize_t got = pread(neediest->fd, buffer.data(), n, off_t(neediest->at));
        reads.push_back(since(r0) * 1000);
        if (got > 0) { neediest->at += uint64_t(got); bytes += double(got); neediest->have += double(got) / (neediest->rate * neediest->bpf); }
    }
    for (auto& s : streams) close(s.fd);
    std::sort(reads.begin(), reads.end());
    std::printf("MELODEE_PRINT_STREAM prints=%u voices=%u seconds=%.0f hold_ms=%.0f reads=%zu mb_s=%.1f read_ms_p50=%.1f p99=%.1f max=%.1f "
                "lowest_ms=%.0f underruns=%u\n",
                prints, voices, seconds, hold * 1000, reads.size(), bytes / seconds / 1e6, reads.empty() ? 0 : reads[reads.size() / 2],
                reads.empty() ? 0 : reads[reads.size() * 99 / 100], reads.empty() ? 0 : reads.back(), lowest * 1000, under);
    return under ? 1 : 0;
}

} // namespace

int print_spike(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "--melodee-print-spike equal|speed|plugin|stream [KEY=VALUE...]\n"); return 1; }
    Args a;
    for (int n = 3; n < argc; ++n) {
        std::string arg = argv[n];
        auto eq = arg.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "print-spike: key=value, not %s\n", argv[n]); return 1; }
        a.v[arg.substr(0, eq)] = arg.substr(eq + 1);
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // a line at a time: a plug-in that hangs closing keeps what was found
    std::string mode = argv[2];
    if (mode == "equal") return equal(a);
    if (mode == "memory") return memory(a);
    if (mode == "speed") return speed(a);
    if (mode == "plugin") return plugin(a);
    if (mode == "stream") return stream(a);
    if (mode == "project") return project(a);
    std::fprintf(stderr, "print-spike: no mode %s\n", mode.c_str());
    return 1;
}

} // namespace melodee
