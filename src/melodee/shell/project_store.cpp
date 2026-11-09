// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/project_store.h"
#include "drivers/platform.h"
#include "melodee/model/project_file.h"
#include "runtime/app.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace melodee {
namespace fs = std::filesystem;
namespace {
constexpr const char* extension = ".mldp";

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}
} // namespace

ProjectStore::ProjectStore() = default;
ProjectStore::~ProjectStore() {
    Request* r;
    while (requests.pop(r)) delete r;
    LoadedProject* l;
    while (loaded.pop(l)) delete l;
}

void ProjectStore::push(Request* request) { if (!requests.push(request)) delete request; }
void ProjectStore::list(const std::string& f) { push(new Request {Request::Kind::List, f, {}}); }
void ProjectStore::save(const std::string& path, std::vector<uint8_t> bytes) { push(new Request {Request::Kind::Save, path, std::move(bytes)}); }
void ProjectStore::load(const std::string& path) { push(new Request {Request::Kind::Load, path, {}}); }
void ProjectStore::duplicate(const std::string& path) { push(new Request {Request::Kind::Duplicate, path, {}}); }
void ProjectStore::remove(const std::string& path) { push(new Request {Request::Kind::Remove, path, {}}); }
void ProjectStore::new_folder() { push(new Request {Request::Kind::NewFolder, {}, {}}); }
void ProjectStore::open_at_start(const std::string& path) { push(new Request {Request::Kind::OpenAtStart, path, {}}); }
void ProjectStore::save_settings(const std::string& text) { push(new Request {Request::Kind::Settings, text, {}}); }

std::unique_ptr<LoadedProject> ProjectStore::take_loaded() {
    LoadedProject* l = nullptr;
    return loaded.pop(l) ? std::unique_ptr<LoadedProject>(l) : nullptr;
}

void ProjectStore::view(View& out) const { std::lock_guard<std::mutex> guard(lock); out = known; }

std::string ProjectStore::free_path(const std::string& base) const {
    std::string dir;
    { std::lock_guard<std::mutex> guard(lock); dir = known.root.empty() ? std::string() : known.root + (known.folder.empty() ? "" : "/" + known.folder); }
    if (dir.empty()) return {};
    std::error_code error;
    for (unsigned n = 1; n < 1000; ++n) {
        std::string name = base.empty() ? "Project " + std::to_string(n) : (n == 1 ? base + " copy" : base + " copy " + std::to_string(n));
        auto path = dir + "/" + name + extension;
        if (!fs::exists(path, error)) return path;
    }
    return {};
}

void ProjectStore::publish_message(const std::string& message) {
    std::lock_guard<std::mutex> guard(lock);
    known.message = message;
    changes.fetch_add(1, std::memory_order_release);
}

void ProjectStore::service(Hardware& hardware) {
    // The root: $MELODEE_PROJECTS, else Melodee/Projects on the first card, looked for every few seconds.
    if (++ticks % 600 == 1) {
        std::string found;
        uint64_t free_bytes = 0;
        if (const char* forced = std::getenv("MELODEE_PROJECTS")) found = forced;
        for (const auto& space : hardware.platform.storage())
            if (space.path.rfind("/media/", 0) == 0) {
                if (found.empty()) found = space.path + "/Melodee/Projects";
                free_bytes = space.free_bytes;
                break;
            }
        if (found != root) {
            root = found;
            folder.clear();
            std::error_code error;
            // Nothing is created on the card until something is saved there.
            // What to open at start lives next to the projects.
            start.clear();
            settings.clear();
            if (!root.empty()) {
                std::ifstream file(fs::path(root).parent_path() / "settings");
                std::string line;
                while (std::getline(file, line)) {
                    if (line.rfind("start=", 0) == 0) start = line.substr(6);
                    else if (!line.empty()) settings += line + '\n';
                }
            }
            {
                std::lock_guard<std::mutex> guard(lock);
                known.settings = settings; known.settings_read = !root.empty();
            }
            scan();
            if (!started && !start.empty() && fs::exists(start, error)) {
                auto* l = new LoadedProject {start, {}, read_file(start)};
                if (!loaded.push(l)) delete l;
            }
            started = true;
        }
        std::lock_guard<std::mutex> guard(lock);
        if (known.free_bytes != free_bytes) { known.free_bytes = free_bytes; changes.fetch_add(1, std::memory_order_release); }
    }
    Request* request = nullptr;
    if (requests.pop(request)) {
        std::unique_ptr<Request> owned(request);
        if (!root.empty()) run(*owned);
        else publish_message("No card for projects");
    }
}

void ProjectStore::run(Request& r) {
    std::error_code error;
    switch (r.kind) {
    case Request::Kind::List:
        folder = r.path;
        break;
    case Request::Kind::Save: {
        // Write beside it, then put it in place, so a pulled card never leaves half a project.
        fs::create_directories(fs::path(r.path).parent_path(), error);
        auto temporary = r.path + ".new";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(r.bytes.data()), std::streamsize(r.bytes.size()));
            if (!file) { publish_message("Could not save"); fs::remove(temporary, error); return; }
        }
        fs::rename(temporary, r.path, error);
        if (error) { publish_message("Could not save"); return; }
        saved.fetch_add(1, std::memory_order_release);
        break;
    }
    case Request::Kind::Load: {
        auto* l = new LoadedProject {r.path, {}, read_file(r.path)};
        if (l->bytes.empty()) l->error = "Could not read";
        if (!loaded.push(l)) delete l;
        return;
    }
    case Request::Kind::Duplicate: {
        auto base = fs::path(r.path).stem().string();
        auto target = free_path(base);
        if (!target.empty()) fs::copy_file(r.path, target, error);
        break;
    }
    case Request::Kind::Remove:
        fs::remove(r.path, error);
        break;
    case Request::Kind::NewFolder: {
        auto dir = root + (folder.empty() ? "" : "/" + folder);
        fs::create_directories(dir, error);
        for (unsigned n = 1; n < 1000; ++n)
            if (!fs::exists(dir + "/Folder " + std::to_string(n), error)) { fs::create_directory(dir + "/Folder " + std::to_string(n), error); break; }
        break;
    }
    case Request::Kind::OpenAtStart:
        start = start == r.path ? std::string() : r.path; // again on the same project: no longer
        write_settings(error);
        break;
    case Request::Kind::Settings:
        settings = r.path;
        write_settings(error);
        break;
    }
    if (error) publish_message(error.message());
    scan();
}

void ProjectStore::write_settings(std::error_code& error) {
    fs::create_directories(fs::path(root).parent_path(), error);
    std::ofstream file(fs::path(root).parent_path() / "settings", std::ios::trunc);
    if (!start.empty()) file << "start=" << start << '\n';
    file << settings;
    std::lock_guard<std::mutex> guard(lock);
    known.settings = settings;
}

// The folder in view: its folders first, then its projects, newest first.
void ProjectStore::scan() {
    std::vector<ProjectEntry> entries;
    std::error_code error;
    auto dir = root + (folder.empty() ? "" : "/" + folder);
    if (!root.empty() && fs::is_directory(dir, error)) {
        for (const auto& item : fs::directory_iterator(dir, error)) {
            ProjectEntry e;
            e.path = item.path().string();
            e.name = item.path().stem().string();
            if (item.is_directory(error)) {
                e.kind = ProjectEntry::Kind::Folder;
                for (const auto& inner : fs::directory_iterator(item.path(), error)) e.projects += inner.path().extension() == extension;
            } else if (item.path().extension() == extension) {
                e.modified = int64_t(fs::last_write_time(item.path(), error).time_since_epoch().count());
                // Its size in tracks and its tempo, read from the file itself.
                auto project = std::make_unique<Project>();
                auto patterns = std::make_unique<PatternBank>();
                if (load_project(read_file(e.path), *project, *patterns).empty()) { e.tracks = project->used_tracks(); e.tempo = project->tempo; }
            } else {
                continue;
            }
            entries.push_back(std::move(e));
        }
        std::sort(entries.begin(), entries.end(), [](const ProjectEntry& a, const ProjectEntry& b) {
            if (a.kind != b.kind) return a.kind == ProjectEntry::Kind::Folder;
            return a.kind == ProjectEntry::Kind::Folder ? a.name < b.name : a.modified > b.modified;
        });
    }
    std::lock_guard<std::mutex> guard(lock);
    known.root = root; known.folder = folder; known.start = start;
    known.entries = std::move(entries);
    known.saves = saved.load();
    changes.fetch_add(1, std::memory_order_release);
}

} // namespace melodee
