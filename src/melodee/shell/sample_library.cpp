// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/sample_library.h"
#include "drivers/mixer.h"
#include "drivers/platform.h"
#include "runtime/app.h"
#include "melodee/engine/engine_host.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace melodee {
namespace fs = std::filesystem;

SampleLibrary::SampleLibrary(EngineHost& h) : host(h) {}
SampleLibrary::~SampleLibrary() = default;

bool SampleLibrary::load(unsigned slot, const std::string& path) {
    Request r;
    r.slot = uint8_t(slot % sample_slots);
    std::strncpy(r.path, path.c_str(), sizeof(r.path) - 1);
    return requests.push(r);
}

void SampleLibrary::view(View& out) const { std::lock_guard<std::mutex> guard(lock); out = known; }
LoadedSample SampleLibrary::slot(unsigned n) const { std::lock_guard<std::mutex> guard(lock); return known.slots[n % sample_slots]; }
float SampleLibrary::peak(unsigned n, uint32_t from, uint32_t to) const {
    std::lock_guard<std::mutex> guard(lock);
    const SampleData* data = data_of[n % sample_slots];
    if (!data || !data->frames) return -1;
    to = std::min(to ? to : data->frames, data->frames);
    float top = 0;
    for (uint32_t f = std::min(from, to); f < to; ++f) top = std::max({top, std::fabs(data->left[f]), std::fabs(data->right[f])});
    return top;
}

std::string SampleLibrary::root() const { std::lock_guard<std::mutex> guard(lock); return known.root; }
std::vector<SampleFile> SampleLibrary::files() const { std::lock_guard<std::mutex> guard(lock); return known.files; }

void SampleLibrary::service(Hardware& hardware) {
    const Platform& platform = hardware.platform;
    // The input Record listens to: the line inputs or the microphone, else off.
    int wanted = input_wanted.load();
    if (wanted != input_routed && hardware.mixer.available()) {
        hardware.mixer.input(wanted == 0 ? "mic" : wanted == 1 ? "line" : "off");
        input_routed = wanted;
    }
    if (host.recorder().state() == TakeRecorder::State::Done) save_take();
    // Samples the audio thread has let go of.
    const SampleData* gone = nullptr;
    while (host.take_retired(gone)) {
        {
            std::lock_guard<std::mutex> guard(lock);
            for (auto& d : data_of) if (d == gone) d = nullptr;
        }
        owned.erase(std::remove_if(owned.begin(), owned.end(), [&](const auto& p) { return p.get() == gone; }), owned.end());
    }
    Request request;
    if (requests.pop(request)) read(request); // one file per pass keeps the thread answering
    // The Samples folder: $MELODEE_SAMPLES, else the first card's, looked for every few seconds.
    if (++ticks % 600 == 1 || scan_wanted.load()) {
        std::string root;
        if (const char* forced = std::getenv("MELODEE_SAMPLES")) root = forced;
        else
            for (const auto& space : platform.storage())
                if (space.path.rfind("/media/", 0) == 0 && fs::is_directory(space.path + "/Samples")) { root = space.path + "/Samples"; break; }
        if (root != root_seen || scan_wanted.exchange(false)) { root_seen = root; scan(root); }
    }
}

void SampleLibrary::scan(const std::string& root) {
    std::vector<SampleFile> files;
    std::error_code error;
    if (!root.empty() && fs::is_directory(root, error)) {
        // The folder and one level of subfolders, sorted by name.
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error);
             it != fs::recursive_directory_iterator() && files.size() < 1000; it.increment(error)) {
            if (error) break;
            if (it.depth() > 1) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(error)) continue;
            auto ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext != ".wav") continue;
            auto name = it->path().stem().string();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
            files.push_back({it->path().string(), name.substr(0, 26), it->file_size(error)});
        }
        std::sort(files.begin(), files.end(), [](const SampleFile& a, const SampleFile& b) { return a.name < b.name; });
    }
    std::lock_guard<std::mutex> guard(lock);
    known.root = root;
    known.files = std::move(files);
    changes.fetch_add(1, std::memory_order_release);
}

// A finished take: copied out of the recorder, written to Samples/Recorded on
// the card when there is one, and handed back as the slot the take was armed for.
void SampleLibrary::save_take() {
    auto& recorder = host.recorder();
    auto data = std::make_unique<SampleData>();
    data->frames = recorder.frames();
    data->left.assign(recorder.left(), recorder.left() + data->frames);
    data->right.assign(recorder.right(), recorder.right() + data->frames);
    unsigned slot = recorder.slot() % sample_slots;
    recorder.release();
    unsigned number = take_count.fetch_add(1) + 1;
    LoadedSample loaded;
    loaded.name = "TAKE " + std::to_string(number);
    std::string root;
    { std::lock_guard<std::mutex> guard(lock); root = known.root; }
    if (!root.empty()) {
        std::error_code error;
        fs::create_directories(root + "/Recorded", error);
        // The next free name, so takes from earlier sessions stay.
        for (unsigned n = number; n < number + 1000; ++n) {
            auto path = root + "/Recorded/TAKE " + std::to_string(n) + ".wav";
            if (fs::exists(path, error)) continue;
            loaded.name = "TAKE " + std::to_string(n);
            take_count.store(n);
            auto failed = write_wav(path, *data);
            if (failed.empty()) loaded.path = path; else loaded.error = "not saved: " + failed;
            break;
        }
    }
    loaded.frames = data->frames;
    make_overview(*data, loaded.overview);
    loaded.hit_count = find_hits(*data, loaded.hits);
    const SampleData* held = nullptr;
    if (host.offer_sample(slot, data.get())) { held = data.get(); owned.push_back(std::move(data)); loaded.ready = true; }
    else loaded.error = "busy, try again";
    if (!loaded.path.empty()) scan_wanted.store(true); // it shows under RECORDED
    std::lock_guard<std::mutex> guard(lock);
    known.slots[slot] = loaded;
    if (held) data_of[slot % sample_slots] = held;
    changes.fetch_add(1, std::memory_order_release);
}

void SampleLibrary::read(const Request& request) {
    if (!request.path[0]) { // an empty slot in the project now: nothing plays from it
        if (host.offer_sample(request.slot, nullptr)) {
            std::lock_guard<std::mutex> guard(lock);
            known.slots[request.slot] = LoadedSample {};
            data_of[request.slot % sample_slots] = nullptr;
            changes.fetch_add(1, std::memory_order_release);
        }
        return;
    }
    auto data = std::make_unique<SampleData>();
    auto error = read_wav(request.path, *data);
    const SampleData* held = nullptr;
    LoadedSample loaded;
    loaded.path = request.path;
    loaded.name = fs::path(request.path).stem().string();
    std::transform(loaded.name.begin(), loaded.name.end(), loaded.name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (!error.empty()) {
        loaded.error = error;
    } else {
        loaded.frames = data->frames;
        make_overview(*data, loaded.overview);
        loaded.hit_count = find_hits(*data, loaded.hits);
        if (!host.offer_sample(request.slot, data.get())) { loaded.error = "busy, try again"; }
        else { held = data.get(); owned.push_back(std::move(data)); loaded.ready = true; }
    }
    std::lock_guard<std::mutex> guard(lock);
    known.slots[request.slot] = loaded;
    if (held) data_of[request.slot % sample_slots] = held;
    changes.fetch_add(1, std::memory_order_release);
}

} // namespace melodee
