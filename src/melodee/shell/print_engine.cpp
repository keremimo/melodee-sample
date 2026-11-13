// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/print_engine.h"
#include "melodee/engine/effects.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/print_player.h"
#include "melodee/engine/print_render.h"
#include "melodee/engine/stream_pager.h"
#include "melodee/engine/sample_data.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/sample_library.h"
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

// A print file's frames: where they start and how many bytes, if it is stereo 32-bit float at `rate` with them on a
// page's boundary (as print_one() writes it); false otherwise (an older print: printed again).
bool print_layout(const std::string& path, unsigned rate, uint64_t& offset, uint64_t& bytes) {
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
            format = (f[0] | f[1] << 8) == 3 && (f[2] | f[3] << 8) == 2 && u32(f + 4) == rate && (f[14] | f[15] << 8) == 32;
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
        if (rec->rate != rate || !print_layout(path, rate, offset, bytes)) { f.error = "print " + rec->file + " unreadable"; return f; }
        int fd = ::open(path.c_str(), O_RDONLY);
        struct stat st {};
        if (fd < 0 || fstat(fd, &st) != 0 || uint64_t(st.st_size) < offset + bytes) { if (fd >= 0) ::close(fd); f.error = "cannot open " + rec->file; return f; }
        void* map = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (map == MAP_FAILED) { f.error = "cannot map " + rec->file; return f; }
        auto clip = std::make_unique<PrintClip>();
        auto& part = clip->part;
        part.mapping = map; part.mapping_bytes = size_t(st.st_size); // unmapped with the clip
        clip->frames = uint32_t(std::min<uint64_t>(bytes / 8, rec->frames));
        clip->rate = rate;
        clip->tail = rec->tail;
        clip->pass = uint32_t(std::llround(double(rec->pass) * rate / dsp::model_rate));
        part.left = part.right = reinterpret_cast<const float*>(static_cast<const char*>(map) + offset);
        part.total = 2 * clip->frames;
        if (!part.total) { f.error = "empty print " + rec->file; return f; }
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
        bool held = hold(0, std::max<uint32_t>(2 * rate, 2 * stream_chunk));
        const uint32_t ticks = std::max<uint32_t>(1, played_pattern(job.bank->at[job.track][p], track.speed).length);
        for (uint32_t b = bar; held && b < ticks; b += bar) {
            uint64_t frame = uint64_t(clip->pass) * b / ticks;
            if (2 * frame >= 2 * uint64_t(rate)) held = hold(uint32_t(std::min<uint64_t>(2 * frame, part.total)), 2 * stream_chunk);
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
                    most = std::max({most, std::fabs(data[2 * size_t(f0)]), std::fabs(data[2 * size_t(f0) + 1])});
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
                                                       fs::exists(path, e) && print_layout(path.string(), r.record.rate, offset, bytes)) {
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
    std::set<long> threads_before, threads_idle; // freeze plan F2d: the print's plug-ins' threads at SCHED_IDLE
    bool plugins = false;
    for (const auto& ref : project->plugins) plugins |= ref.used();
    if (plugins) { // its own rack: the plug-ins opened again, staged apart from the live rack's
        start_plugin_messages(); // a thread of the live rack's, not one of the print's
        threads_before = task_ids();
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
        idle_new_threads(threads_before, threads_idle);
    }
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
    bool written = wav.open(part.string(), r.record.rate, 32, print_data_at); // its frames on a page, for a freeze's mapping
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
