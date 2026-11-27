// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/print_engine.h"
#include "melodee/engine/auto_probe.h"
#include "melodee/engine/effects.h"
#include "melodee/engine/external_notes.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/print_player.h"
#include "melodee/engine/print_render.h"
#include "melodee/engine/release_fit.h"
#include "melodee/engine/loop_find.h"
#include "melodee/engine/stream_pager.h"
#include "melodee/engine/sample_data.h"
#include "melodee/model/scales.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/mpc3_export.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/xpm_write.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <set>
#include <unistd.h>

namespace melodee {
namespace fs = std::filesystem;

namespace {
// Freeze plan F2d: threads a print's plug-ins start (TONE3000's workers and model loaders) run at SCHED_IDLE like
// the print thread, not at the priority of the thread that opened them (the plug-ins' message thread): at normal
// priority they took turns with the live copy's own workers, which its audio waits for. The process's threads now,
// and those not in `before` put at SCHED_IDLE.
std::set<long> task_ids() {
    std::set<long> out;
    std::error_code e;
    for (const auto& entry : fs::directory_iterator("/proc/self/task", e)) out.insert(std::atol(entry.path().filename().c_str()));
    return out;
}
void idle_new_threads(const std::set<long>& before, std::set<long>& done) {
#if defined(__linux__)
    sched_param none {};
    for (long tid : task_ids())
        if (!before.count(tid) && done.insert(tid).second) sched_setscheduler(pid_t(tid), SCHED_IDLE, &none);
#else
    (void)before; (void)done;
#endif
}
using Clock = std::chrono::steady_clock;
double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

constexpr double plugin_wait_seconds = 60; // a plug-in not open by then fails the print
constexpr unsigned kept_lines = 8;
constexpr unsigned print_data_at = 4096; // freeze plan F2b: a print's frames start on a page (WavStream's data_at)

// A print file's frames: where they start and how many bytes, if it is `channels` (2, or more with a kit's pad sends
// beside it) of 32-bit float at `rate` with them on a page's boundary (as print_one() writes it); false otherwise (an
// older print: printed again).
bool print_layout(const std::string& path, unsigned rate, unsigned channels, uint64_t& offset, uint64_t& bytes) {
    std::ifstream in(path, std::ios::binary);
    unsigned char h[12];
    if (!in.read(reinterpret_cast<char*>(h), 12) || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) return false;
    auto u32 = [](const unsigned char* b) { return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24; };
    bool format = false;
    uint64_t at = 12;
    while (in.read(reinterpret_cast<char*>(h), 8)) {
        uint32_t size = u32(h + 4);
        if (!std::memcmp(h, "fmt ", 4)) {
            unsigned char f[16];
            if (size < 16 || !in.read(reinterpret_cast<char*>(f), 16)) return false;
            format = (f[0] | f[1] << 8) == 3 && unsigned(f[2] | f[3] << 8) == channels && u32(f + 4) == rate && (f[14] | f[15] << 8) == 32;
            in.seekg(std::streamoff(at + 8 + size + (size & 1)));
        } else if (!std::memcmp(h, "data", 4)) {
            offset = at + 8; bytes = size;
            return format && offset % print_data_at == 0;
        } else {
            in.seekg(std::streamoff(at + 8 + size + (size & 1)));
        }
        at += 8 + uint64_t(size) + (size & 1);
    }
    return false;
}

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
    if (t.engine == EngineKind::Plugin) n += 2; // its performance knobs' echo and reverb (engine/macros.h)
    return std::min(n, DelayPool::blocks);
}

// A print host (EngineHost::Lean) holding `project` (a print_project()), its samples offered and its plug-ins open
// in a rack of its own, staged apart from the live rack's, at `states`. `error` says why not.
struct PrintHost {
    std::unique_ptr<EngineHost> host;
    std::unique_ptr<PluginRack> rack;
    bool streams = false; // an instrument streamed from the card: its pager runs
    std::set<long> threads_before, threads_idle; // freeze plan F2d: the print's plug-ins' threads at SCHED_IDLE
    std::string error;
    PrintHost() = default;
    PrintHost(PrintHost&&) = default;
    PrintHost& operator=(PrintHost&&) = default;
    ~PrintHost() {
        if (streams && host) host->streams().stop();
        rack.reset(); // its plug-ins freed before the host
        host.reset();
    }
};

PrintHost open_print_host(const Project& project, const PatternBank& bank, const Track& track,
                          const std::array<const SampleData*, sample_slots>& samples,
                          const std::array<std::vector<uint8_t>, max_plugins>& states, const std::string& card_top,
                          const std::atomic<bool>& cancelled) {
    PrintHost h;
    h.host = std::make_unique<EngineHost>(EngineHost::Lean {delay_blocks_for(track)});
    auto& host = h.host;
    host->load(project, bank);
    host->set_audio_format(dsp::sample_rate(), EngineHost::block);
    Command reload {Command::Type::Reload};
    reload.pack = new ProjectPack(project, bank);
    host->send(reload);
    // An instrument streamed from the card: its pager runs, and the print goes no faster than real time, as the
    // live pager's reads would (a print faster than that would outrun the card's reads; freeze plan F2b).
    for (unsigned n = 0; n < sample_slots; ++n) h.streams |= project.samples[n].used() && samples[n] && samples[n]->multi;
    if (h.streams) host->streams().start();
    std::vector<int32_t> out(EngineHost::block * 2);
    auto idle = [&] { // a silent block: offers taken, packs handed back
        host->render(out.data(), EngineHost::block, 1.0f);
        const ProjectPack* done = nullptr;
        while (host->take_done_pack(done)) delete done;
    };
    for (unsigned n = 0; n < sample_slots; ++n) { // the slots it plays; the offers' queue taken a block at a time
        if (!project.samples[n].used() || !samples[n]) continue;
        while (!host->offer_sample(n, samples[n])) idle();
    }
    idle();
    bool plugins = false;
    for (const auto& ref : project.plugins) plugins |= ref.used();
    if (plugins) { // its own rack: the plug-ins opened again, staged apart from the live rack's
        start_plugin_messages(); // a thread of the live rack's, not one of the print's
        h.threads_before = task_ids();
        h.rack = std::make_unique<PluginRack>(*host, "print-");
        h.rack->set_card(card_top);
        h.rack->set_states(states);
        h.rack->sync(project);
        auto w0 = Clock::now();
        while (h.error.empty()) {
            idle();
            bool ready = true;
            for (unsigned entry = 0; entry < max_plugins; ++entry) {
                if (!project.plugins[entry].used()) continue;
                auto st = h.rack->status(entry);
                if (st == PluginStatus::Failed || st == PluginStatus::Crashed) h.error = "plug-in did not open: " + h.rack->summary();
                ready &= st == PluginStatus::Ready;
            }
            if (ready) break;
            if (cancelled.load()) h.error = "stopped";
            else if (since(w0) > plugin_wait_seconds) h.error = "plug-in did not open in time";
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        idle(); // the plug-in offered is in place
        idle_new_threads(h.threads_before, h.threads_idle);
    }
    return h;
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
    for (auto& r : ready) trash.push_back(r.pack);
    ready.clear();
    empty_trash();
}

bool PrintEngine::take_ready(FreezeReady& out) {
    std::lock_guard<std::mutex> held(lock);
    if (ready.empty()) return false;
    out = std::move(ready.front());
    ready.pop_front();
    return true;
}

void PrintEngine::discard(FreezePack* pack) {
    if (!pack) return;
    std::lock_guard<std::mutex> held(lock);
    trash.push_back(pack);
    wake.notify_all();
}

void PrintEngine::empty_trash() {
    std::vector<FreezePack*> going;
    {
        std::lock_guard<std::mutex> held(lock);
        going.swap(trash);
    }
    if (going.empty()) return;
    if (pager) pager->flush(); // the pager has seen every close the audio thread sent: no pin of theirs is taken after
    for (auto* pack : going) delete pack; // their files' mappings go with them
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

void PrintEngine::post(const Command& command, uint64_t at) {
    std::lock_guard<std::mutex> held(lock);
    auto place = outbox.end();
    while (place != outbox.begin() && std::prev(place)->first > at) --place; // by frame; equal frames in the order posted
    outbox.insert(place, {at, command});
}

void PrintEngine::forget() {
    std::lock_guard<std::mutex> held(lock);
    outbox.clear();
}

bool PrintEngine::due(uint64_t now, Command& out) {
    std::lock_guard<std::mutex> held(lock);
    if (outbox.empty() || outbox.front().first > now) return false;
    out = outbox.front().second;
    outbox.pop_front();
    return true;
}

bool PrintEngine::take_probed(AutoProbeResult& out) {
    std::lock_guard<std::mutex> held(lock);
    if (probed.empty()) return false;
    out = std::move(probed.front());
    probed.pop_front();
    return true;
}

bool PrintEngine::take_sampled(AutoSampleResult& out) {
    std::lock_guard<std::mutex> held(lock);
    if (sampled.empty()) return false;
    out = std::move(sampled.front());
    sampled.pop_front();
    return true;
}

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

PrintProgress PrintEngine::progress() const {
    std::lock_guard<std::mutex> held(lock);
    return shown;
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
            wake.wait(held, [&] { return stopping || waiting || !trash.empty(); });
            if (stopping) return;
            job = std::move(waiting);
        }
        empty_trash();
        if (!job) continue;
        PrintTable known = job->known; // with each print made or found unchanged, for a freeze's pack
        bool failed = false;
        {
            std::lock_guard<std::mutex> held(lock);
            shown = PrintProgress {};
            shown.busy = true; shown.track = uint8_t(job->track); shown.freeze = job->freeze; shown.serial = job->serial;
            shown.count = uint16_t(job->patterns.size());
        }
        auto samples = library ? library->hold_samples() : std::array<const SampleData*, sample_slots> {};
        if (job->probing) { // the auto sampler's check: a few notes heard, nothing kept
            AutoProbeResult heard = probe_track(*job, samples);
            if (library) library->release_samples();
            std::lock_guard<std::mutex> held(lock);
            const auto& f = heard.found;
            lines.push_back("autosample-check track=" + std::to_string(job->track + 1) + " " +
                            (heard.error.empty() ? "kind=" + std::string(f.kind == AutoKind::Mono ? "mono" : f.kind == AutoKind::Drums ? "drums" : "keys")
                                                       + " glide_ms=" + std::to_string(int(std::lround(f.glide * 1000))) + " legato=" + std::to_string(f.legato) + " tied=" + std::to_string(f.tied)
                                                       + " accent=" + std::to_string(f.accent) + " takes=" + std::to_string(f.takes)
                                                       + " sustains=" + std::to_string(f.sustains) + " moves=" + std::to_string(f.moves) + " hold_s=" + std::to_string(int(std::lround(f.hold * 10)) / 10) + "." + std::to_string(int(std::lround(f.hold * 10)) % 10) + " release_ms=" + std::to_string(int(std::lround(f.release * 1000)))
                                                       + " sounds=" + std::to_string(f.sounding()) + heard.heard
                                                 : "error=" + heard.error));
            while (lines.size() > kept_lines) lines.pop_front();
            probed.push_back(std::move(heard));
            doing.clear();
            shown.busy = false; shown.checking = false; shown.pattern = -1;
            running.store(false, std::memory_order_release);
            continue;
        }
        if (job->autosample) { // the auto sampler: notes, not patterns
            std::string check_failed; // its check found no sound, an input that clips, or was called off: nothing is sampled
            std::unique_ptr<NoteSource> checked; // an instrument outside: the check's notes, sampled with next
            int check_key = -1;
            if (job->probe_first) { // as the sheet does: the check first, its findings sampled with
                AutoProbeResult heard = probe_track(*job, samples, job->external ? &checked : nullptr);
                if (heard.error.empty()) job->sampling.found = heard.found;
                check_failed = heard.error;
                check_key = heard.key;
                std::lock_guard<std::mutex> held(lock);
                const auto& f = heard.found;
                lines.push_back("autosample-check track=" + std::to_string(job->track + 1) + " kind=" +
                                std::string(f.kind == AutoKind::Mono ? "mono" : f.kind == AutoKind::Drums ? "drums" : "keys") +
                                " glide_ms=" + std::to_string(int(std::lround(f.glide * 1000))) + " legato=" + std::to_string(f.legato) + " tied=" + std::to_string(f.tied) +
                                " accent=" + std::to_string(f.accent) + " takes=" + std::to_string(f.takes) + " sustains=" + std::to_string(f.sustains) + " moves=" + std::to_string(f.moves) + " hold_s=" + std::to_string(int(std::lround(f.hold * 10)) / 10) + "." + std::to_string(int(std::lround(f.hold * 10)) % 10) + " release_ms=" + std::to_string(int(std::lround(f.release * 1000))) +
                                " sounds=" + std::to_string(f.sounding()) + (heard.error.empty() ? std::string() : " error=" + heard.error) + heard.heard);
                while (lines.size() > kept_lines) lines.pop_front();
                shown.checking = false;
            }
            AutoSampleResult made;
            if (check_failed.empty()) made = sample_track(*job, samples, std::move(checked));
            else {
                made.project_path = job->project_path; made.track = job->track % track_count; made.name = job->sample_name;
                made.error = check_failed;
                made.key = check_key;
            }
            if (library) library->release_samples();
            std::lock_guard<std::mutex> held(lock);
            lines.push_back("autosample track=" + std::to_string(job->track + 1) + " " +
                            (made.error.empty() ? "made=" + made.program + " samples=" + std::to_string(made.samples)
                                                      + " loops=" + std::to_string(made.loops) + " release_ms=" + std::to_string(int(std::lround(made.release * 1000)))
                                                      + (made.release_slow > 0 ? "+" + std::to_string(int(std::lround(made.release_slow * 1000))) + "@"
                                                                                     + std::to_string(int(std::lround(20 * std::log10(made.release_share)))) + "dB"
                                                                               : std::string())
                                                      + " centred=" + std::to_string(made.centred) + " mono=" + std::to_string(made.mono) + " layers=" + std::to_string(made.layers) + " " + made.release_note
                                                : "error=" + made.error) +
                            " seconds=" + std::to_string(int(std::lround(made.seconds))));
            while (lines.size() > kept_lines) lines.pop_front();
            sampled.push_back(std::move(made));
            doing.clear();
            shown.busy = false; shown.pattern = -1;
            running.store(false, std::memory_order_release);
            continue;
        }
        for (uint8_t pattern : job->patterns) {
            if (cancelled.load()) { failed = true; break; }
            empty_trash();
            {
                std::lock_guard<std::mutex> held(lock);
                doing = "track=" + std::to_string(job->track + 1) + " pattern=" + print_pattern_name(pattern);
                shown.pattern = int16_t(pattern); shown.fraction = 0;
            }
            PrintResult r = print_one(*job, pattern, samples);
            if (r.error.empty()) known.put(r.record);
            else failed = true;
            std::lock_guard<std::mutex> held(lock);
            auto& bits = r.error.empty() ? shown.done : shown.failed;
            bits[pattern / 64] |= uint64_t(1) << (pattern % 64);
            ++shown.through;
            shown.pattern = -1; shown.fraction = 0;
            lines.push_back(line_of(r));
            while (lines.size() > kept_lines) lines.pop_front();
            results.push_back(std::move(r));
        }
        if (library) library->release_samples();
        if (job->freeze) { // freeze plan F2b: the prints mapped for the audio thread, or why not
            FreezeReady f;
            if (failed) {
                f.project_path = job->project_path; f.track = job->track; f.serial = job->serial;
                f.error = cancelled.load() ? "stopped" : "a print failed";
            } else {
                std::lock_guard<std::mutex> held(lock);
                doing = "track=" + std::to_string(job->track + 1) + " mapping";
            }
            if (!failed) f = freeze_pack(*job, known);
            std::lock_guard<std::mutex> held(lock);
            lines.push_back("freeze track=" + std::to_string(job->track + 1) + (f.pack ? " ready" : " error=" + f.error));
            while (lines.size() > kept_lines) lines.pop_front();
            ready.push_back(std::move(f));
        }
        std::lock_guard<std::mutex> held(lock);
        doing.clear();
        shown.busy = false; shown.pattern = -1;
        running.store(false, std::memory_order_release);
    }
}

// Freeze plan F2b: each print of the job's patterns mapped from the card, its first second and a chunk at each bar
// line pinned (StreamPager::hold) so the audio thread can start it at once there.
FreezeReady PrintEngine::freeze_pack(const PrintJob& job, const PrintTable& known) {
    FreezeReady f;
    f.project_path = job.project_path; f.track = job.track; f.serial = job.serial;
    auto pack = std::make_unique<FreezePack>();
    pack->track = uint8_t(job.track);
    pack->tap = job.tap;
    pack->deep = job.deep; // freeze plan F2d
    { // freeze plan F2c: what its prints were made at (a print whose hash still matches was made at these too)
        Mix m = print_mix(job.project->tracks[job.track % track_count]);
        pack->level = m.level; pack->pan = m.pan;
    }
    const unsigned rate = unsigned(std::lround(dsp::sample_rate()));
    const std::string folder = prints_folder(job.project_path);
    const Track& track = job.project->tracks[job.track % track_count];
    const unsigned bar = std::max(1u, job.project->bar_ticks());
    for (uint8_t p : job.patterns) {
        const PrintRecord* rec = known.find(job.track, p);
        if (!rec) { f.error = "no print of " + print_pattern_name(p); return f; }
        const std::string path = (fs::path(folder) / rec->file).string();
        uint64_t offset = 0, bytes = 0;
        if (rec->rate != rate || !print_layout(path, rate, rec->channels(), offset, bytes)) { f.error = "print " + rec->file + " unreadable"; return f; }
        int fd = ::open(path.c_str(), O_RDONLY);
        struct stat st {};
        if (fd < 0 || fstat(fd, &st) != 0 || uint64_t(st.st_size) < offset + bytes) { if (fd >= 0) ::close(fd); f.error = "cannot open " + rec->file; return f; }
        void* map = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (map == MAP_FAILED) { f.error = "cannot map " + rec->file; return f; }
        auto clip = std::make_unique<PrintClip>();
        clip->set_sends(rec->sends); // a kit's pad sends ride along in pairs after the print's own two
        const unsigned s = clip->stride;
        auto& part = clip->part;
        part.mapping = map; part.mapping_bytes = size_t(st.st_size); // unmapped with the clip
        clip->frames = uint32_t(std::min<uint64_t>(bytes / (4 * s), rec->frames));
        clip->rate = rate;
        clip->tail = rec->tail;
        clip->pass = uint32_t(std::llround(double(rec->pass) * rate / dsp::model_rate));
        part.left = part.right = reinterpret_cast<const float*>(static_cast<const char*>(map) + offset);
        part.total = s * clip->frames;
        if (!part.total) { f.error = "empty print " + rec->file; return f; }
        if (job.bank->at[job.track % track_count][p].empty()) { // without notes: kept only if its plug-in played something
            constexpr float floor = 3.16e-5f; // -90 dB, as a frozen track's engine counts as quiet below
            bool heard = false;
            for (uint32_t n = 0; n < part.total && !heard; ++n) heard = std::fabs(part.left[n]) >= floor;
            if (!heard) { f.silent[p / 64] |= uint64_t(1) << (p % 64); continue; } // the clip unmaps it
        }
        part.pins.assign((part.total - 1) / stream_chunk + 1, 0);
        part.frames.store(part.total);
        // Held: the first second, and two chunks from each bar line in the pass on.
        auto hold = [&](uint32_t from_float, uint32_t floats) {
            from_float -= from_float % stream_chunk;
            uint32_t to = uint32_t(std::min<uint64_t>(uint64_t(from_float) + floats, part.total));
            if (from_float >= to) return true;
            if (!pager || !pager->hold(part, from_float, to - from_float)) return false;
            uint32_t end = uint32_t(std::min<uint64_t>((uint64_t(to - 1) / stream_chunk + 1) * stream_chunk, part.total)); // whole chunks
            clip->held.push_back({from_float, end});
            return true;
        };
        bool held = hold(0, std::max<uint32_t>(s * rate, 2 * stream_chunk));
        const uint32_t ticks = std::max<uint32_t>(1, played_pattern(job.bank->at[job.track][p], track.speed).length);
        for (uint32_t b = bar; held && b < ticks; b += bar) {
            uint64_t frame = uint64_t(clip->pass) * b / ticks;
            if (frame >= uint64_t(rate)) held = hold(uint32_t(std::min<uint64_t>(s * frame, part.total)), 2 * stream_chunk);
        }
        if (!held) { f.error = "cannot hold " + rec->file + " in memory"; return f; }
        { // freeze plan F2c: its first pass in small for Stage, a few frames of each column read
            PrintPeaks peaks {};
            const uint32_t span = std::max<uint32_t>(1, std::min(clip->frames, clip->pass ? clip->pass : clip->frames));
            const float* data = part.left;
            for (unsigned col = 0; col < print_peak_columns; ++col) {
                uint32_t from = uint32_t(uint64_t(span) * col / print_peak_columns), to = uint32_t(uint64_t(span) * (col + 1) / print_peak_columns);
                uint32_t step = std::max<uint32_t>(1, (to - from) / 48);
                float most = 0;
                for (uint32_t f0 = from; f0 < to && f0 < clip->frames; f0 += step)
                    most = std::max({most, std::fabs(data[s * size_t(f0)]), std::fabs(data[s * size_t(f0) + 1])});
                peaks[col] = uint8_t(std::lround(std::clamp(most, 0.0f, 1.0f) * 255));
            }
            f.peaks.emplace_back(p, peaks);
        }
        pack->clips[p] = clip.get();
        pack->owned.push_back(std::move(clip));
    }
    f.pack = pack.release();
    return f;
}

PrintResult PrintEngine::print_one(const PrintJob& job, unsigned pattern, const std::array<const SampleData*, sample_slots>& samples) {
    PrintResult r;
    r.project_path = job.project_path;
    r.record.track = uint8_t(job.track % track_count);
    r.record.pattern = uint8_t(pattern % pattern_count);
    r.record.tap = job.tap;
    r.record.tail = job.tail;
    r.record.file = print_file_name(job.track, pattern);
    r.record.sends = print_sends(job.project->tracks[job.track % track_count]); // a kit's pad sends ride along
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
    uint64_t offset = 0, bytes = 0;
    if (const auto* known = job.known.find(t, pattern); known && known->hash == r.record.hash && known->file == r.record.file &&
                                                       known->sends == r.record.sends && fs::exists(path, e) &&
                                                       print_layout(path.string(), r.record.rate, known->channels(), offset, bytes)) {
        r.record = *known; // unchanged: never printed twice
        return r;
    }

    long rss0 = rss_kb();
    PrintHost opened = open_print_host(*project, *bank, track, samples, job.states, job.card_top, cancelled);
    r.error = opened.error;
    auto& host = opened.host;
    auto& rack = opened.rack;
    const bool streams = opened.streams;
    auto& threads_before = opened.threads_before;
    auto& threads_idle = opened.threads_idle;
    r.host_mb = double(rss_kb() - rss0) / 1024.0;
    PrintAudio audio;
    double rendering = 0;
    if (r.error.empty()) {
        uint32_t ticks = played_pattern(bank->at[t][pattern], track.speed).length;
        auto r0 = Clock::now();
        uint64_t blocks = 0, total = 0;
        unsigned left = 0; // the job's patterns after this one
        {
            std::lock_guard<std::mutex> held(lock);
            left = shown.count > shown.through + 1u ? shown.count - shown.through - 1u : 0;
        }
        audio = render_print(*host, t, ticks, job.tap, job.tail, [&] {
            if (streams) {
                double ahead = double(blocks) * EngineHost::block / rate - since(r0);
                if (ahead > 0) std::this_thread::sleep_for(std::chrono::duration<double>(ahead));
            }
            if (blocks == 1) { // the first block placed the tempo: what the print renders, two passes for Loop, a second's tail for Ring
                uint64_t pass = host->tick_frame(std::max<uint32_t>(ticks, 1));
                total = pass * (job.tail == PrintTail::Loop ? 2 : 1) + (job.tail == PrintTail::Ring ? uint64_t(rate) : 0);
            }
            if (total && (blocks & 7) == 0) { // the sheet's progress, every eighth block
                double rendered = double(blocks) * EngineHost::block, wall = since(r0);
                double speed = wall > 0 ? rendered / rate / wall : 0;
                std::lock_guard<std::mutex> held(lock);
                shown.fraction = float(std::min(1.0, rendered / double(total)));
                shown.times_real = float(speed);
                shown.seconds_left = speed > 0 ? float((std::max(0.0, double(total) - rendered) + double(left) * double(total)) / rate / speed) : 0;
            }
            ++blocks;
            if (rack && (blocks & 63) == 1) idle_new_threads(threads_before, threads_idle); // workers started on the way
            return !cancelled.load();
        }, r.record.sends);
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
    const unsigned channels = r.record.channels(); // the print's two, then a pair for each bus its pads send into
    bool written = wav.open(part.string(), r.record.rate, 32, print_data_at, channels); // its frames on a page, for a freeze's mapping
    std::vector<float> interleaved(size_t(EngineHost::block) * channels);
    for (size_t at = 0; written && at < left.size(); at += EngineHost::block) {
        unsigned n = unsigned(std::min<size_t>(EngineHost::block, left.size() - at));
        for (unsigned f = 0; f < n; ++f) {
            float* frame = interleaved.data() + size_t(channels) * f;
            frame[0] = left[at + f]; frame[1] = right[at + f];
            unsigned pair = 2;
            for (unsigned b = 0; b < 4; ++b) {
                if (!((r.record.sends >> b) & 1)) continue;
                frame[pair] = audio.send_left[b][at + f]; frame[pair + 1] = audio.send_right[b][at + f];
                pair += 2;
            }
        }
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

// ---- the auto sampler

// Where the auto sampler's notes are played and heard: a plug-in in a print host of its own, faster than real time,
// or an instrument outside over MIDI and an input, in real time (engine/external_notes.h).
struct NoteSource {
    using Heard = ExternalNotes::Heard;
    virtual ~NoteSource() = default;
    // Nothing playing, until what is left of the last note has died away (`quiet` seconds of silence, `most` at most).
    virtual void settle(float most, float quiet) = 0;
    virtual NoteAudio note(uint8_t key, uint8_t velocity, uint64_t hold, float most_tail, const Heard& heard) = 0;
    virtual std::vector<float> notes(std::vector<ProbeNote> notes, uint64_t frames) = 0;
    virtual std::string about() const { return {}; } // for `melodee-print status`
    virtual float silence() const { return 0.000316f; } // what counts as silent (-70 dB; an input's own noise and a little more)
    virtual std::string trouble() const { return {}; }  // what went wrong meanwhile, if anything
    virtual int trouble_key() const { return -1; }      // the key it is about (a clip), -1 none
};

namespace {
struct PluginNotes final : NoteSource {
    std::unique_ptr<Project> project;
    std::unique_ptr<PatternBank> bank;
    PrintHost opened;
    unsigned track = 0;
    PrintTap tap = PrintTap::PreFx;
    const std::atomic<bool>* cancelled = nullptr;
    std::vector<int32_t> out = std::vector<int32_t>(EngineHost::block * 2);
    void settle(float most, float quiet) override {
        auto& host = *opened.host;
        const float rate = dsp::sample_rate();
        unsigned still = 0;
        for (unsigned b = 0; b < unsigned(most * rate) / EngineHost::block && still < unsigned(quiet * rate); ++b) {
            host.render(out.data(), EngineHost::block, 1.0f);
            const ProjectPack* done = nullptr;
            while (host.take_done_pack(done)) delete done;
            const float* l = host.print_left();
            const float* r = host.print_right();
            bool loud = false;
            for (unsigned f = 0; f < EngineHost::block; ++f) loud |= std::fabs(l[f]) >= 0.000316f || std::fabs(r[f]) >= 0.000316f;
            still = loud ? 0 : still + EngineHost::block;
        }
    }
    NoteAudio note(uint8_t key, uint8_t velocity, uint64_t hold, float most_tail, const Heard& heard) override {
        auto& host = *opened.host;
        const float rate = dsp::sample_rate();
        unsigned blocks = 0;
        const auto r0 = Clock::now();
        return render_note(host, track, key, velocity, hold, tap, most_tail, [&] {
            bool go = !cancelled->load();
            if (blocks && heard) go = heard(host.print_left(), host.print_right(), EngineHost::block) && go; // the block rendered last
            if (opened.streams) { // an instrument streamed from the card: no faster than real time
                double ahead = double(blocks) * EngineHost::block / rate - since(r0);
                if (ahead > 0) std::this_thread::sleep_for(std::chrono::duration<double>(ahead));
            }
            ++blocks;
            if (opened.rack && (blocks & 63) == 1) idle_new_threads(opened.threads_before, opened.threads_idle);
            return go;
        });
    }
    std::vector<float> notes(std::vector<ProbeNote> notes, uint64_t frames) override {
        return render_notes(*opened.host, track, std::move(notes), frames, tap, [&] { return !cancelled->load(); });
    }
    std::string trouble() const override {
        return opened.streams && opened.host->streams().underruns() ? "the card's reads came late" : std::string();
    }
};

struct OutsideNotes final : NoteSource {
    struct Io final : ExternalIo {
        PrintEngine& engine;
        const RecallRing& ring;
        Io(PrintEngine& engine, const RecallRing& ring) : engine(engine), ring(ring) {}
        uint64_t now() override { return ring.written(); }
        void post(const Command& command, uint64_t at) override { engine.post(command, at); }
        void forget() override { engine.forget(); }
        bool copy(uint64_t from, uint32_t frames, float* left, float* right) override { return ring.copy(from, frames, left, right); }
        void wait(unsigned milliseconds) override { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
    };
    Io io;
    ExternalNotes played;
    const std::atomic<bool>* cancelled;
    OutsideNotes(PrintEngine& engine, const RecallRing& ring, unsigned track, const std::atomic<bool>* cancelled)
        : io(engine, ring), played(io, track, dsp::sample_rate()), cancelled(cancelled) {}
    void settle(float, float) override { played.settle([&] { return !cancelled->load(); }); }
    NoteAudio note(uint8_t key, uint8_t velocity, uint64_t hold, float most_tail, const Heard& heard) override {
        return played.note(key, velocity, hold, most_tail, [&](const float* l, const float* r, unsigned n) {
            return (!heard || heard(l, r, n)) && !cancelled->load();
        });
    }
    std::vector<float> notes(std::vector<ProbeNote> notes, uint64_t frames) override {
        return played.notes(std::move(notes), frames, [&] { return !cancelled->load(); });
    }
    float silence() const override { return played.silence(); }
    std::string about() const override {
        char text[64];
        std::snprintf(text, sizeof(text), "silence=%.0fdB delay=%.1fms", 20.0 * std::log10(double(played.silence())),
                      1000.0 * played.latency() / double(dsp::sample_rate()));
        return text;
    }
    std::string trouble() const override {
        if (!played.clipped()) return {};
        const int key = played.clipped_key();
        return (key >= 0 ? note_name(key) : std::string("the input")) + " clips: turn the instrument down";
    }
    int trouble_key() const override { return played.clipped() ? played.clipped_key() : -1; }
};

// The job's notes: its plug-in track's plug-in opened in a print host, or (a MIDI track) the instrument outside,
// its input listened to and a note of `key` tried. `error` says why not.
std::unique_ptr<NoteSource> open_notes(PrintEngine& engine, const PrintJob& job, const std::array<const SampleData*, sample_slots>& samples,
                                       uint8_t key, const std::atomic<bool>& cancelled, std::string& error) {
    const unsigned t = job.track % track_count;
    if (job.external) {
        if (job.project->tracks[t].engine != EngineKind::Midi) { error = "not a MIDI track"; return nullptr; }
        if (!job.input) { error = "no input"; return nullptr; }
        auto made = std::make_unique<OutsideNotes>(engine, *job.input, t, &cancelled);
        error = made->played.open(key, [&] { return !cancelled.load(); });
        if (!error.empty()) return nullptr;
        return made;
    }
    auto made = std::make_unique<PluginNotes>();
    made->project = std::make_unique<Project>(*job.project);
    made->bank = std::make_unique<PatternBank>(*job.bank);
    print_project(*made->project, *made->bank, t, made->project->tracks[t].pattern % pattern_count);
    const Track& track = made->project->tracks[t];
    if (track.engine != EngineKind::Plugin) { error = "not a plug-in track"; return nullptr; }
    made->opened = open_print_host(*made->project, *made->bank, track, samples, job.states, job.card_top, cancelled);
    if (!made->opened.error.empty()) { error = made->opened.error; return nullptr; }
    made->track = t;
    made->tap = job.sampling.post_fx ? PrintTap::PostFx : PrintTap::PreFx;
    made->cancelled = &cancelled;
    made->opened.host->set_print_tap(t, made->tap == PrintTap::PostFx);
    return made;
}

// A sample's name for a key: flats, so no "#" (Melodee's paths keep "#" for a program's pad), C4 = MIDI 60 as Melodee names it.
std::string key_word(unsigned key) {
    static const char* names[] {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    return std::string(names[key % 12]) + std::to_string(int(key / 12) - 1);
}
} // namespace

// The auto sampler's check (shell/auto_sample.h AutoFindings): a few notes on the track's plug-in, as it is set now,
// heard by engine/auto_probe.h. Its type from five keys (drums when their pitch does not follow them); a mono
// instrument from two keys pressed legato against the first alone, its glide from how the second's pitch moves, and
// from two keys apart whether it glides only legato; drums, the keys of the range that sound; the accent from one key
// at sixteen velocities; takes from three hits of it.
AutoProbeResult PrintEngine::probe_track(const PrintJob& job, const std::array<const SampleData*, sample_slots>& samples,
                                         std::unique_ptr<NoteSource>* keep) {
    AutoProbeResult r;
    r.project_path = job.project_path;
    r.track = job.track % track_count;
    const unsigned t = r.track;
    const auto& set = job.sampling;
    AutoFindings& found = r.found;
    auto show = [&](uint8_t step) {
        std::lock_guard<std::mutex> held(lock);
        shown.check_step = step;
        shown.found = found;
    };
    {
        std::lock_guard<std::mutex> held(lock);
        shown.autosample = true; shown.checking = true; shown.check_step = 0; shown.found = {};
        shown.opened = job.external; // an instrument outside has nothing to open
        doing = "track=" + std::to_string(t + 1) + " autosample-check";
    }
    const float rate = dsp::sample_rate();
    // Its test notes in the range, but never outside C2-C5, where a pitch reads clearly (a range at the top of the
    // keyboard is still a pitched instrument, not drums whose pitch could not be heard).
    const int low = set.low, high = std::max<int>(set.low, set.high);
    const uint8_t base = uint8_t(std::clamp(std::clamp(48, low, std::max(low, high - 12)), 36, 60));
    auto source = open_notes(*this, job, samples, base, cancelled, r.error);
    if (!source) return r;
    {
        std::lock_guard<std::mutex> held(lock);
        shown.opened = true;
    }
    // Before each go: silence, until what is left of the last has died away (a second at most).
    auto settle = [&] { source->settle(1.0f, 0.05f); };
    auto at = [&](double seconds) { return uint64_t(seconds * rate); };
    bool stopped = false;
    double played = 0, playing = 0; // seconds of notes and the time they took, for the sampling's time on the sheet
    auto play = [&](std::vector<ProbeNote> notes, double seconds) {
        if (stopped || cancelled.load()) { stopped = true; return std::vector<float> {}; }
        settle();
        const auto p0 = Clock::now();
        auto x = source->notes(std::move(notes), at(seconds));
        playing += since(p0);
        played += double(x.size()) / rate;
        if (x.empty()) stopped = true;
        return x;
    };
    auto hit = [&](uint8_t key, uint8_t velocity, double hold, double seconds) {
        return play({{0, true, key, velocity}, {at(hold), false, key, velocity}}, seconds);
    };
    auto stop = [&] { r.error = "stopped"; return r; };
    settle();

    // Type: five keys, their pitch read just past the attack.
    std::vector<uint8_t> keys;
    for (int k : {0, 1, 2, 5, 12}) keys.push_back(uint8_t(std::min(127, base + k)));
    std::vector<float> hz;
    for (uint8_t k : keys) {
        auto x = hit(k, 100, 0.3, 0.35);
        if (stopped) return stop();
        hz.push_back(probe_pitch(x, at(0.06), at(0.08), rate));
    }
    found.kind = probe_drums(keys, hz) ? AutoKind::Drums : AutoKind::Keys;
    show(1);
    uint8_t loud_key = base;
    if (found.kind == AutoKind::Drums) { // the keys of the range that sound, a short hit each
        found.scan_low = uint8_t(low); found.scan_high = uint8_t(high);
        found.hold = 2.0f; // a drum is cut where it falls silent, however long its key is held
        bool any = false;
        for (int k = low; k <= high; ++k) {
            auto x = hit(uint8_t(k), 100, 0.1, 0.15);
            if (stopped) return stop();
            float peak = 0;
            for (float v : x) peak = std::max(peak, std::fabs(v));
            if (peak < std::max(0.001f, 2.0f * source->silence())) continue;
            found.keys[unsigned(k) / 64] |= uint64_t(1) << (unsigned(k) % 64);
            if (!any || std::abs(k - int(base)) < std::abs(int(loud_key) - int(base))) loud_key = uint8_t(k);
            any = true;
        }
    } else {
        auto hz_of = [](int key) { return 440.0f * std::pow(2.0f, float(key - 69) / 12.0f); };
        auto fifth = [&](uint8_t a) { // `a` held, the fifth above pressed 0.4 s on, both let go at 1.2 s
            const uint8_t b = uint8_t(std::min(127, a + 7));
            return play({{0, true, a, 100}, {at(0.4), true, b, 100}, {at(1.2), false, a, 100}, {at(1.2), false, b, 100}}, 1.3);
        };
        // One sound, heard at key `a` (sounding `from_hz`) and the fifth above it: mono or poly, its glide, how it
        // holds and how it falls. `both_out`, `alone_out`: what it played, kept.
        auto check_zone = [&](uint8_t a, float from_hz, std::vector<float>* alone_out) {
            AutoZone z;
            const uint8_t b = uint8_t(std::min(127, a + 7));
            const float to_hz = from_hz * std::pow(2.0f, 7.0f / 12.0f);
            // Mono: the first key's pitch gone soon after a second is pressed legato, where alone it sounds on.
            auto both = fifth(a);
            // The first key alone, held 8 s: whether it sustains, whether it moves while held, how long it is heard.
            auto alone = play({{0, true, a, 100}, {at(8.0), false, a, 100}}, 8.1);
            if (stopped) return z;
            const ProbeGlide g = probe_glide(both, alone, at(0.4), from_hz, to_hz, rate);
            z.sustains = probe_sustains(alone, at(0.5), at(8.0), rate, 9.0f); // past its attack to the end of the 8 s: there still, or fading
            z.moves = z.sustains && sound_moves(alone, at(8.0), rate);
            // HOLD at Auto: a steady sound 2 s (a short loop is all it needs), one that moves 8 s (a long loop), one that
            // fades for as long as it is heard (so a key held long plays its whole fade).
            z.hold = z.moves ? 8.0f : z.sustains ? 2.0f : std::clamp(probe_fade_seconds(alone, rate) + 0.2f, 1.0f, 8.0f);
            { // How it falls let go early, while it is loud: the key held half a second, then up to 4 s of what follows.
                const double tail = std::clamp<double>(set.tail, 0.5, 4.0);
                auto let_go = play({{0, true, a, 100}, {at(0.5), false, a, 100}}, 0.5 + tail);
                if (stopped) return z;
                const size_t last = size_t(at(0.1));
                const float quiet = job.external ? 20.0f * std::log10(source->silence()) : -100.0f; // an input's own noise
                const bool cut = let_go.size() > last && probe_db(let_go, let_go.size() - last, last) > quiet + 15.0f; // still sounding at the end
                const ReleaseShape shape = release_shape(let_go, let_go, at(0.5), rate, cut, quiet);
                if (!shape.db.empty()) {
                    const ReleaseModel fall = fit_release({shape});
                    z.release = fall.fast;
                    z.release_slow = fall.two() ? fall.slow : 0;
                    z.release_share = fall.two() ? fall.share : 0;
                }
            }
            char held[64];
            std::snprintf(held, sizeof(held), " hold=%.1f/%.1f", probe_db(alone, at(0.25), at(0.225)), probe_db(alone, at(0.925), at(0.225)));
            r.heard += held;
            if (g.mono) {
                z.kind = AutoKind::Mono;
                z.glide = g.seconds;
                // Played legato, whether the second key starts the sound again or the first's note carries on: against
                // the second key alone.
                auto fresh = play({{0, true, b, 100}, {at(0.5), false, b, 100}}, 0.6);
                if (stopped) return z;
                const int again = probe_retriggers(both, alone, fresh, at(0.4), rate);
                z.tied = again != 1;
                r.heard += again < 0 ? " again=?" : again ? " again=1" : " again=0";
                if (g.seconds > 0) { // gliding also from a key let go: not legato
                    auto apart = play({{0, true, a, 100}, {at(0.3), false, a, 100}, {at(0.35), true, b, 100}, {at(0.8), false, b, 100}}, 0.9);
                    if (stopped) return z;
                    z.legato = !probe_slides_apart(apart, at(0.35), from_hz, to_hz, rate);
                }
            }
            if (alone_out) *alone_out = std::move(alone);
            return z;
        };
        std::vector<float> alone;
        const float base_hz = hz[0] > 0 ? hz[0] : hz_of(base);
        AutoZone lower = check_zone(base, base_hz, &alone), upper;
        if (stopped) return stop();

        // A split: two sounds side by side on the keyboard. The range's ends against the key just heard: an end that
        // is mono where that is poly (or the other way), or that sounds unlike it (probe_character()), is another
        // sound. Where it starts is found by halves, each key compared with the nearest key known on either side, and
        // holds only if the two keys it ends between are themselves unlike (one sound changing slowly along the
        // keyboard has no such step). One split is looked for.
        const ProbeCharacter here = probe_character(alone, base_hz, rate);
        const bool here_mono = lower.kind == AutoKind::Mono;
        struct End { int key = -1; ProbeCharacter sound; bool mono = false, other = false; float apart = 0; };
        auto end_at = [&](int key) {
            End e;
            e.key = key;
            auto two = fifth(uint8_t(key));
            auto one = hit(uint8_t(key), 100, 1.2, 1.25);
            if (stopped) return e;
            e.sound = probe_character(one, hz_of(key), rate);
            e.mono = probe_glide(two, one, at(0.4), hz_of(key), hz_of(key + 7), rate).mono;
            e.apart = probe_apart(e.sound, here);
            e.other = e.sound.level > -80.0f && (e.mono != here_mono || e.apart > 8.0f);
            return e;
        };
        End bottom, top;
        const int first = std::clamp(low, 24, 108), last = std::clamp(high, 24, 108);
        if (int(base) - first >= 8) bottom = end_at(first);
        if (stopped) return stop();
        if (last - 7 - int(base) >= 8) top = end_at(last - 7);
        if (stopped) return stop();
        const End* far = top.other && (!bottom.other || top.apart >= bottom.apart) ? &top : bottom.other ? &bottom : nullptr;
        uint8_t zone_key = 0; // where the other side was heard in full
        if (far) {
            const bool above = far == &top; // the other sound is above the key just heard
            int lo = above ? int(base) : far->key, hi = above ? far->key : int(base);
            ProbeCharacter under = above ? here : far->sound, over = above ? far->sound : here;
            const bool under_mono = above ? here_mono : far->mono, over_mono = above ? far->mono : here_mono;
            bool found_split = false;
            if (probe_apart(under, over) >= 6.0f) { // by their sound
                while (hi - lo > 1 && !stopped) {
                    const int mid = (lo + hi) / 2;
                    const ProbeCharacter c = probe_character(hit(uint8_t(mid), 100, 1.2, 1.25), hz_of(mid), rate);
                    if (probe_apart(c, under) <= probe_apart(c, over)) { lo = mid; under = c; } else { hi = mid; over = c; }
                }
                found_split = probe_apart(under, over) >= 6.0f;
            } else if (under_mono != over_mono) { // alike but for mono: a fifth cuts its lower key on the mono side only
                auto cuts = [&](int key) { // the fifth above `key` cuts it
                    auto two = fifth(uint8_t(key));
                    auto one = hit(uint8_t(key), 100, 1.2, 1.25);
                    return !stopped && probe_glide(two, one, at(0.4), hz_of(key), hz_of(key + 7), rate).mono;
                };
                if (under_mono) lo += 7; // the fifth above the lower key heard is still on its side
                while (hi - lo > 1 && !stopped) {
                    const int mid = (lo + hi) / 2;
                    // Mono below: `mid` is below when the fifth under it is cut by it. Mono above: above when its fifth cuts it.
                    const bool below = under_mono ? cuts(mid - 7) : !cuts(mid);
                    if (below) lo = mid; else hi = mid;
                }
                found_split = hi > lo;
            }
            if (stopped) return stop();
            if (found_split) {
                found.split = uint8_t(hi);
                // The other side in full, at a key of its own whose fifth is on its side too where there is room.
                if (above) zone_key = uint8_t(std::clamp(std::min((hi + last) / 2, 84), hi, std::max(hi, last - 7)));
                else zone_key = uint8_t(std::clamp(std::max((first + hi) / 2 - 4, 36), first, std::max(first, hi - 8)));
                r.heard += " | split=" + std::to_string(hi) + " other@" + std::to_string(zone_key);
                AutoZone other = check_zone(zone_key, hz_of(zone_key), nullptr);
                if (stopped) return stop();
                // How far its level follows velocity, from five velocities of that key.
                std::vector<uint8_t> its;
                std::vector<float> levels;
                for (int v : {16, 48, 80, 112, 127}) {
                    auto x = hit(zone_key, uint8_t(v), 0.25, 0.3);
                    if (stopped) return stop();
                    its.push_back(uint8_t(v));
                    levels.push_back(probe_db(x, at(0.01), at(0.15)));
                }
                other.velocity = probe_velocity(its, levels);
                if (above) upper = other; else { upper = lower; lower = other; }
            }
        }
        found.kind = lower.kind; found.glide = lower.glide; found.legato = lower.legato; found.tied = lower.tied;
        found.sustains = lower.sustains; found.moves = lower.moves; found.hold = lower.hold;
        found.release = lower.release; found.release_slow = lower.release_slow; found.release_share = lower.release_share;
        found.velocity = lower.velocity;
        found.upper = upper;
    }
    show(2);
    // Accent: one key at sixteen velocities, each one's level and brightness over its first 150 ms.
    std::vector<uint8_t> velocities;
    std::vector<float> db, bright;
    for (int v = 8; v <= 127; v = v == 120 ? 127 : v + 8) {
        auto x = hit(loud_key, uint8_t(v), 0.25, 0.3);
        if (stopped) return stop();
        velocities.push_back(uint8_t(v));
        db.push_back(probe_db(x, at(0.01), at(0.15)));
        bright.push_back(probe_brightness(x, at(0.01), at(0.15), rate));
    }
    found.accent = probe_accent(velocities, db, bright);
    // How far the level follows velocity, for the side of a split this key is on.
    if (found.kind != AutoKind::Drums) {
        std::vector<uint8_t> plain_v; // under an accent: the accent's jump is the top layer's, not the level following velocity
        std::vector<float> plain_db;
        for (size_t i = 0; i < velocities.size(); ++i)
            if (!found.accent || velocities[i] < found.accent) { plain_v.push_back(velocities[i]); plain_db.push_back(db[i]); }
        (found.split && loud_key >= found.split ? found.upper.velocity : found.velocity) = probe_velocity(plain_v, plain_db);
    }
    // A sound whose level does not follow velocity: these were sixteen hits of the same note, and tell as the five
    // below do whether it differs from hit to hit.
    bool sweep_varies = false;
    if (found.kind != AutoKind::Drums && !found.accent && (found.split && loud_key >= found.split ? found.upper.velocity : found.velocity) == 0 && !db.empty())
        sweep_varies = *std::max_element(db.begin(), db.end()) - *std::min_element(db.begin(), db.end()) > 1.5f;
    if (found.accent) { // narrowed down between the two velocities heard either side of it
        const size_t i = size_t(std::find(velocities.begin(), velocities.end(), found.accent) - velocities.begin());
        int below = velocities[i - 1], above = found.accent;
        const float low_db = db[i - 1], high_db = db[i], low_b = bright[i - 1], high_b = bright[i];
        while (above - below > 1) {
            const int mid = (below + above) / 2;
            auto x = hit(loud_key, uint8_t(mid), 0.25, 0.3);
            if (stopped) return stop();
            const float d = probe_db(x, at(0.01), at(0.15)), b = probe_brightness(x, at(0.01), at(0.15), rate);
            auto apart = [](float d1, float b1, float d2, float b2) {
                return std::fabs(d1 - d2) + (b1 > 0 && b2 > 0 ? 6.0f * std::fabs(std::log2(b1 / b2)) : 0.0f);
            };
            if (apart(d, b, high_db, high_b) < apart(d, b, low_db, low_b)) above = mid; else below = mid;
        }
        found.accent = uint8_t(above);
    }
    for (size_t i = 0; i < velocities.size(); ++i) {
        char one[48];
        std::snprintf(one, sizeof(one), "%s%d:%.1f/%.0f", i ? "," : " levels=", velocities[i], db[i], bright[i]);
        r.heard += one;
    }
    show(3);
    // Takes: five hits of the key, when one velocity layer is all there is to take.
    if (!found.accent) {
        std::vector<std::vector<float>> hits;
        for (int n = 0; n < 5; ++n) {
            hits.push_back(hit(loud_key, 80, 0.3, 0.35)); // under an accent, where one is (it may carry from hit to hit)
            if (stopped) return stop();
        }
        if (probe_varies(hits, rate) || sweep_varies) found.takes = 3;
        for (size_t h = 0; h < hits.size(); ++h) {
            char one[48];
            std::snprintf(one, sizeof(one), "%s%.1f/%.0f", h ? "," : " hits=", probe_db(hits[h], at(0.01), at(0.15)), probe_brightness(hits[h], 0, at(0.1), rate));
            r.heard += one;
        }
    }
    if (auto why = source->trouble(); !why.empty()) { r.error = why; r.key = source->trouble_key(); return r; } // an input that clips: nothing good can be sampled
    found.speed = playing > 0 ? float(played / playing) : 1.0f;
    // Velocity layers from the sixteen hits: where the tone changes (an accent starts a layer), for 1 to 8 layers,
    // and at Auto as many as the tone needs. One where the hits differ from hit to hit: takes then (an MPC plays a
    // keygroup's layers in turn or by velocity, not both). Drums: the accent's two, else one, as their Velocity is
    // not heard (engine/auto_probe.h probe_layers()).
    {
        const bool drum_kit = found.kind == AutoKind::Drums;
        const float amount = drum_kit ? 0.0f : found.split && loud_key >= found.split ? found.upper.velocity : found.velocity;
        found.sweep_count = uint8_t(std::min(velocities.size(), found.sweep_velocity.size()));
        for (size_t i = 0; i < found.sweep_count; ++i) { found.sweep_velocity[i] = velocities[i]; found.sweep_bright[i] = bright[i]; }
        for (unsigned k = 1; k <= found.layer_tops.size(); ++k) {
            const auto tops = probe_layers(velocities, db, bright, amount, found.accent, k);
            for (size_t l = 0; l < tops.size() && l < found.layer_tops[k - 1].size(); ++l) found.layer_tops[k - 1][l] = tops[l];
        }
        const size_t heard_layers = probe_layers(velocities, db, bright, amount, found.accent, 0).size();
        found.layers = uint8_t(found.takes > 1 ? 1 : drum_kit ? (found.accent ? 2 : 1) : std::clamp<size_t>(heard_layers, 1, 8));
    }
    {
        char text[200];
        std::snprintf(text, sizeof(text), " | velocity=%.2f speed=%.1f layers=%u", double(found.velocity), double(found.speed), unsigned(found.layers));
        r.heard += text;
        for (unsigned l = 0; l < found.layers; ++l) {
            std::snprintf(text, sizeof(text), "%s%u", l ? "," : " tops=", unsigned(found.layer_tops[found.layers - 1][l]));
            r.heard += text;
        }
        if (found.split) {
            const auto& u = found.upper;
            std::snprintf(text, sizeof(text), " from %s: kind=%s glide_ms=%d legato=%d tied=%d sustains=%d moves=%d hold_s=%.1f release_ms=%d velocity=%.2f",
                          key_word(found.split).c_str(), u.kind == AutoKind::Mono ? "mono" : "keys", int(std::lround(u.glide * 1000)), u.legato, u.tied,
                          u.sustains, u.moves, double(u.hold), int(std::lround(u.release * 1000)), double(u.velocity));
            r.heard += text;
        }
    }
    found.heard = true;
    show(4);
    if (keep) *keep = std::move(source);
    return r;
}

AutoSampleResult PrintEngine::sample_track(const PrintJob& job, const std::array<const SampleData*, sample_slots>& samples,
                                           std::unique_ptr<NoteSource> heard_by) {
    AutoSampleResult r;
    r.project_path = job.project_path;
    r.track = job.track % track_count;
    r.name = job.sample_name;
    r.post_fx = job.sampling.post_fx;
    auto t0 = Clock::now();
    const unsigned t = r.track;
    const auto& set = job.sampling;
    const auto roots = set.roots();
    const unsigned layers = set.layer_count(), takes = set.take_count();
    const bool drums = set.found.kind == AutoKind::Drums;
    // Keygroups/<name>/: the program and its WAV files in "<name>_[TrackData]", where MPC 3 looks for the samples of
    // "<name>.xty", its track file written beside that folder (an .xpm finds its samples beside itself, so one copy
    // of the files serves both).
    const fs::path folder(job.sample_folder), parts = folder / ".parts", files = folder / (job.sample_name + "_[TrackData]");
    std::error_code e;
    auto fail = [&](std::string why) { // nothing half made is left on the card
        fs::remove_all(folder, e);
        fs::remove(folder.parent_path(), e); // Keygroups/ too, when it is left empty
        r.error = std::move(why);
        r.program.clear();
        r.seconds = since(t0);
        return r;
    };
    if (roots.empty() || roots.size() > 128) { r.error = "no keys to sample"; return r; }
    {
        std::lock_guard<std::mutex> held(lock);
        shown.count = uint16_t(roots.size() * layers * takes);
        shown.autosample = true;
        doing = "track=" + std::to_string(t + 1) + " autosample";
    }
    const float rate = dsp::sample_rate();
    const unsigned whole_rate = unsigned(std::lround(rate));
    { // the card: room for every note at its longest, as 24-bit WAVs and the 32-bit float parts they are made from
        const double most = double(roots.size()) * layers * takes * (std::max(0.05f, set.hold_most()) + set.tail + 0.01) * rate;
        const uint64_t need = uint64_t(most * (6 + 8)) + (8u << 20);
        fs::path probe = folder.parent_path();
        while (!probe.empty() && !fs::exists(probe, e)) probe = probe.parent_path();
        auto room = fs::space(probe.empty() ? fs::path("/") : probe, e);
        if (!e && room.available < need)
            return fail("card full: " + std::to_string(need >> 20) + " MB needed, " + std::to_string(room.available >> 20) + " MB free");
    }
    // An instrument outside goes on with the check's notes: their silence was heard before the check played anything.
    // Opened again here, it would be heard while the check's last notes still ring (a delay's repeats, a slow fade)
    // and called not quiet.
    std::string why_not;
    auto source = heard_by ? std::move(heard_by) : open_notes(*this, job, samples, roots[roots.size() / 2], cancelled, why_not);
    if (!source) return fail(why_not);
    fs::create_directories(parts, e);
    if (!e) fs::create_directories(files, e);
    if (e) return fail("cannot make " + folder.filename().string());

    const float silent = source->silence();
    // Between notes: silence until what is left of the last (a delay's repeats too, at Post FX) has died away, 4 s at most.
    auto settle = [&] { source->settle(4.0f, 0.1f); };
    settle();

    XpmProgram program;
    program.name = job.sample_name;
    program.volume_db = 20.0 * std::log10(std::sqrt(2.0)); // the pan law's 3 dB back: as loud as the plug-in (xpm_write.h)
    const uint8_t split = drums ? 0 : set.found.split; // the first key of a split's upper side, 0 none
    { // As the check heard it: one note at a time, sliding as the plug-in does; with a split, on its mono side's keys.
        const AutoZone under = set.found.zone(0), over = set.found.zone(split);
        const bool under_mono = !drums && under.kind == AutoKind::Mono, over_mono = split && over.kind == AutoKind::Mono;
        if (under_mono || over_mono) {
            const AutoZone& z = under_mono ? under : over;
            program.mono = true;
            program.legato = z.legato;
            program.tied = z.tied;
            program.glide = z.glide;
            if (split && !over_mono) program.mono_high = uint8_t(split - 1);
            if (split && !under_mono) program.mono_low = split;
        }
    }
    std::vector<ReleaseShape> falls; // each note's fall once its key was let go (engine/release_fit.h)
    std::vector<uint8_t> fall_keys;  // and the key it was
    float peak_all = 0;
    uint64_t rendered = 0;
    double rendering = 0;
    for (size_t k = 0; k < roots.size(); ++k) {
        XpmKeygroup group;
        const AutoZone zone = set.found.zone(roots[k]); // its side of a split
        // Held as long as its side needs, its level following velocity as its side's did.
        const uint64_t hold = uint64_t(std::max(0.05f, set.hold_seconds(roots[k])) * rate);
        if (set.found.heard && !drums) group.velocity = zone.velocity;
        group.cycle = takes > 1;
        group.one_shot = drums; // a drum plays out whole however short the tap
        for (unsigned v = 0; v < layers * takes; ++v) {
            const unsigned take = v % takes, layer_index = v / takes;
            const uint8_t velocity = set.velocity(layer_index);
            // The note's level for the screen: the hold on 60% of the bars at first, the release after it.
            constexpr unsigned bars = PrintProgress::wave_bars;
            std::vector<float> wave;
            wave.reserve(bars);
            uint64_t per = std::max<uint64_t>(1, (hold + bars * 6 / 10 - 1) / (bars * 6 / 10)), into = 0;
            float loudest = 0;
            auto show_wave = [&] {
                std::lock_guard<std::mutex> held(lock);
                for (size_t b = 0; b < wave.size(); ++b) {
                    const float db = wave[b] > 0 ? 20.0f * std::log10(wave[b]) : -36.0f;
                    shown.wave[b] = uint8_t(std::lround(std::clamp((db + 36.0f) / 36.0f, 0.0f, 1.0f) * 255));
                }
                shown.wave_count = uint8_t(wave.size());
                shown.wave_hold = uint8_t(std::min<uint64_t>(255, hold / per));
            };
            {
                std::lock_guard<std::mutex> held(lock);
                shown.pattern = int16_t(roots[k]);
                shown.fraction = 0;
                shown.velocity = velocity;
                shown.wave_count = 0;
            }
            auto r0 = Clock::now();
            uint64_t heard = 0, shown_at = 0;
            NoteAudio audio = source->note(roots[k], velocity, hold, set.tail, [&](const float* l, const float* rr, unsigned n) {
                for (unsigned f = 0; f < n; ++f) { // each stretch as it is played, into its bars
                    loudest = std::max({loudest, std::fabs(l[f]), std::fabs(rr[f])});
                    if (++into < per) continue;
                    wave.push_back(loudest); into = 0; loudest = 0;
                    if (wave.size() >= bars) { // a long release: each bar twice the time
                        for (size_t b = 0; b < bars / 2; ++b) wave[b] = std::max(wave[2 * b], wave[2 * b + 1]);
                        wave.resize(bars / 2);
                        per *= 2;
                    }
                }
                heard += n;
                if (heard - shown_at >= uint64_t(0.02f * rate)) { // the screen's, every 20 ms of it
                    shown_at = heard;
                    show_wave();
                    std::lock_guard<std::mutex> held(lock);
                    shown.fraction = std::min(0.95f, float(double(heard) / (double(hold) + 0.5 * rate)));
                }
                return !cancelled.load();
            });
            show_wave();
            rendering += since(r0);
            rendered += audio.left.size();
            if (!audio.error.empty()) return fail(audio.error == "stopped" ? "stopped" : audio.error);
            if (drums) trim_silence(audio, rate, silent); // a one shot: no silence kept for the time the key was held
            if (remove_dc(audio.left, audio.right, audio.held, rate)) ++r.centred;
            settle();
            if (auto why = source->trouble(); !why.empty()) { r.key = source->trouble_key(); return fail(why); }
            float peak = 0;
            for (size_t f = 0; f < audio.left.size(); ++f) peak = std::max({peak, std::fabs(audio.left[f]), std::fabs(audio.right[f])});
            {
                std::lock_guard<std::mutex> held(lock);
                ++shown.through;
                const double wall = since(t0);
                shown.times_real = rendering > 0 ? float(double(rendered) / rate / rendering) : 0;
                shown.seconds_left = float(wall / shown.through * (shown.count - shown.through));
                if (v + 1 == layers * takes) shown.done[roots[k] / 64 % shown.done.size()] |= uint64_t(1) << (roots[k] % 64);
            }
            if (peak < silent) continue; // silent here (a plug-in with a narrower range): the next keygroup's reaches
            // How it falls once its key is let go, against its level then: the program's release is fitted to these.
            if (!drums) {
                falls.push_back(release_shape(audio.left, audio.right, audio.held, rate,
                                              audio.release + uint64_t(0.02f * rate) >= uint64_t(std::max(0.0f, set.tail) * rate),
                                              job.external ? 20.0f * std::log10(silent) : -100.0f)); // an input's noise
                fall_keys.push_back(roots[k]);
            }
            XpmLayer layer;
            layer.sample = job.sample_name + "-" + key_word(roots[k]) + "-V" + std::to_string(velocity)
                         + (takes > 1 ? "-T" + std::to_string(take + 1) : std::string());
            layer.root = roots[k];
            layer.low_velocity = uint8_t(takes > 1 || layer_index == 0 ? 0 : set.velocity(layer_index - 1) + 1);
            layer.high_velocity = takes > 1 ? 127 : velocity;
            // A velocity layer is recorded at its loudest velocity: the keygroup's Velocity would take it down again
            // there, so it is raised by as much, and a softer press within the layer falls from that as the plug-in's.
            if (takes == 1 && group.velocity > 0)
                layer.gain = std::min(16.0, 1.0 / std::max(1e-3, 1.0 - group.velocity * (1.0 - velocity / 127.0)));
            if (set.looping(roots[k])) {
                LoopPoints loop = find_loop(audio.left, audio.right, audio.held, rate, true); // it sustains: a loop however unlike its ends
                if (loop.found()) {
                    layer.loop_start = loop.start; layer.loop_end = loop.end; ++r.loops;
                    // A looped note never plays past its loop (the keygroup's release fades the loop itself): what
                    // was recorded after it is left out of the file.
                    const size_t keep = std::min(audio.left.size(), size_t(loop.end) + 256);
                    audio.left.resize(keep); audio.right.resize(keep);
                }
            }
            // The program's gain is set by the loudest sample as written (after a loop's levelling).
            for (size_t f = 0; f < audio.left.size(); ++f) peak_all = std::max({peak_all, std::fabs(audio.left[f]), std::fabs(audio.right[f])});
            layer.frames = uint32_t(audio.left.size());
            // Kept as 32-bit float until the program's gain is known.
            std::ofstream part(parts / (layer.sample + ".f32"), std::ios::binary | std::ios::trunc);
            std::vector<float> interleaved(audio.left.size() * 2);
            for (size_t f = 0; f < audio.left.size(); ++f) { interleaved[2 * f] = audio.left[f]; interleaved[2 * f + 1] = audio.right[f]; }
            part.write(reinterpret_cast<const char*>(interleaved.data()), std::streamsize(interleaved.size() * sizeof(float)));
            if (!part) return fail("cannot write " + layer.sample);
            group.layers.push_back(layer);
            ++r.samples;
        }
        if (!group.layers.empty()) program.keygroups.push_back(std::move(group));
    }
    if (program.keygroups.empty()) return fail("the plug-in made no sound");
    // The keygroups that are left cover every key: each reaches halfway to the next (drums: its own key only, a sound
    // not stretched over its neighbours). A silent layer leaves its velocities to the layer above (the loudest reaches
    // 127); takes each reach every velocity.
    for (size_t k = 0; k < program.keygroups.size(); ++k) {
        auto& g = program.keygroups[k];
        const int root = g.layers.front().root;
        if (drums) g.low = g.high = uint8_t(root);
        else {
            g.low = uint8_t(k == 0 ? 0 : (program.keygroups[k - 1].layers.front().root + root) / 2 + 1);
            g.high = uint8_t(k + 1 == program.keygroups.size() ? 127 : (root + program.keygroups[k + 1].layers.front().root) / 2);
            // A split's sides keep to their own keys: the last below it reaches up to it, the first above down to it.
            if (split && root < split) g.high = uint8_t(std::min<int>(g.high, split - 1));
            if (split && root >= split) g.low = uint8_t(std::max<int>(g.low, split));
            if (split && root < split && (k + 1 == program.keygroups.size() || program.keygroups[k + 1].layers.front().root >= split)) g.high = uint8_t(split - 1);
            if (split && root >= split && (k == 0 || program.keygroups[k - 1].layers.front().root < split)) g.low = split;
        }
        if (g.cycle) continue;
        for (size_t l = 0; l < g.layers.size(); ++l) g.layers[l].low_velocity = uint8_t(l == 0 ? 0 : g.layers[l - 1].high_velocity + 1);
        g.layers.back().high_velocity = 127;
    }
    // Let go, a key falls as the plug-in's did (engine/release_fit.h): the keygroups' release, fitted to how the notes
    // fell against their level as their keys were let go. Two stages (a fast drop, then echoes or a reverb ringing
    // on): each keygroup twice over the same keys and layers, one with the fast release and most of the level, one
    // with the slow release and the rest, while there is room for both (128 keygroups). Notes that faded while held
    // and fall no faster let go (a pluck that rings on whatever the key does) play out whole, as one shots.
    const size_t played = program.keygroups.size();
    for (unsigned side = 0; !drums && side < (split ? 2u : 1u); ++side) { // a split: each side falls its own way
        const AutoZone zone = set.found.zone(side ? split : 0);
        auto on_side = [&](unsigned key) { return !split || (key >= split) == (side == 1); };
        std::vector<ReleaseShape> its;
        for (size_t n = 0; n < falls.size(); ++n) if (on_side(fall_keys[n])) its.push_back(falls[n]);
        size_t keygroups = 0;
        for (size_t k = 0; k < played; ++k) keygroups += on_side(program.keygroups[k].layers.front().root);
        if (!keygroups) continue;
        std::string note;
        ReleaseModel fall = fit_release(its, &note);
        size_t told = 0, fading = 0;
        for (const auto& f : its) { told += !f.db.empty(); fading += f.fading; }
        if (!told && zone.release > 0) { // held until they had died away: as the check heard it let go early
            fall.fast = zone.release; fall.slow = zone.release_slow; fall.share = zone.release_share;
        }
        const bool looping = set.looping(side ? split : 0);
        const bool whole = !looping && fading * 2 > its.size() && fall.db(1.0f) > -3.0f; // played out whole, as one shots
        const bool twice = !whole && fall.two() && program.keygroups.size() + keygroups <= 128;
        if (!whole && fall.two() && !twice) { // no room for two: one stage, the slow one's time when it holds most of the level
            fall.fast = fall.share >= 0.5f ? fall.slow : fall.fast;
            fall.slow = fall.share = 0;
        }
        for (size_t k = 0; k < played; ++k) {
            if (!on_side(program.keygroups[k].layers.front().root)) continue;
            if (whole) { program.keygroups[k].one_shot = true; continue; }
            if (side) program.keygroups[k].release = fall.fast; // the lower side's (or the only one's) is the program's
            if (!twice) continue;
            XpmKeygroup ringing = program.keygroups[k];
            ringing.gain = fall.share;
            ringing.release = fall.slow;
            program.keygroups[k].gain = 1.0 - fall.share;
            program.keygroups.push_back(std::move(ringing));
        }
        if (!side) {
            r.release_note = note;
            if (job.external) r.release_note += " " + source->about(); // the input's silence and the instrument's delay
            program.release = fall.fast;
            r.release = fall.fast; r.release_slow = fall.two() ? fall.slow : 0; r.release_share = fall.two() ? fall.share : 0;
        } else {
            char text[96];
            std::snprintf(text, sizeof(text), " | from %s: release_ms=%d", key_word(split).c_str(), int(std::lround(fall.fast * 1000)));
            r.release_note += text;
            if (fall.two()) {
                std::snprintf(text, sizeof(text), "+%d@%.0fdB", int(std::lround(fall.slow * 1000)), 20.0 * std::log10(double(fall.share)));
                r.release_note += text;
            }
            r.release_note += " " + note;
        }
    }
    // One gain for every sample: the loudest at -1 dB (24 dB more at most), so the layers keep the balance played.
    const float gain = set.normalize && peak_all > 0 ? std::min(0.891251f / peak_all, 15.8489f) : 1.0f;
    r.gain_db = 20 * std::log10(gain);
    for (size_t k = 0; k < played; ++k) // the second stage's keygroups play the same files
        for (const auto& l : program.keygroups[k].layers) {
            if (cancelled.load()) return fail("stopped");
            std::ifstream part(parts / (l.sample + ".f32"), std::ios::binary);
            std::vector<float> interleaved(size_t(l.frames) * 2);
            part.read(reinterpret_cast<char*>(interleaved.data()), std::streamsize(interleaved.size() * sizeof(float)));
            if (!part) return fail("cannot read " + l.sample);
            for (auto& x : interleaved) x *= gain;
            // Both sides the same (a mono plug-in or input): one channel, half the card and memory, the same sound.
            const unsigned channels = sides_alike(interleaved.data(), l.frames) ? 1 : 2;
            if (channels == 1) {
                for (size_t f = 0; f < l.frames; ++f) interleaved[f] = interleaved[2 * f];
                ++r.mono;
            }
            const fs::path path = files / (l.sample + ".wav");
            WavStream wav;
            bool written = wav.open(path.string(), whole_rate, 24, 0, channels);
            for (size_t at = 0; written && at < l.frames; at += EngineHost::block) {
                unsigned n = unsigned(std::min<size_t>(EngineHost::block, l.frames - at));
                written = wav.add(interleaved.data() + channels * at, n);
            }
            written = wav.close() && written;
            if (!written) return fail("cannot write " + path.filename().string());
        }
    fs::remove_all(parts, e);
    const fs::path xpm = files / (job.sample_name + ".xpm"), xty = folder / (job.sample_name + ".xty");
    size_t most = 0;
    for (const auto& k : program.keygroups) most = std::max(most, k.layers.size());
    // MPC 3's track file: it has what an .xpm has no tag for (Mono Retrig) and up to eight layers a keygroup, made
    // from a copy of the program with all of them (removed after). The .xpm keeps four, as MPC 2 and MPC Beats read
    // it (xpm_layers()). The program stands without the track file, so a failure is only told.
    std::string xty_failed;
    if (most > 4) {
        const fs::path whole = files / (job.sample_name + ".all.xpm");
        xty_failed = write_xpm(whole.string(), program, 8);
        if (xty_failed.empty()) xty_failed = write_mpc3_track(whole.string(), xty.string());
        fs::remove(whole, e);
    }
    if (auto why = write_xpm(xpm.string(), program); !why.empty()) return fail(why);
    if (most <= 4) xty_failed = write_mpc3_track(xpm.string(), xty.string());
    if (!xty_failed.empty()) r.release_note += " xty=" + xty_failed;
    // The keygroup track plays every layer: from the track file where the .xpm holds fewer.
    r.program = most > 4 && xty_failed.empty() ? xty.string() : xpm.string();
    r.layers = unsigned(most);
    r.seconds = since(t0);
    return r;
}
} // namespace melodee
