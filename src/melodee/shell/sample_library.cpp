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
#include <cstdio>
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
std::string SampleLibrary::folder() const { std::lock_guard<std::mutex> guard(lock); return known.folder; }
Drive SampleLibrary::drive() const { std::lock_guard<std::mutex> guard(lock); return known.drive; }
bool SampleLibrary::top() const { std::lock_guard<std::mutex> guard(lock); return known.top; }
int SampleLibrary::came_from() const { std::lock_guard<std::mutex> guard(lock); return known.came_from; }
std::vector<SampleFile> SampleLibrary::files() const { std::lock_guard<std::mutex> guard(lock); return known.files; }
std::string SampleLibrary::previewed() const { std::lock_guard<std::mutex> guard(lock); return preview_heard; }

void SampleLibrary::browse(Drive drive, const std::string& folder) {
    std::lock_guard<std::mutex> guard(lock);
    wanted.pending = true; wanted.up = false; wanted.drive = drive; wanted.folder = folder;
}
void SampleLibrary::browse_up() {
    std::lock_guard<std::mutex> guard(lock);
    wanted.pending = true; wanted.up = true;
}
void SampleLibrary::preview(const std::string& path) {
    std::lock_guard<std::mutex> guard(lock);
    preview_wanted = path; preview_pending = true;
    if (path.empty()) preview_heard.clear();
}

std::vector<std::string> SampleLibrary::internal_tops() {
    std::vector<std::string> tops;
    if (const char* forced = std::getenv("MELODEE_INTERNAL")) { // a host: folders split by ':'
        std::string list = forced;
        for (size_t at = 0; at <= list.size();) {
            size_t colon = list.find(':', at);
            if (colon == std::string::npos) colon = list.size();
            if (colon > at) tops.push_back(list.substr(at, colon - at));
            at = colon + 1;
        }
        return tops;
    }
    return {"/content/Samples", "/data/UserContent/Samples"}; // the factory library, the stock app's own
}

namespace {
std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return text;
}
bool is_wav(const fs::path& path) { return upper(path.extension().string()) == ".WAV"; }
// A folder's folders and WAV files, appended; hidden ones and lost+found left out.
void list_into(const std::string& folder, std::vector<SampleFile>& out) {
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) return;
    for (auto it = fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, error);
         !error && it != fs::directory_iterator() && out.size() < 4000; it.increment(error)) {
        auto name = it->path().filename().string();
        if (name.empty() || name[0] == '.' || name == "lost+found") continue;
        bool folder_here = it->is_directory(error);
        if (!folder_here && !(it->is_regular_file(error) && is_wav(it->path()))) continue;
        SampleFile f;
        f.path = it->path().string(); f.folder = folder_here;
        f.name = upper(folder_here ? name : it->path().stem().string()).substr(0, 26);
        if (!folder_here) f.bytes = it->file_size(error);
        out.push_back(std::move(f));
    }
}
} // namespace

void SampleLibrary::service(Hardware& hardware) {
    const Platform& platform = hardware.platform;
    // The input Record listens to: the line inputs or the microphone, else off.
    int wanted_input = input_wanted.load();
    if (wanted_input != input_routed && hardware.mixer.available()) {
        hardware.mixer.input(wanted_input == 0 ? "mic" : wanted_input == 1 ? "line" : "off");
        input_routed = wanted_input;
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
    SampleJob job;
    if (requests.pop(request)) read(request); // one file per pass keeps the thread answering
    else if (jobs.pop(job)) make(job);
    else {
        std::string heard;
        bool pending = false;
        { std::lock_guard<std::mutex> guard(lock); std::swap(pending, preview_pending); heard = preview_wanted; }
        if (pending && !heard.empty()) hear(heard);
    }
    // The card's Samples folder: $MELODEE_SAMPLES, else the first card's, looked for every few seconds.
    bool browse_wanted;
    { std::lock_guard<std::mutex> guard(lock); browse_wanted = wanted.pending; }
    if (++ticks % 600 == 1 || scan_wanted.load() || browse_wanted) {
        std::string root, top;
        if (const char* forced = std::getenv("MELODEE_SAMPLES")) root = top = forced;
        else
            for (const auto& space : platform.storage())
                if (space.path.rfind("/media/", 0) == 0 && fs::is_directory(space.path + "/Samples")) { top = space.path; root = space.path + "/Samples"; break; }
        bool changed = root != root_seen;
        if (changed) { root_seen = root; card_top = top; std::lock_guard<std::mutex> guard(lock); known.root = root; }
        scan_wanted.store(false);
        scan();
    }
}

// The folder the browser is in, after what the MIDI thread asked: another
// folder, up one, or another drive. Only a listing that differs counts as a change.
void SampleLibrary::scan() {
    Wanted ask;
    Drive drive;
    std::string folder;
    {
        std::lock_guard<std::mutex> guard(lock);
        ask = wanted; wanted.pending = false;
        drive = known.drive; folder = known.folder;
    }
    auto tops = internal_tops();
    auto start_of = [&](Drive d) { return d == Drive::Internal ? std::string() : root_seen.empty() ? card_top : root_seen; };
    std::string left; // going up: the folder left, to put the cursor on
    if (ask.pending && ask.up) {
        bool at_top = drive == Drive::Internal ? folder.empty() : (folder.empty() || folder == card_top);
        if (!at_top) {
            left = folder;
            auto parent = fs::path(folder).parent_path().string();
            bool to_top = drive == Drive::Internal && std::find(tops.begin(), tops.end(), parent) != tops.end();
            folder = to_top ? std::string() : parent;
        }
    } else if (ask.pending) {
        drive = ask.drive;
        folder = ask.folder.empty() ? start_of(drive) : ask.folder;
    }
    // A card taken out, or a folder gone: back to the drive's start.
    std::error_code error;
    if (drive == Drive::Card && (card_top.empty() || folder.rfind(card_top, 0) != 0)) folder = start_of(drive);
    if (!folder.empty() && !fs::is_directory(folder, error)) folder = start_of(drive);
    std::vector<SampleFile> files;
    if (drive == Drive::Internal && folder.empty()) for (const auto& t : tops) list_into(t, files);
    else list_into(folder, files);
    std::sort(files.begin(), files.end(), [](const SampleFile& a, const SampleFile& b) {
        return a.folder != b.folder ? a.folder : a.name != b.name ? a.name < b.name : a.path < b.path;
    });
    int came_from = -1;
    for (size_t n = 0; n < files.size() && !left.empty(); ++n) if (files[n].path == left) came_from = int(n);
    bool top = drive == Drive::Internal ? folder.empty() : (folder.empty() || folder == card_top);
    std::lock_guard<std::mutex> guard(lock);
    bool same = known.drive == drive && known.folder == folder && known.files.size() == files.size()
        && std::equal(files.begin(), files.end(), known.files.begin(), [](const SampleFile& a, const SampleFile& b) { return a.path == b.path; });
    if (same && !ask.pending) return;
    known.drive = drive; known.folder = folder; known.top = top;
    known.files = std::move(files);
    known.came_from = came_from;
    changes.fetch_add(1, std::memory_order_release);
}

void SampleLibrary::finish(const std::string& text, int failed) {
    Done d;
    std::snprintf(d.text, sizeof(d.text), "%s", text.c_str());
    d.failed = int16_t(failed);
    finished.push(d);
}

// A name free in `folder`: the name, else "name 2", "name 3" ... (as Projects do).
std::string SampleLibrary::free_name(const std::string& folder, const std::string& name) const {
    std::string clean;
    for (char ch : name) clean += (ch == '/' || ch == '\\' || ch == ':' || ch == '"' || ch == '*' || ch == '?' || ch == '<' || ch == '>' || ch == '|') ? '-' : ch;
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    if (clean.empty()) clean = "Sample";
    std::error_code error;
    for (unsigned n = 1; n < 1000; ++n) {
        auto path = folder + "/" + clean + (n > 1 ? " " + std::to_string(n) : std::string()) + ".wav";
        if (!fs::exists(path, error)) return path;
    }
    return {};
}

// Save Sample, Trim Sample, Extract: a slot's region as a WAV file on the
// card. Derive also puts the new file in a slot of its own (in memory when
// there is no card), as a load would.
void SampleLibrary::make(const SampleJob& job) {
    const char* what = job.kind == SampleJob::Kind::Save ? "Save Sample" : "Sample";
    const SampleData* source = job.source < sample_slots ? data_of[job.source] : nullptr; // only this thread writes it
    auto fail = [&](const std::string& why) { // the MIDI thread lets go of the slot it held
        finish(std::string(what) + ": " + why, job.kind == SampleJob::Kind::Derive ? int(job.target % sample_slots) : -1);
    };
    if (!source || !source->frames) { fail("not loaded"); return; }
    uint32_t to = job.to && job.to <= source->frames ? job.to : source->frames, from = std::min(job.from, to);
    if (to - from < 2) { fail("too short"); return; }
    auto data = std::make_unique<SampleData>();
    data->frames = to - from;
    data->left.assign(source->left.begin() + from, source->left.begin() + to);
    data->right.assign(source->right.begin() + from, source->right.begin() + to);
    std::string root;
    { std::lock_guard<std::mutex> guard(lock); root = known.root; }
    std::string name(job.name), path, error;
    if (!root.empty()) {
        std::string folder = job.folder[0] ? root + "/" + job.folder : root;
        std::error_code made;
        fs::create_directories(folder, made);
        path = free_name(folder, name);
        error = path.empty() ? std::string("no free name") : write_wav(path, *data);
        if (!error.empty()) path.clear();
        else name = fs::path(path).stem().string();
    } else error = "no card";
    if (job.kind == SampleJob::Kind::Save) {
        if (path.empty()) { fail(error); return; }
        finish("Saved " + upper(name));
        scan_wanted.store(true); // it shows in the browser
        return;
    }
    LoadedSample loaded;
    loaded.name = upper(name).substr(0, 27);
    loaded.path = path;
    loaded.frames = data->frames;
    make_overview(*data, loaded.overview);
    loaded.hit_count = find_hits(*data, loaded.hits);
    if (!host.offer_sample(job.target % sample_slots, data.get())) { fail("busy, try again"); return; }
    const SampleData* held = data.get();
    owned.push_back(std::move(data));
    loaded.ready = true;
    if (!path.empty()) scan_wanted.store(true);
    else finish("Not on the card: " + error);
    std::lock_guard<std::mutex> guard(lock);
    known.slots[job.target % sample_slots] = loaded;
    data_of[job.target % sample_slots] = held;
    changes.fetch_add(1, std::memory_order_release);
}

// The browser's preview (B3): the file in the preview slot, at most 30 s of it.
void SampleLibrary::hear(const std::string& path) {
    auto data = std::make_unique<SampleData>();
    if (!read_wav(path, *data, 30).empty() || !data->frames) return;
    if (!host.offer_sample(preview_slot, data.get())) return;
    owned.push_back(std::move(data));
    { std::lock_guard<std::mutex> guard(lock); preview_heard = path; }
    preview_count.fetch_add(1, std::memory_order_release);
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
