// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The Looper's files (docs/LOOPER.md): Commit to pad, and a project's loops
// kept beside it as "<project> Loops/Loop N.wav" (24-bit stereo, 44.1 kHz).
// The MIDI thread asks; the service thread holds a loop (the audio thread
// lets go of it once nothing writes it), copies it, lets go and writes, never
// waiting in between: each service pass tries again.
#include "melodee/engine/engine_host.h"
#include "melodee/engine/take_recorder.h"
#include "melodee/shell/context.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/sample_library.h"
#include <algorithm>
#include <filesystem>

namespace melodee {
namespace fs = std::filesystem;

namespace {
constexpr unsigned hold_passes = 600; // service passes a loop may stay busy before a job gives up on it (a few seconds)

std::string loop_dir(const std::string& project_path) {
    if (project_path.empty()) return {};
    fs::path p(project_path);
    return (p.parent_path() / (p.stem().string() + " Loops")).string();
}
std::string loop_file(const std::string& dir, unsigned loop) { return dir + "/Loop " + std::to_string(loop + 1) + ".wav"; }
} // namespace

void commit_loop(Context& c, unsigned loop, bool note) {
    loop %= loop_tracks;
    auto& t = c.track();
    auto v = c.host.looper().view(loop);
    std::string name = "L" + std::to_string(loop + 1);
    if (t.engine != EngineKind::Sampler) { c.notice("Commit: pick a kit track"); return; }
    if (!v.length || (v.state != Looper::State::Playing && v.state != Looper::State::Stopped)) { c.notice(name + ": nothing to commit now"); return; }
    if (v.length > TakeRecorder::max_frames) { c.notice(name + ": too long for a pad"); return; }
    if (!c.library) return;
    int kit = c.next_empty_pad(t.last_pad);
    if (kit < 0) { c.notice("No empty pad"); return; }
    int slot = c.hold_file(name + " Loop", c.ui.track, kit, 1);
    if (slot < 0) { c.notice("Commit: samples full"); return; }
    if (!c.library->commit_loop({uint8_t(slot), uint8_t(loop)})) { c.drop_file(slot); c.notice("Commit: busy"); return; }
    if (note) { // one note at bar 1, as long as the loop (the pattern made long enough), and the loop stops: the pad plays it now
        unsigned ticks = unsigned(std::max(1.0, std::round(v.ticks)));
        unsigned bar = c.project.bar_ticks();
        c.edit_pattern(c.ui.track, t.pattern, "Commit + note", [&] {
            auto& p = c.pattern();
            uint32_t want = std::min<uint32_t>((ticks + bar - 1) / bar * bar, c.project.max_length());
            if (p.length < want) p.length = want;
            Event e;
            e.tick = 0; e.length = uint16_t(std::min<unsigned>(ticks, 65535)); e.key = uint8_t(sampler_first_note + unsigned(kit)); e.velocity = 100;
            p.add(e);
            c.sync_pattern(c.ui.track, t.pattern);
        });
        looper_do(c, looper::Do::Halt, loop);
    }
    c.notice(name + " to " + std::string(1, char('A' + kit / 16)) + (kit % 16 + 1 < 10 ? "0" : "") + std::to_string(kit % 16 + 1));
}

// The project's loops beside its file, as they are now; the loops with nothing lose their old file.
void save_loop_files(Context& c, const std::string& path) {
    auto& lp = c.project.looper;
    uint8_t mask = 0;
    for (unsigned n = 0; n < loop_tracks; ++n) {
        auto v = c.host.looper().view(n);
        auto& l = lp.loops[n];
        bool keep = lp.on && v.length && v.ticks > 0 && v.state != Looper::State::Empty && v.state != Looper::State::Armed && v.state != Looper::State::Recording;
        if (keep) { l.frames = v.length; l.ticks = v.ticks; l.tempo = v.tempo; l.layers = v.layers; l.stopped = v.state == Looper::State::Stopped; mask |= uint8_t(1u << n); }
        else l.frames = 0;
    }
    if (!c.library || path.empty()) return;
    std::error_code error;
    if (!mask && !fs::exists(loop_dir(path), error)) return; // nothing now, nothing before
    SampleLibrary::LoopFiles job;
    job.save = true; job.dir = loop_dir(path); job.mask = mask; job.loops = lp.loops;
    c.library->loop_files(std::move(job));
}

// A project just opened: its loops read back into the Looper.
void load_loop_files(Context& c, const std::string& path) {
    const auto& lp = c.project.looper;
    uint8_t mask = 0;
    for (unsigned n = 0; n < loop_tracks; ++n) if (lp.on && lp.loops[n].frames) mask |= uint8_t(1u << n);
    if (!mask || !c.library || path.empty()) return;
    SampleLibrary::LoopFiles job;
    job.save = false; job.dir = loop_dir(path); job.mask = mask; job.loops = lp.loops;
    c.library->loop_files(std::move(job));
}

// ---- SampleLibrary (service thread) ------------------------------------------

void SampleLibrary::loop_files(LoopFiles job) {
    std::lock_guard<std::mutex> guard(loop_lock);
    loop_jobs.push_back(std::move(job));
    loop_busy.store(true);
}

bool SampleLibrary::loops_busy() const { return loop_busy.load(); }

void SampleLibrary::loop_service() {
    auto& looper = host.looper();
    // Commit to pad: the loop's frames onto the slot held for it.
    if (!committing.on && commits.pop(committing.job)) { committing.on = true; committing.passes = 0; }
    if (committing.on) {
        unsigned loop = committing.job.loop % loop_tracks;
        if (looper.hold(loop)) {
            auto v = looper.view(loop);
            uint32_t frames = std::min<uint32_t>(v.length, looper::capacity);
            auto data = std::make_unique<SampleData>();
            data->left.assign(looper.held_left(loop), looper.held_left(loop) + frames);
            data->right.assign(looper.held_right(loop), looper.held_right(loop) + frames);
            data->frames = frames;
            looper.release(loop);
            committing.on = false;
            std::string stem = "L" + std::to_string(loop + 1) + " Loop";
            if (frames) land(committing.job.slot, std::move(data), unsigned(dsp::model_rate), "Looper", stem.c_str(), "Commit");
            else finish("Commit: nothing there", int(committing.job.slot));
        } else if (++committing.passes > hold_passes) {
            looper.release(loop);
            committing.on = false;
            finish("Commit: the loop is busy", int(committing.job.slot));
        }
    }
    // A project's loops, one loop at a time.
    if (!loop_job_on) {
        std::lock_guard<std::mutex> guard(loop_lock);
        if (loop_jobs.empty()) { loop_busy.store(committing.on); return; }
        loop_job = std::move(loop_jobs.front());
        loop_jobs.erase(loop_jobs.begin());
        loop_job_on = true; loop_at = 0; loop_passes = 0;
    }
    std::error_code error;
    while (loop_at < loop_tracks) {
        unsigned n = loop_at;
        bool wanted = (loop_job.mask >> n) & 1;
        std::string file = loop_file(loop_job.dir, n);
        if (!loop_job.save) { // read it back: into a buffer of the Looper's own, handed over whole
            if (wanted) {
                SampleData data;
                auto failed = read_wav(file, data, 70);
                if (failed.empty() && data.frames) {
                    auto* buffer = new LoopBuffer;
                    uint32_t frames = std::min<uint32_t>({data.frames, looper::capacity, loop_job.loops[n].frames ? loop_job.loops[n].frames : data.frames});
                    std::copy(data.left.begin(), data.left.begin() + frames, buffer->left.get());
                    std::copy(data.right.begin(), data.right.begin() + frames, buffer->right.get());
                    LoopLoad item;
                    item.buffer = buffer; item.loop = uint8_t(n); item.frames = frames;
                    item.ticks = loop_job.loops[n].ticks; item.tempo = loop_job.loops[n].tempo;
                    item.layers = std::max<uint8_t>(1, loop_job.loops[n].layers);
                    item.stopped = loop_job.loops[n].stopped;
                    if (!looper.give_load(item)) { delete buffer; return; } // full: again next pass
                } else {
                    finish("L" + std::to_string(n + 1) + ": file missing");
                }
            }
            ++loop_at;
            return; // one file a pass keeps the thread answering
        }
        if (!wanted) { fs::remove(file, error); ++loop_at; continue; }
        if (!looper.hold(n)) {
            if (++loop_passes > hold_passes) { looper.release(n); finish("L" + std::to_string(n + 1) + ": not saved, busy"); ++loop_at; loop_passes = 0; }
            return;
        }
        auto v = looper.view(n);
        SampleData data;
        uint32_t frames = std::min<uint32_t>(v.length, looper::capacity);
        data.left.assign(looper.held_left(n), looper.held_left(n) + frames);
        data.right.assign(looper.held_right(n), looper.held_right(n) + frames);
        data.frames = frames;
        looper.release(n);
        fs::create_directories(loop_job.dir, error);
        auto failed = write_wav(file + ".new", data);
        if (failed.empty()) fs::rename(file + ".new", file, error);
        if (!failed.empty() || error) finish("L" + std::to_string(n + 1) + ": not saved");
        ++loop_at; loop_passes = 0;
        return;
    }
    if (loop_job.save) { // no loops left: no folder either
        bool any = loop_job.mask != 0;
        if (!any) fs::remove(loop_job.dir, error);
    }
    loop_job_on = false;
}

} // namespace melodee
