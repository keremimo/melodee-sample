// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/project_store.h"
#include "melodee/model/card_folders.h"
#include "drivers/platform.h"
#include "melodee/crash_log.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/project_file.h"
#include "melodee/shell/mpc_program.h"
#include "melodee/shell/mpc_project.h"
#include "melodee/shell/live_export.h"
#include "melodee/shell/mpc3_export.h"
#include "melodee/ui/shades.h"
#include "runtime/app.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

namespace melodee {
namespace fs = std::filesystem;
namespace {
constexpr const char* extension = ".mldp";
// The row at the top folder that shows the stock app's own folder on the card. Nothing is ever written there.
constexpr const char* stock_folder = "MPC Sample";

std::string stock_projects(const std::string& root) { return (fs::path(root).parent_path().parent_path() / "MPC-Sample" / "Projects").string(); }
// The internal drive's folders: the stock app's projects ($MELODEE_INTERNAL_PROJECTS on a host), with its autosave and
// the factory projects as folders at the top.
struct InternalFolder { const char* name; const char* path; };
constexpr InternalFolder internal_extras[] {{"Autosave", "/data/TempProjects"}, {"Demos", "/content/Projects/Demos"}, {"Kits", "/content/Projects/Kits"}};
std::string internal_projects() {
    const char* forced = std::getenv("MELODEE_INTERNAL_PROJECTS");
    return forced ? forced : "/data/UserContent/Projects/User";
}
bool in_stock(const std::string& folder) { return folder == stock_folder || folder.rfind(std::string(stock_folder) + "/", 0) == 0; }
std::string card_of(const std::string& root) { return fs::path(root).parent_path().parent_path().string(); }
// Folders at the card's top that hold nothing to open: the system's own, and dot folders.
bool skip_on_card(const fs::path& dir) {
    auto name = dir.filename().string();
    return name.empty() || name[0] == '.' || name == "System Volume Information" || name == "LOST.DIR" || name == "$RECYCLE.BIN";
}
// The directory a folder in view lists: on the card, in the stock app's card folder, or on the internal drive.
std::string folder_dir(const std::string& root, const std::string& folder) {
    if (on_card_top(folder)) return root.empty() ? std::string() : card_of(root) + folder.substr(std::strlen(card_folder));
    if (on_internal(folder)) {
        std::string rest = folder.substr(std::strlen(internal_folder)); // "" or "/<folder>..."
        if (rest.empty()) return internal_projects();
        auto cut = rest.find('/', 1);
        std::string first = rest.substr(1, cut == std::string::npos ? std::string::npos : cut - 1), tail = cut == std::string::npos ? "" : rest.substr(cut);
        for (const auto& extra : internal_extras) if (first == extra.name) return extra.path + tail;
        return internal_projects() + rest;
    }
    if (root.empty()) return {};
    if (in_stock(folder)) return stock_projects(root) + folder.substr(std::strlen(stock_folder));
    return root + (folder.empty() ? "" : "/" + folder);
}
// Where a project or folder made while `folder` is in view goes: nothing is written to the stock app's folders.
std::string write_dir(const std::string& root, const std::string& folder) {
    return folder.empty() || in_stock(folder) || on_internal(folder) || on_card_top(folder) ? root : root + "/" + folder;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}
} // namespace

std::string card_folder_of(const std::string& card, const std::string& root, const std::string& dir) {
    auto under = [&](const std::string& top) { return dir == top || dir.rfind(top + "/", 0) == 0; };
    if (!root.empty() && under(root)) return dir.size() > root.size() ? dir.substr(root.size() + 1) : std::string();
    const std::string stock = (fs::path(card) / "MPC-Sample" / "Projects").string();
    if (under(stock)) return stock_folder + dir.substr(stock.size());
    if (!card.empty() && under(card)) return card_folder + dir.substr(card.size());
    return {};
}

ProjectStore::ProjectStore() = default;
ProjectStore::~ProjectStore() {
    if (exporter.joinable()) exporter.join();
    std::string* n;
    while (notes.pop(n)) delete n;
    Request* r;
    while (requests.pop(r)) delete r;
    LoadedProject* l;
    while (loaded.pop(l)) delete l;
}

void ProjectStore::push(Request* request) { if (!requests.push(request)) delete request; }
void ProjectStore::list(const std::string& f) { push(new Request {Request::Kind::List, f, {}}); }
void ProjectStore::save(const std::string& path, std::vector<uint8_t> bytes, const std::string& from) {
    push(new Request {Request::Kind::Save, path, std::move(bytes), from});
}
void ProjectStore::load(const std::string& path) {
    load_count.fetch_add(1, std::memory_order_release); // before the request: the app sees it no later than service() does
    push(new Request {Request::Kind::Load, path, {}});
}
void ProjectStore::export_mpc3(const std::string& path, std::vector<uint8_t> bytes, const std::string& name) {
    push(new Request {Request::Kind::ExportMpc3, name, std::move(bytes), path});
}
void ProjectStore::export_live(const std::string& path, std::vector<uint8_t> bytes, const std::string& name) {
    push(new Request {Request::Kind::ExportLive, name, std::move(bytes), path});
}
std::unique_ptr<std::string> ProjectStore::take_note() {
    std::string* n = nullptr;
    return std::unique_ptr<std::string>(notes.pop(n) ? n : nullptr);
}

// The export on a thread of its own, the request's bytes moved to it: read back as the project, written beside the
// projects as Melodee/MPC 3/<name>.xpj and its data folder, or Melodee/Ableton/<name> Project/. One at a time.
void ProjectStore::start_export(Request& r) {
    const bool live = r.kind == Request::Kind::ExportLive;
    const std::string said = live ? "Live Set: " : "MPC 3: ";
    auto note = [this](std::string text) { auto* n = new std::string(std::move(text)); if (!notes.push(n)) delete n; };
    if (export_busy.load(std::memory_order_acquire)) { note(said + "an export runs"); return; }
    if (root.empty()) { note(said + "no card"); return; }
    if (exporter.joinable()) exporter.join();
    std::string clean;
    for (char ch : r.path) clean += std::string("/\\:\"*?<>|").find(ch) != std::string::npos ? '-' : ch;
    if (clean.empty()) clean = "Project";
    const fs::path folder = fs::path(root).parent_path() / (live ? "Ableton" : "MPC 3");
    std::error_code error;
    std::string name = clean;
    auto taken = [&](const std::string& n) { return fs::exists(folder / (live ? n + " Project" : n + ".xpj"), error); };
    for (unsigned n = 2; n < 1000 && taken(name); ++n) name = clean + " " + std::to_string(n);
    export_busy.store(true, std::memory_order_release);
    exporter = std::thread([this, bytes = std::move(r.bytes), from = r.from, folder, name, note, live, said] {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        auto prints = std::make_unique<PrintTable>();
        std::string why = load_project(bytes, *project, *bank, nullptr, nullptr, prints.get());
        if (!why.empty()) {
            note(said + why);
        } else if (live) {
            LiveExport report;
            const LiveColours colours = ui::track_live_colours; // each track in its colour on the MPC
            why = export_live_set(*project, *bank, folder.string(), name, report, prints.get(), prints_folder(from), audio_folder(from), &colours);
            if (!why.empty()) note(said + why);
            else {
                std::string text = "Ableton/" + name + " Project, " + std::to_string(report.tracks) + (report.tracks == 1 ? " track" : " tracks");
                if (report.missing) text += ", " + std::to_string(report.missing) + " files missing";
                if (report.skipped) text += ", " + std::to_string(report.skipped) + " sounds left out";
                if (report.mixer) text += ", mixer settings left out"; // strips, submixes, bus mutes
                note(text);
            }
        } else {
            Mpc3Export report;
            why = export_mpc3_project(*project, *bank, folder.string(), name, report, prints.get(), prints_folder(from), audio_folder(from));
            if (!why.empty()) note(said + why);
            else {
                std::string text = "MPC 3/" + name;
                const unsigned left = report.skipped_tracks + (report.missing ? 1 : 0);
                if (report.skipped_tracks) text += ", " + std::to_string(report.skipped_tracks) + (report.skipped_tracks == 1 ? " track left out" : " tracks left out");
                if (report.missing) text += ", " + std::to_string(report.missing) + " files missing";
                if (!left && report.skipped_pads) text += ", " + std::to_string(report.skipped_pads) + " sounds left out";
                if (report.mixer) text += ", mixer settings left out"; // strips, submixes, bus mutes
                note(text);
            }
        }
        export_busy.store(false, std::memory_order_release);
    });
}
void ProjectStore::duplicate(const std::string& path) { push(new Request {Request::Kind::Duplicate, path, {}}); }
void ProjectStore::remove(const std::string& path) { push(new Request {Request::Kind::Remove, path, {}}); }
void ProjectStore::new_folder() { push(new Request {Request::Kind::NewFolder, {}, {}}); }
void ProjectStore::open_at_start(const std::string& path) { push(new Request {Request::Kind::OpenAtStart, path, {}}); }
void ProjectStore::opened(const std::string& path) { if (!path.empty()) push(new Request {Request::Kind::Opened, path, {}}); }
void ProjectStore::save_settings(const std::string& text) { push(new Request {Request::Kind::Settings, text, {}}); }
void ProjectStore::rename(const std::string& path, const std::string& name) {
    push(new Request {Request::Kind::Rename, path, std::vector<uint8_t>(name.begin(), name.end())});
}
void ProjectStore::move(const std::string& path, const std::string& folder) {
    push(new Request {Request::Kind::Move, path, std::vector<uint8_t>(folder.begin(), folder.end())});
}
void ProjectStore::save_sound(const std::string& name, std::vector<uint8_t> bytes) { push(new Request {Request::Kind::SaveSound, name, std::move(bytes)}); }
void ProjectStore::append_usage(const std::string& file, const std::string& text) {
    push(new Request {Request::Kind::Usage, file, std::vector<uint8_t>(text.begin(), text.end())});
}

std::string ProjectStore::named_path(const std::string& name) const {
    std::string dir;
    { std::lock_guard<std::mutex> guard(lock); dir = known.root.empty() ? std::string() : write_dir(known.root, known.folder); }
    if (dir.empty() || name.empty()) return {};
    std::error_code error;
    for (unsigned n = 1; n < 1000; ++n) {
        auto path = dir + "/" + name + (n == 1 ? std::string() : " " + std::to_string(n)) + extension;
        if (!fs::exists(path, error)) return path;
    }
    return {};
}

// Sounds of one's own, read whole: they are a few hundred bytes each.
void ProjectStore::scan_sounds() {
    std::vector<UserSound> sounds;
    std::error_code error;
    if (!root.empty())
        for (const auto& item : fs::directory_iterator(fs::path(root).parent_path() / "Sounds", error)) {
            if (item.path().extension() != ".mlds") continue;
            UserSound s;
            unsigned version = 0;
            if (!load_sound_file(read_file(item.path().string()), s, &version)) continue;
            upgrade_params(s.engine, version, s.params); // older sounds: pad expression at its defaults
            sounds.push_back(s);
        }
    std::sort(sounds.begin(), sounds.end(), [](const UserSound& a, const UserSound& b) { return std::strcmp(a.name, b.name) < 0; });
    std::lock_guard<std::mutex> guard(lock);
    known.sounds = std::move(sounds);
    changes.fetch_add(1, std::memory_order_release);
}

std::unique_ptr<LoadedProject> ProjectStore::take_loaded() {
    LoadedProject* l = nullptr;
    return loaded.pop(l) ? std::unique_ptr<LoadedProject>(l) : nullptr;
}

void ProjectStore::view(View& out) const { std::lock_guard<std::mutex> guard(lock); out = known; }

std::string ProjectStore::free_path(const std::string& base) const {
    std::string dir;
    { std::lock_guard<std::mutex> guard(lock); dir = known.root.empty() ? std::string() : write_dir(known.root, known.folder); }
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

namespace {
// The first card among the mounts, or null.
const StorageSpace* first_card(const std::vector<StorageSpace>& storage) {
    for (const auto& space : storage)
        if (space.path.rfind("/media/", 0) == 0) return &space;
    return nullptr;
}
} // namespace

std::string ProjectStore::projects_root(const std::vector<StorageSpace>& storage) {
    if (const char* forced = std::getenv("MELODEE_PROJECTS")) return forced;
    const StorageSpace* card = first_card(storage);
    return card ? card->path + "/Melodee/Projects" : std::string();
}

ProjectStore::SettingsFile ProjectStore::read_settings_file(const std::string& root) {
    SettingsFile out;
    if (root.empty()) return out;
    std::ifstream file(fs::path(root).parent_path() / "settings");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("start=", 0) == 0) out.start = line.substr(6);
        else if (line.rfind("last=", 0) == 0) out.last = line.substr(5);
        else if (!line.empty()) out.settings += line + '\n';
    }
    return out;
}

void ProjectStore::service(Hardware& hardware) {
    // The root: $MELODEE_PROJECTS, else Melodee/Projects on the first card, looked for every few seconds.
    if (++ticks % 600 == 1) {
        auto storage = hardware.platform.storage();
        std::string found = projects_root(storage);
        const StorageSpace* card = first_card(storage);
        uint64_t free_bytes = card ? card->free_bytes : 0;
        if (found != root) {
            root = found;
            crash_log::set_folder(root.empty() ? root : fs::path(root).parent_path().string() + "/Crashes"); // melodee/crash_log.h
            folder.clear();
            std::error_code error;
            // Nothing is created on the card until something is saved there.
            // What to open at start lives next to the projects: the one set to, else the last one open.
            auto file = read_settings_file(root);
            start = std::move(file.start);
            last = std::move(file.last);
            settings = std::move(file.settings);
            {
                std::lock_guard<std::mutex> guard(lock);
                known.settings = settings; known.settings_read = !root.empty();
            }
            scan_sounds();
            scan();
            const auto& resume = !start.empty() && fs::exists(start, error) ? start : last;
            if (!started && !resume.empty() && fs::exists(resume, error)) {
                auto* l = new LoadedProject {resume, {}, read_file(resume), false, {}, {}};
                if (l->bytes.empty() || !loaded.push(l)) delete l;
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

// A project's loops (docs/LOOPER.md) sit beside it in "<name> Loops", its prints (freeze plan F2a, model/print.h)
// in "<name> Prints" and its audio tracks' takes (freeze plan F3, model/audio_track.h) in "<name> Audio"; those
// folders belong to the project, are not listed, and go wherever the project goes.
namespace {
constexpr const char* project_folders[] {" Loops", " Prints", " Audio"};
fs::path folder_of(const fs::path& project, const char* tail) { return project.parent_path() / (project.stem().string() + tail); }
bool is_loops_folder(const fs::path& dir, const char* extension) {
    auto name = dir.filename().string();
    std::error_code error;
    for (std::string tail : project_folders)
        if (name.size() > tail.size() && !name.compare(name.size() - tail.size(), tail.size(), tail) &&
            fs::exists(dir.parent_path() / (name.substr(0, name.size() - tail.size()) + extension), error))
            return true;
    return false;
}
// A dot file or folder (.Trashes, a Mac's ._ copies): never listed.
bool dot_name(const fs::path& path) { auto name = path.filename().string(); return name.empty() || name[0] == '.'; }
// An MPC project's samples, beside its .xpj: not a folder of projects.
bool is_hidden_folder(const fs::path& dir, const char* extension) {
    const std::string tail = "_[ProjectData]", name = dir.filename().string();
    return is_loops_folder(dir, extension) || (name.size() > tail.size() && !name.compare(name.size() - tail.size(), tail.size(), tail));
}
unsigned projects_in(const fs::path& dir, const char* extension) {
    unsigned count = 0;
    std::error_code error;
    for (const auto& inner : fs::directory_iterator(dir, error))
        count += !dot_name(inner.path()) && (inner.path().extension() == extension || is_mpc_project(inner.path().string()));
    return count;
}
// Freeze plan F3: the hidden folders an import's takes wait in until its project is first saved, all of them gone
// before the next import fills its own.
constexpr const char* import_prefix = ".Import ";
void clear_import_takes(const fs::path& root) {
    std::error_code error;
    std::vector<fs::path> going;
    for (const auto& entry : fs::directory_iterator(root, error)) {
        const auto name = entry.path().filename().string();
        if (!name.rfind(import_prefix, 0) && entry.is_directory(error)) going.push_back(entry.path());
    }
    for (const auto& path : going) { std::error_code e; fs::remove_all(path, e); }
}

// An MPC project read and turned into a Melodee project's bytes (on the service thread: it takes a moment).
void import_mpc(LoadedProject& l, const std::string& root) {
    auto project = std::make_unique<Project>();
    auto patterns = std::make_unique<PatternBank>();
    MpcImport report;
    const bool program = is_mpc_program(l.path); // a drum program (.xpm, .xty): a kit
    l.error = program ? import_mpc_program(l.bytes, l.path, *project, report) : import_mpc_project(l.bytes, l.path, *project, *patterns, report);
    l.bytes.clear();
    if (!l.error.empty()) { l.note = l.error; return; }
    layers_from_programs(*project); // roadmap P3: pads of two to four layers take them as their own where slots allow
    if (!root.empty()) clear_import_takes(root);
    if (!report.takes.empty() && !root.empty()) { // its Audio tracks' files, as takes, where the project will find them
        const std::string stem = std::string(import_prefix) + fs::path(l.path).stem().string();
        const fs::path from = fs::path(root) / (stem + ".mldp");
        const fs::path folder = audio_folder(from.string());
        std::error_code error;
        fs::create_directories(folder, error);
        for (const auto& [take, file] : report.takes) {
            std::error_code e;
            if (!fs::copy_file(file, folder / audio_take_name(take), fs::copy_options::overwrite_existing, e)) ++report.missing;
        }
        l.audio_from = from.string();
    }
    l.bytes = save_project(*project, *patterns);
    l.imported = true;
    l.note = "Opened a copy of " + fs::path(l.path).stem().string();
    if (report.missing) l.note = std::to_string(report.missing) + " samples missing";
    else if (report.over_limit || report.regions_left_out || report.steps_left_out || report.songs_left_out) l.note += ", some left out";
}
void carry_loops(const fs::path& from, const fs::path& to, bool copy) {
    for (const char* tail : project_folders) {
        std::error_code error;
        auto source = folder_of(from, tail);
        if (!fs::is_directory(source, error)) continue;
        auto target = folder_of(to, tail);
        fs::remove_all(target, error);
        if (copy) fs::copy(source, target, fs::copy_options::recursive, error);
        else fs::rename(source, target, error);
    }
}
// Freeze plan F2d: Save As takes the prints along, so a frozen track plays them under the new name without printing
// again, and (F3) the audio tracks' takes, which cannot be made again (a file still being written, a .part, stays behind).
void copy_prints(const fs::path& from, const fs::path& to) {
    for (const char* tail : {" Prints", " Audio"}) {
        std::error_code error;
        auto source = folder_of(from, tail);
        if (!fs::is_directory(source, error)) continue;
        auto target = folder_of(to, tail);
        fs::create_directories(target, error);
        for (const auto& entry : fs::directory_iterator(source, error)) {
            std::error_code e;
            if (!entry.is_regular_file(e) || entry.path().extension() == ".part") continue;
            fs::copy_file(entry.path(), target / entry.path().filename(), fs::copy_options::overwrite_existing, e);
        }
    }
}
} // namespace

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
        scanned.erase(r.path); // read again whatever its time says: a card's clock steps two seconds at a time
        if (!r.from.empty() && r.from != r.path) copy_prints(r.from, r.path); // Save As (freeze plan F2d)
        remember(r.path); // only the open project is saved, and before the count moves, so a leave after it keeps it
        saved.fetch_add(1, std::memory_order_release);
        break;
    }
    case Request::Kind::ExportMpc3: case Request::Kind::ExportLive:
        start_export(r);
        return;
    case Request::Kind::Load: {
        auto* l = new LoadedProject {r.path, {}, read_file(r.path), false, {}, {}};
        if (l->bytes.empty()) l->error = "Could not read";
        else if (is_mpc_project(r.path) || is_drum_program(r.path)) import_mpc(*l, root);
        if (!loaded.push(l)) delete l;
        return;
    }
    case Request::Kind::Duplicate: {
        auto base = fs::path(r.path).stem().string();
        auto target = free_path(base);
        if (!target.empty()) { fs::copy_file(r.path, target, error); carry_loops(r.path, target, true); }
        break;
    }
    case Request::Kind::Remove:
        fs::remove(r.path, error);
        for (const char* tail : project_folders) fs::remove_all(folder_of(r.path, tail), error);
        break;
    case Request::Kind::NewFolder: {
        auto dir = write_dir(root, folder);
        fs::create_directories(dir, error);
        for (unsigned n = 1; n < 1000; ++n)
            if (!fs::exists(dir + "/Folder " + std::to_string(n), error)) { fs::create_directory(dir + "/Folder " + std::to_string(n), error); break; }
        break;
    }
    case Request::Kind::OpenAtStart:
        start = start == r.path ? std::string() : r.path; // again on the same project: no longer
        write_settings(error);
        break;
    case Request::Kind::Opened:
        remember(r.path);
        break;
    case Request::Kind::Settings:
        settings = r.path;
        write_settings(error);
        break;
    case Request::Kind::Rename: {
        std::string name(r.bytes.begin(), r.bytes.end());
        auto target = fs::path(r.path).parent_path() / (name + extension);
        if (fs::exists(target, error)) { publish_message("A project has that name"); return; }
        fs::rename(r.path, target, error);
        if (error) { publish_message("Could not rename"); return; }
        carry_loops(r.path, target, false);
        if (start == r.path) { start = target.string(); write_settings(error); }
        if (last == r.path) remember(target.string());
        break;
    }
    case Request::Kind::Move: {
        auto dir = fs::path(root) / std::string(r.bytes.begin(), r.bytes.end());
        auto stem = fs::path(r.path).stem().string();
        if (fs::path(r.path).parent_path() == dir) break; // already there
        fs::path target;
        for (unsigned n = 1; n < 1000; ++n) {
            target = dir / (stem + (n == 1 ? std::string() : " " + std::to_string(n)) + extension);
            if (!fs::exists(target, error)) break;
        }
        fs::rename(r.path, target, error);
        if (error) { publish_message("Could not move"); return; }
        carry_loops(r.path, target, false);
        if (start == r.path) { start = target.string(); write_settings(error); }
        if (last == r.path) remember(target.string());
        std::lock_guard<std::mutex> guard(lock);
        known.moved_from = r.path; known.moved_to = target.string();
        break;
    }
    case Request::Kind::Usage: { // the usage log's lines, added to its file (no card: they go)
        if (root.empty()) break;
        auto dir = fs::path(root).parent_path() / "Usage";
        fs::create_directories(dir, error);
        std::ofstream file(dir / r.path, std::ios::binary | std::ios::app);
        file.write(reinterpret_cast<const char*>(r.bytes.data()), std::streamsize(r.bytes.size()));
        break;
    }
    case Request::Kind::SaveSound: {
        auto dir = fs::path(root).parent_path() / "Sounds";
        fs::create_directories(dir, error);
        auto path = (dir / (r.path + ".mlds")).string(), temporary = path + ".new";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(r.bytes.data()), std::streamsize(r.bytes.size()));
            if (!file) { publish_message("Could not save the sound"); fs::remove(temporary, error); return; }
        }
        fs::rename(temporary, path, error);
        if (error) { publish_message("Could not save the sound"); return; }
        scan_sounds();
        break;
    }
    }
    if (error) publish_message(error.message());
    scan();
}

void ProjectStore::remember(const std::string& path) {
    if (path == last) return;
    last = path;
    std::error_code error;
    write_settings(error);
}

void ProjectStore::write_settings(std::error_code& error) {
    fs::create_directories(fs::path(root).parent_path(), error);
    std::ofstream file(fs::path(root).parent_path() / "settings", std::ios::trunc);
    if (!start.empty()) file << "start=" << start << '\n';
    if (!last.empty()) file << "last=" << last << '\n';
    file << settings;
    std::lock_guard<std::mutex> guard(lock);
    known.settings = settings;
}

// The folder in view: its folders first, then its projects, newest first.
void ProjectStore::scan() {
    std::vector<ProjectEntry> entries;
    std::error_code error;
    auto dir = folder_dir(root, folder);
    if (!dir.empty() && fs::is_directory(dir, error)) {
        for (const auto& item : fs::directory_iterator(dir, error)) {
            if (dot_name(item.path())) continue;
            ProjectEntry e;
            e.path = item.path().string();
            e.name = item.path().stem().string();
            if (item.is_directory(error)) {
                if (is_hidden_folder(item.path(), extension)) continue; // a project's loops or samples
                if (on_card_top(folder) && skip_on_card(item.path())) continue;
                if (melodee_own_folder(item.path())) continue; // Melodee/Cache, Melodee/Crashes
                e.kind = ProjectEntry::Kind::Folder;
                e.projects = projects_in(item.path(), extension);
            } else if (item.path().extension() == extension) {
                e.modified = int64_t(fs::last_write_time(item.path(), error).time_since_epoch().count());
                // Its size in tracks and its tempo, read from the file itself: once while its time and size stand,
                // not again for every save, setting or usage line written beside it.
                const uint64_t bytes = uint64_t(item.file_size(error));
                auto& known_file = scanned[e.path];
                if (!known_file.ok || known_file.modified != e.modified || known_file.bytes != bytes) {
                    known_file = {e.modified, bytes, 0, 0, false};
                    auto project = std::make_unique<Project>();
                    auto patterns = std::make_unique<PatternBank>();
                    if (load_project(read_file(e.path), *project, *patterns).empty()) {
                        known_file.tracks = project->used_tracks(); known_file.tempo = project->tempo; known_file.ok = true;
                    }
                }
                if (known_file.ok) { e.tracks = known_file.tracks; e.tempo = known_file.tempo; }
            } else if (is_mpc_project(e.path)) {
                e.modified = int64_t(fs::last_write_time(item.path(), error).time_since_epoch().count()); // read only when opened: 10 MB of JSON
            } else {
                continue;
            }
            entries.push_back(std::move(e));
        }
        if (folder == internal_folder)
            for (const auto& extra : internal_extras) {
                if (!fs::is_directory(extra.path, error)) continue;
                ProjectEntry e;
                e.kind = ProjectEntry::Kind::Folder;
                e.name = extra.name;
                e.path = extra.path;
                e.projects = projects_in(e.path, extension);
                entries.push_back(std::move(e));
            }
        if (folder.empty() && fs::is_directory(stock_projects(root), error)) { // its projects may all be in folders
            ProjectEntry e;
            e.kind = ProjectEntry::Kind::Folder;
            e.name = stock_folder;
            e.path = stock_projects(root);
            e.projects = projects_in(e.path, extension);
            entries.push_back(std::move(e));
        }
        std::sort(entries.begin(), entries.end(), [](const ProjectEntry& a, const ProjectEntry& b) {
            if (a.kind != b.kind) return a.kind == ProjectEntry::Kind::Folder;
            return a.kind == ProjectEntry::Kind::Folder ? a.name < b.name : a.modified > b.modified;
        });
    }
    // Every folder under the root, for Move to folder (four deep at most).
    std::vector<std::string> folders;
    if (!root.empty()) {
        std::function<void(const fs::path&, const std::string&, unsigned)> walk = [&](const fs::path& at, const std::string& rel, unsigned depth) {
            for (const auto& item : fs::directory_iterator(at, error)) {
                if (dot_name(item.path()) || !item.is_directory(error) || folders.size() >= 128 || is_hidden_folder(item.path(), extension)) continue;
                auto name = rel.empty() ? item.path().filename().string() : rel + "/" + item.path().filename().string();
                folders.push_back(name);
                if (depth < 4) walk(item.path(), name, depth + 1);
            }
        };
        walk(root, "", 1);
        std::sort(folders.begin(), folders.end());
    }
    std::lock_guard<std::mutex> guard(lock);
    known.root = root; known.folder = folder; known.start = start;
    known.entries = std::move(entries);
    known.folders = std::move(folders);
    known.saves = saved.load();
    changes.fetch_add(1, std::memory_order_release);
}

} // namespace melodee
