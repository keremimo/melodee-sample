// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/print_engine.h"
#include "melodee/engine/effects.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/print_render.h"
#include "melodee/engine/sample_data.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/sample_library.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sched.h>

namespace melodee {
namespace fs = std::filesystem;

namespace {
using Clock = std::chrono::steady_clock;
double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

constexpr double plugin_wait_seconds = 60; // a plug-in not open by then fails the print
constexpr unsigned kept_lines = 8;

long rss_kb() {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) if (line.rfind("VmRSS:", 0) == 0) return std::atol(line.c_str() + 6);
    return 0;
}

// The effects' delay blocks a print of track `t` can take: its chain's and its pads' chains', and two to spare.
unsigned delay_blocks_for(const Track& t) {
    unsigned n = 2;
    auto count = [&](const Chain& c) { for (const auto& s : c.slots) n += s.used() && effect_needs_memory(EffectKind(s.kind)); };
    count(t.fx);
    for (const auto& c : t.pad_fx.at) count(c);
    return std::min(n, DelayPool::blocks);
}

std::string line_of(const PrintResult& r) {
    char text[320];
    if (!r.error.empty())
        std::snprintf(text, sizeof(text), "track=%u pattern=%s error=%s", r.record.track + 1u, print_pattern_name(r.record.pattern).c_str(),
                      r.error.c_str());
    else
        std::snprintf(text, sizeof(text), "track=%u pattern=%s %s rate=%u frames=%u pass=%u tap=%s tail=%s seconds=%.2f times_real=%.1f "
                      "host_mb=%.1f bytes=%llu file=%s",
                      r.record.track + 1u, print_pattern_name(r.record.pattern).c_str(), r.made ? "made" : "unchanged", r.record.rate, r.record.frames,
                      r.record.pass, tap_name(r.record.tap), tail_name(r.record.tail), r.seconds, r.times_real, r.host_mb,
                      (unsigned long long)r.bytes, r.record.file.c_str());
    return text;
}
} // namespace

PrintEngine::PrintEngine(SampleLibrary* samples) : library(samples) {
    thread = std::thread([this] { run(); });
}

PrintEngine::~PrintEngine() {
    {
        std::lock_guard<std::mutex> held(lock);
        stopping = true;
    }
    cancelled.store(true);
    wake.notify_all();
    if (thread.joinable()) thread.join();
}

bool PrintEngine::start(PrintJob job) {
    std::lock_guard<std::mutex> held(lock);
    if (running.load() || waiting) return false;
    cancelled.store(false);
    running.store(true, std::memory_order_release);
    waiting = std::make_unique<PrintJob>(std::move(job));
    wake.notify_all();
    return true;
}

void PrintEngine::cancel() { cancelled.store(true); }

bool PrintEngine::take(PrintResult& out) {
    std::lock_guard<std::mutex> held(lock);
    if (results.empty()) return false;
    out = std::move(results.front());
    results.pop_front();
    return true;
}

void PrintEngine::note(const std::string& line) {
    std::lock_guard<std::mutex> held(lock);
    lines.push_back(line);
    while (lines.size() > kept_lines) lines.pop_front();
}

std::string PrintEngine::status() const {
    std::lock_guard<std::mutex> held(lock);
    std::string s = running.load() ? "printing " + doing : "idle";
    for (const auto& l : lines) s += " | " + l; // one line: ctl answers a line
    return s;
}

void PrintEngine::run() {
#if defined(__linux__)
    sched_param none {};
    sched_setscheduler(0, SCHED_IDLE, &none); // only the time nothing else wants: the audio never waits for a print
#endif
    while (true) {
        std::unique_ptr<PrintJob> job;
        {
            std::unique_lock<std::mutex> held(lock);
            wake.wait(held, [&] { return stopping || waiting; });
            if (stopping) return;
            job = std::move(waiting);
        }
        auto samples = library ? library->hold_samples() : std::array<const SampleData*, sample_slots> {};
        for (uint8_t pattern : job->patterns) {
            if (cancelled.load()) break;
            {
                std::lock_guard<std::mutex> held(lock);
                doing = "track=" + std::to_string(job->track + 1) + " pattern=" + print_pattern_name(pattern);
            }
            PrintResult r = print_one(*job, pattern, samples);
            std::lock_guard<std::mutex> held(lock);
            lines.push_back(line_of(r));
            while (lines.size() > kept_lines) lines.pop_front();
            results.push_back(std::move(r));
        }
        if (library) library->release_samples();
        std::lock_guard<std::mutex> held(lock);
        doing.clear();
        running.store(false, std::memory_order_release);
    }
}

PrintResult PrintEngine::print_one(const PrintJob& job, unsigned pattern, const std::array<const SampleData*, sample_slots>& samples) {
    PrintResult r;
    r.project_path = job.project_path;
    r.record.track = uint8_t(job.track % track_count);
    r.record.pattern = uint8_t(pattern % pattern_count);
    r.record.tap = job.tap;
    r.record.tail = job.tail;
    r.record.file = print_file_name(job.track, pattern);
    const unsigned t = r.record.track;
    auto t0 = Clock::now();
    auto project = std::make_unique<Project>(*job.project);
    auto bank = std::make_unique<PatternBank>(*job.bank);
    print_project(*project, *bank, t, pattern);
    const Track& track = project->tracks[t];
    if (!track.used() || track.engine == EngineKind::Midi) { r.error = "nothing to print on this track"; return r; }
    const float rate = dsp::sample_rate();
    r.record.rate = unsigned(std::lround(rate));
    r.record.hash = print_hash(*project, *bank, job.states, job.tap, job.tail, r.record.rate);
    const std::string folder = prints_folder(job.project_path);
    const fs::path path = fs::path(folder) / r.record.file;
    std::error_code e;
    if (const auto* known = job.known.find(t, pattern); known && known->hash == r.record.hash && known->file == r.record.file &&
                                                       fs::exists(path, e)) {
        r.record = *known; // unchanged: never printed twice
        return r;
    }

    long rss0 = rss_kb();
    auto host = std::make_unique<EngineHost>(EngineHost::Lean {delay_blocks_for(track)});
    host->load(*project, *bank);
    host->set_audio_format(rate, EngineHost::block);
    Command reload {Command::Type::Reload};
    reload.pack = new ProjectPack(*project, *bank);
    host->send(reload);
    // An instrument streamed from the card: its pager runs, and the print goes no faster than real time, as the
    // live pager's reads would (a print faster than that would outrun the card's reads; freeze plan F2b).
    bool streams = false;
    for (unsigned n = 0; n < sample_slots; ++n) streams |= project->samples[n].used() && samples[n] && samples[n]->multi;
    if (streams) host->streams().start();
    std::vector<int32_t> out(EngineHost::block * 2);
    auto idle = [&] { // a silent block: offers taken, packs handed back
        host->render(out.data(), EngineHost::block, 1.0f);
        const ProjectPack* done = nullptr;
        while (host->take_done_pack(done)) delete done;
    };
    for (unsigned n = 0; n < sample_slots; ++n) { // the slots it plays; the offers' queue taken a block at a time
        if (!project->samples[n].used() || !samples[n]) continue;
        while (!host->offer_sample(n, samples[n])) idle();
    }
    idle();
    std::unique_ptr<PluginRack> rack;
    bool plugins = false;
    for (const auto& ref : project->plugins) plugins |= ref.used();
    if (plugins) { // its own rack: the plug-ins opened again, staged apart from the live rack's
        rack = std::make_unique<PluginRack>(*host, "print-");
        rack->set_card(job.card_top);
        rack->set_states(job.states);
        rack->sync(*project);
        auto w0 = Clock::now();
        while (r.error.empty()) {
            idle();
            bool ready = true;
            for (unsigned entry = 0; entry < max_plugins; ++entry) {
                if (!project->plugins[entry].used()) continue;
                auto s = rack->status(entry);
                if (s == PluginStatus::Failed || s == PluginStatus::Crashed) r.error = "plug-in did not open: " + rack->summary();
                ready &= s == PluginStatus::Ready;
            }
            if (ready) break;
            if (cancelled.load()) r.error = "stopped";
            else if (since(w0) > plugin_wait_seconds) r.error = "plug-in did not open in time";
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        idle(); // the plug-in offered is in place
    }
    r.host_mb = double(rss_kb() - rss0) / 1024.0;
    PrintAudio audio;
    double rendering = 0;
    if (r.error.empty()) {
        uint32_t ticks = played_pattern(bank->at[t][pattern], track.speed).length;
        auto r0 = Clock::now();
        uint64_t blocks = 0;
        audio = render_print(*host, t, ticks, job.tap, job.tail, [&] {
            if (streams) {
                double ahead = double(blocks) * EngineHost::block / rate - since(r0);
                if (ahead > 0) std::this_thread::sleep_for(std::chrono::duration<double>(ahead));
            }
            ++blocks;
            return !cancelled.load();
        });
        rendering = since(r0);
        r.error = audio.error;
        if (r.error.empty() && host->streams().underruns()) r.error = "the card's reads came late";
    }
    if (streams) host->streams().stop();
    rack.reset(); // its plug-ins freed (the host renders no more)
    host.reset();
    if (!r.error.empty()) { r.seconds = since(t0); return r; }

    // At the device's rate, as it played (never converted); written beside, then put in place.
    const auto& left = audio.left;
    const auto& right = audio.right;
    r.record.frames = uint32_t(left.size());
    r.record.pass = model_frames_of(audio.pass, rate);
    fs::create_directories(folder, e);
    const fs::path part = fs::path(folder) / (r.record.file + ".part");
    WavStream wav;
    bool written = wav.open(part.string(), r.record.rate, 32);
    std::vector<float> interleaved(size_t(EngineHost::block) * 2);
    for (size_t at = 0; written && at < left.size(); at += EngineHost::block) {
        unsigned n = unsigned(std::min<size_t>(EngineHost::block, left.size() - at));
        for (unsigned f = 0; f < n; ++f) { interleaved[2 * f] = left[at + f]; interleaved[2 * f + 1] = right[at + f]; }
        written = wav.add(interleaved.data(), n);
    }
    written = wav.close() && written;
    if (written) fs::rename(part, path, e);
    if (!written || e) { fs::remove(part, e); r.error = "could not write " + r.record.file; r.seconds = since(t0); return r; }
    r.bytes = uint64_t(fs::file_size(path, e));
    r.made = true;
    r.seconds = since(t0);
    r.times_real = rendering > 0 ? double(audio.rendered) / rate / rendering : 0;
    return r;
}

} // namespace melodee
