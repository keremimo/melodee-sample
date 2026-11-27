// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Samples made from what Melodee hears (parity phase 6), on the service
// thread: Audio Recall's ring copied out, and renders of the mix drained
// from the engine host as they play (Resample into a slot, the song's
// mixdown into a WAV file). The audio thread only fills the rings; every
// allocation and file write is here.
#include "melodee/engine/engine_host.h"
#include "melodee/shell/sample_library.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace melodee {
namespace fs = std::filesystem;

std::string SampleLibrary::card() const { std::lock_guard<std::mutex> guard(lock); return card_top; }

// Shift + Sample Record: the last 25 s of the source, cut as the Settings
// page's Recall rows say (recall_bounds: the last phrase, from the first
// sound, or all of it; the quiet after the last sound left out or kept).
// Kept at the ring's rate, the device's.
void SampleLibrary::save_recall(const RecallJob& job) {
    int slot = int(job.slot);
    const auto& ring = host.recall();
    const unsigned rate = ring.rate();
    uint32_t frames = ring.filled();
    if (frames < rate / 100) { finish("Audio Recall: nothing yet", slot); return; }
    auto data = std::make_unique<SampleData>();
    data->left.resize(frames); data->right.resize(frames);
    if (!ring.copy_last(frames, data->left.data(), data->right.data())) { finish("Audio Recall: try again", slot); return; }
    uint32_t first = 0, end = frames;
    if (!recall_bounds(data->left.data(), data->right.data(), frames, rate, job.options, first, end)) {
        finish("Audio Recall: only silence", slot);
        return;
    }
    data->left.erase(data->left.begin() + end, data->left.end());
    data->right.erase(data->right.begin() + end, data->right.end());
    data->left.erase(data->left.begin(), data->left.begin() + first);
    data->right.erase(data->right.begin(), data->right.begin() + first);
    data->frames = end - first;
    land(unsigned(slot), std::move(data), rate, "Recorded", "RECALL", "Audio Recall");
}

// A render: its job first (the MIDI thread queues it before it presses
// Play), then every frame the engine host hands over until it is done,
// stopped by hand, or lost because this thread fell 12 s behind.
void SampleLibrary::drain_render() {
    auto& bounce = host.bounce();
    if (stems_closing) { // the stems' last chunks still going to the card
        if (stem_writer && !stem_writer->done()) return;
        close_stems();
        return;
    }
    if (!rendering) {
        auto state = bounce.state();
        if (renders.pop(render_job)) {
            rendering = true;
            render_rate = bounce.rate();
            render_data.reset();
            render_path.clear();
            stems_done_.store(0, std::memory_order_release);
            if (render_job.to_file && render_job.stems) { // roadmap P4: a folder of its own, the mix beside the stems
                std::string top = card();
                std::error_code error;
                std::string folder;
                if (render_job.more) folder = stems_folder; // a later pass: the pad stems the first could not take
                else if (!top.empty()) {
                    std::string stems = top + "/Melodee/Stems";
                    fs::create_directories(stems, error);
                    folder = free_name(stems, render_job.name, "");
                    fs::create_directories(folder, error);
                }
                render_path = folder.empty() ? std::string() : render_job.more ? folder : folder + "/Mix.wav";
                if (render_path.empty() || !stems_ring || !open_stems(folder)) {
                    render_path.clear();
                    stems_done_.store(-1, std::memory_order_release);
                    finish(top.empty() ? "Mixdown: no card" : "Mixdown: cannot write");
                }
            } else if (render_job.to_file) {
                std::string top = card();
                std::error_code error;
                std::string folder = top + "/Melodee/Mixdowns";
                if (!top.empty()) fs::create_directories(folder, error);
                render_path = top.empty() ? std::string() : free_name(folder, render_job.name);
                if (render_path.empty() || !render_file.open(render_path, render_rate, render_job.bits)) { // at the device's rate
                    render_path.clear();
                    finish(top.empty() ? "Mixdown: no card" : "Mixdown: cannot write");
                }
            } else {
                // Into a slot: with a card, straight into Samples/Recorded/RESAMPLE n.wav as it comes (32-bit float: a mix
                // can go past full scale), then loaded from there, however long; without one, in memory, 60 s at most.
                render_written = false;
                std::string root;
                { std::lock_guard<std::mutex> guard(lock); root = known.root; }
                if (!root.empty()) {
                    std::error_code error;
                    fs::create_directories(root + "/Recorded", error);
                    for (unsigned n = 1; n < 10000 && render_path.empty(); ++n) {
                        const std::string name = root + "/Recorded/RESAMPLE " + std::to_string(n) + ".wav";
                        if (!fs::exists(name, error)) render_path = name;
                    }
                    render_written = !render_path.empty() && render_file.open(render_path, render_rate, 32);
                    if (!render_written) render_path.clear();
                }
                if (!render_written) {
                    render_data = std::make_unique<SampleData>();
                    uint32_t room = std::min<uint32_t>(render_job.frames + 64, TakeRecorder::max_frames);
                    room = uint32_t(uint64_t(room) * render_rate / uint32_t(dsp::model_rate)) + 64; // its frames are the device's
                    render_data->left.reserve(room); render_data->right.reserve(room);
                }
            }
        } else if (state == Bounce::State::Done || state == Bounce::State::Cancelled || state == Bounce::State::Failed) {
            float drop[2 * 1024];
            while (bounce.pop(drop, 1024)) {} // nobody asked for it
            bounce.release();
            return;
        } else return;
    }
    render_chunk.resize(2 * 16384);
    // Everything there is now, a chunk at a time; into a slot, as long as a pad's sample may be (60 s).
    const uint32_t most = uint32_t(uint64_t(TakeRecorder::max_frames) * render_rate / uint32_t(dsp::model_rate));
    if (stems_open) drain_stems();
    for (unsigned n; (n = bounce.pop(render_chunk.data(), 16384)) > 0;) {
        if (stems_open) { if (!render_job.more) stem_writer->add(unsigned(stem_paths.size() - 1), render_chunk.data(), n); } // the mix
        else if (render_job.stems) {} // stems that could not open: nothing kept
        else if (render_job.to_file || render_written) { if (!render_path.empty()) render_file.add(render_chunk.data(), n); }
        else if (render_data && render_data->left.size() < most)
            for (unsigned f = 0; f < n && render_data->left.size() < most; ++f) {
                render_data->left.push_back(render_chunk[2 * f]); render_data->right.push_back(render_chunk[2 * f + 1]);
            }
    }
    // Into a slot and full (a song past 60 s, roadmap P3): it lands now; what still comes is dropped as it ends.
    if (!render_job.to_file && render_data && render_data->left.size() >= most) {
        rendering = false;
        render_data->frames = uint32_t(render_data->left.size());
        land(render_job.slot, std::move(render_data), render_rate, "Recorded", "RESAMPLE", "Resample");
        return;
    }
    auto state = bounce.state();
    bool ended = state == Bounce::State::Done || state == Bounce::State::Cancelled || state == Bounce::State::Failed;
    if (!ended || !bounce.drained()) return;
    rendering = false;
    bounce.release();
    if (stems_open) drain_stems();
    host.set_stems(nullptr);
    if (render_written) { // a render into a slot, in its file: from there into the slot
        render_written = false;
        const bool kept = render_file.close() && state == Bounce::State::Done && render_file.frames() > 0;
        if (!kept) {
            std::error_code error;
            fs::remove(render_path, error);
            finish(state == Bounce::State::Failed ? "Resample: card too slow" : state == Bounce::State::Cancelled ? "Resample stopped" : "Resample: write failed",
                   int(render_job.slot));
            return;
        }
        scan_wanted.store(true);
        load_written(render_job.slot, render_path);
        return;
    }
    if (render_job.to_file) {
        if (render_path.empty()) return; // said already
        if (render_job.stems) { // roadmap P4: the last chunks to the card, then closed (close_stems())
            if (!stems_open) return;
            stems_cancelled = state == Bounce::State::Cancelled;
            stems_failed = state == Bounce::State::Failed; // the render's own ring fell behind
            stem_writer->flush();
            stems_closing = true;
            return;
        }
        bool kept = render_file.close() && state == Bounce::State::Done && render_file.frames() > 0;
        if (!kept) {
            std::error_code error;
            fs::remove(render_path, error);
            finish(state == Bounce::State::Failed ? "Mixdown: card too slow" : state == Bounce::State::Cancelled ? "Mixdown stopped" : "Mixdown: write failed");
            return;
        }
        finish("Mixdown " + fs::path(render_path).stem().string());
        return;
    }
    if (state != Bounce::State::Done || !render_data || render_data->left.empty()) {
        finish(state == Bounce::State::Failed ? "Resample: fell behind" : "Resample stopped", int(render_job.slot));
        render_data.reset();
        return;
    }
    render_data->frames = uint32_t(render_data->left.size());
    land(render_job.slot, std::move(render_data), render_rate, "Recorded", "RESAMPLE", "Resample");
}

// Roadmap P4: the stems' files, by the ring's rows (a stem's "NN <name>.wav", a pad stem's path in the job), then the
// mix's "Mix.wav" (not on a later pass), written by the StemWriter's thread.
bool SampleLibrary::open_stems(const std::string& folder) {
    std::error_code error;
    stem_paths.clear();
    for (unsigned row = 0; row < stems_ring->rows(); ++row) {
        const unsigned s = stems_ring->row_stem(row);
        if (s < StemRing::stem_count) {
            char file[64];
            std::snprintf(file, sizeof(file), "%02u %s.wav", s + 1, render_job.stem_names[s].data());
            stem_paths.push_back(folder + "/" + file);
        } else {
            const std::string path = folder + "/" + render_job.pad_paths[s - StemRing::stem_count].data() + ".wav";
            fs::create_directories(fs::path(path).parent_path(), error);
            stem_paths.push_back(path);
        }
    }
    if (!render_job.more) stem_paths.push_back(folder + "/Mix.wav");
    if (!stem_writer) stem_writer = std::make_unique<StemWriter>();
    stems_open = stem_writer->open(stem_paths, render_rate);
    if (!stems_open) return false;
    stems_folder = folder;
    stems_delayed = stems_closing = stems_cancelled = stems_failed = false;
    return true;
}

// The stems' frames as they come, each row onto its file; a pad stem first gets the silence its track's chain was
// late by (StemRing::pad_delay), once, so it meets its kit.
void SampleLibrary::drain_stems() {
    constexpr unsigned chunk = 4096;
    const unsigned rows = stems_ring->rows();
    stem_chunk.resize(size_t(std::max(rows, 1u)) * 2 * chunk);
    bool ok = true;
    for (unsigned n; (n = stems_ring->pop_rows(stem_chunk.data(), chunk)) > 0;) {
        if (!stems_delayed) {
            stems_delayed = true;
            for (unsigned row = 0; row < rows; ++row) {
                const unsigned s = stems_ring->row_stem(row);
                if (s >= StemRing::stem_count)
                    stem_writer->add_silence(row, stems_ring->pad_delay(render_job.pad_tracks[s - StemRing::stem_count]));
            }
        }
        for (unsigned row = 0; row < rows; ++row) ok = stem_writer->add(row, stem_chunk.data() + size_t(row) * 2 * chunk, n) && ok;
    }
    (void)ok; // a queue that ran over fails the files (close())
}

void SampleLibrary::close_stems() {
    stems_closing = false;
    stems_open = false;
    std::vector<float> loudest;
    const bool written = stem_writer->close(&loudest);
    const bool slow = stems_failed || (stems_ring && stems_ring->failed()) || !written;
    // A pad stem that never sounded is not kept, nor a pad folder left empty.
    std::error_code error;
    for (unsigned row = 0; row < loudest.size() && row < stem_paths.size(); ++row) {
        if (!stems_ring || row >= stems_ring->rows() || stems_ring->row_stem(row) < StemRing::stem_count || loudest[row] > 0) continue;
        fs::remove(stem_paths[row], error);
        const fs::path folder = fs::path(stem_paths[row]).parent_path();
        if (fs::is_empty(folder, error)) fs::remove(folder, error);
    }
    const std::string name = fs::path(stems_folder).filename().string();
    if (stems_cancelled) { stems_done_.store(-1, std::memory_order_release); finish("Stems stopped"); return; }
    if (slow) { stems_done_.store(-1, std::memory_order_release); finish("Stems: card too slow"); return; }
    stems_done_.store(1, std::memory_order_release);
    if (render_job.pass >= render_job.passes) finish("Stems " + name); // a pass before the last: the next one says
}

StemRing* SampleLibrary::stem_ring() {
    if (!stems_ring) stems_ring = std::make_unique<StemRing>();
    return stems_ring.get();
}

bool SampleLibrary::busy() const {
    return rendering || stems_closing || !landings.empty() || pool.pending() || host.recorder().state() != TakeRecorder::State::Idle // an instrument still being laid out writes to it too
           || separator.busy(); // stems are read from the card and written to it
}

} // namespace melodee
