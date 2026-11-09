// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/sample_library.h"
#include "drivers/platform.h"
#include "melodee/engine/engine_host.h"
#include <algorithm>
#include <cctype>
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
std::string SampleLibrary::root() const { std::lock_guard<std::mutex> guard(lock); return known.root; }
std::vector<SampleFile> SampleLibrary::files() const { std::lock_guard<std::mutex> guard(lock); return known.files; }

void SampleLibrary::service(const Platform& platform) {
    // Samples the audio thread has let go of.
    const SampleData* gone = nullptr;
    while (host.take_retired(gone))
        owned.erase(std::remove_if(owned.begin(), owned.end(), [&](const auto& p) { return p.get() == gone; }), owned.end());
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

void SampleLibrary::read(const Request& request) {
    auto data = std::make_unique<SampleData>();
    auto error = read_wav(request.path, *data);
    LoadedSample loaded;
    loaded.name = fs::path(request.path).stem().string();
    std::transform(loaded.name.begin(), loaded.name.end(), loaded.name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (!error.empty()) {
        loaded.error = error;
    } else {
        loaded.frames = data->frames;
        make_overview(*data, loaded.overview);
        loaded.hit_count = find_hits(*data, loaded.hits);
        if (!host.offer_sample(request.slot, data.get())) { loaded.error = "busy, try again"; }
        else { owned.push_back(std::move(data)); loaded.ready = true; }
    }
    std::lock_guard<std::mutex> guard(lock);
    known.slots[request.slot] = loaded;
    changes.fetch_add(1, std::memory_order_release);
}

} // namespace melodee
