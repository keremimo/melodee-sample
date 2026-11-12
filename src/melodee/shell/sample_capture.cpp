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
    if (!rendering) {
        auto state = bounce.state();
        if (renders.pop(render_job)) {
            rendering = true;
            render_rate = bounce.rate();
            render_data.reset();
            render_path.clear();
            if (render_job.to_file) {
                std::string top = card();
                std::error_code error;
                std::string folder = top + "/Melodee/Mixdowns";
                if (!top.empty()) fs::create_directories(folder, error);
                render_path = top.empty() ? std::string() : free_name(folder, render_job.name);
                if (render_path.empty() || !render_file.open(render_path, render_rate)) { // at the device's rate
                    render_path.clear();
                    finish(top.empty() ? "Mixdown: no card" : "Mixdown: cannot write");
                }
            } else {
                render_data = std::make_unique<SampleData>();
                uint32_t room = std::min<uint32_t>(render_job.frames + 64, TakeRecorder::max_frames);
                room = uint32_t(uint64_t(room) * render_rate / uint32_t(dsp::model_rate)) + 64; // its frames are the device's
                render_data->left.reserve(room); render_data->right.reserve(room);
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
    for (unsigned n; (n = bounce.pop(render_chunk.data(), 16384)) > 0;) {
        if (render_job.to_file) { if (!render_path.empty()) render_file.add(render_chunk.data(), n); }
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
    if (render_job.to_file) {
        if (render_path.empty()) return; // said already
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

bool SampleLibrary::busy() const {
    return rendering || !landings.empty() || host.recorder().state() != TakeRecorder::State::Idle; // an instrument still being laid out writes to it too
}

} // namespace melodee
