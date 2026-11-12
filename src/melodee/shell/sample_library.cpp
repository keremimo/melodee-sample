// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/sample_library.h"
#include "melodee/shell/chop.h"
#include "melodee/shell/mpc_program.h"
#include "melodee/shell/sample_decode.h"
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
#include <iterator>
#include <fstream>

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
bool SampleLibrary::thumbnail(unsigned n, uint32_t from, uint32_t to, uint8_t* out, unsigned columns) const {
    std::lock_guard<std::mutex> guard(lock);
    const auto& s = known.slots[n % sample_slots];
    if (!s.ready || !s.frames || !columns) return false;
    to = std::min(to ? to : s.frames, s.frames);
    from = std::min(from, to);
    double width = s.overview.size();
    double a = from * width / s.frames, b = std::max(a + 1, to * width / s.frames);
    for (unsigned c = 0; c < columns; ++c) {
        unsigned c0 = unsigned(a + (b - a) * c / columns), c1 = std::max(c0 + 1, unsigned(a + (b - a) * (c + 1) / columns));
        uint8_t peak = 0;
        for (unsigned k = c0; k < c1 && k < s.overview.size(); ++k) peak = std::max(peak, s.overview[k]);
        out[c] = peak;
    }
    return true;
}
float SampleLibrary::peak(unsigned n, uint32_t from, uint32_t to) const {
    std::lock_guard<std::mutex> guard(lock);
    const SampleData* data = data_of[n % sample_slots];
    if (!data || !data->frames) return -1;
    to = std::min(to ? to : data->frames, data->frames);
    float top = 0;
    for (uint32_t f = std::min(from, to); f < to; ++f) top = std::max({top, std::fabs(data->left[f]), std::fabs(data->right[f])});
    return top;
}

uint32_t SampleLibrary::zero_crossing(unsigned n, uint32_t frame) const {
    std::lock_guard<std::mutex> guard(lock);
    const SampleData* data = data_of[n % sample_slots];
    return data && data->frames ? melodee::zero_crossing(*data, frame) : frame;
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
void SampleLibrary::install(const std::string& path) {
    std::lock_guard<std::mutex> guard(lock);
    installs_wanted.push_back(path);
}

void SampleLibrary::install_service() {
    if (!installing) {
        std::string path, root;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (installs_wanted.empty()) return;
            path = installs_wanted.front();
            installs_wanted.erase(installs_wanted.begin());
            root = known.root;
        }
        if (root.empty()) { finish("Install: no card"); return; }
        auto next = std::make_unique<ExpansionInstall>();
        auto error = next->open(path, (fs::path(root) / "Expansions").string());
        if (!error.empty()) { finish("Install: " + error); return; }
        finish("Installing " + next->title());
        installing = std::move(next);
        return;
    }
    std::string error;
    if (installing->step(error)) return;
    if (!error.empty()) finish("Install: " + error);
    else {
        finish("Installed " + installing->title());
        browse(Drive::Card, installing->folder()); // its programs, in view
    }
    installing.reset();
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
bool is_bank(const fs::path& path) { return upper(path.extension().string()) == ".MLDB"; }
bool is_instrument(const fs::path& path) { // an .sfz, an MPC keygroup program (.xpm, .xty), an EXS24 instrument, or a pad of an MPC program or project
    std::string file, which;
    if (split_program_pad(path.string(), file, which)) return true;
    const auto kind = upper(path.extension().string());
    return kind == ".SFZ" || kind == ".XPM" || kind == ".XTY" || kind == ".EXS";
}
// "<name>_[TrackData]" beside <name>.xty (and an .xpj's, an .xpm's data folder beside its file).
bool owned_data(const fs::path& dir) {
    const std::string name = dir.filename().string();
    for (const char* tail : {"_[TrackData]", "_[ProjectData]", "_[ProgramData]"}) {
        const std::string t = tail;
        if (name.size() <= t.size() || name.compare(name.size() - t.size(), t.size(), t)) continue;
        const std::string stem = name.substr(0, name.size() - t.size());
        std::error_code error;
        for (const char* kind : {".xty", ".xpj", ".xpm", ".XTY", ".XPJ", ".XPM"})
            if (fs::exists(dir.parent_path() / (stem + kind), error)) return true;
    }
    return false;
}
bool is_expansion(const fs::path& path) { return upper(path.extension().string()) == ".XPN"; } // an MPC expansion: a zip to install
// A folder's folders, WAV files and bank files, appended; hidden ones and lost+found left out.
// card_top: the card's own top folder, where its boot folder (autorun, what starts the app) is no sample's.
void list_into(const std::string& folder, std::vector<SampleFile>& out, bool card_top = false) {
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) return;
    for (auto it = fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, error);
         !error && it != fs::directory_iterator() && out.size() < 4000; it.increment(error)) {
        auto name = it->path().filename().string();
        if (name.empty() || name[0] == '.' || name == "lost+found" || (card_top && name == "autorun")) continue;
        bool folder_here = it->is_directory(error);
        if (folder_here && name == "[Previews]") continue; // an expansion's program previews: Preview plays them on the program
        if (folder_here && owned_data(it->path())) continue;  // a program's or project's own samples: it is the row
        bool bank = !folder_here && is_bank(it->path());
        bool instrument = !folder_here && is_instrument(it->path());
        bool expansion = !folder_here && is_expansion(it->path());
        if (!folder_here && !(it->is_regular_file(error) && (is_wav(it->path()) || bank || instrument || expansion))) continue;
        SampleFile f;
        f.path = it->path().string(); f.folder = folder_here; f.bank = bank; f.instrument = instrument; f.expansion = expansion;
        if (instrument && is_drum_program(f.path)) { f.kit = true; f.instrument = false; }
        f.name = upper(folder_here ? name : it->path().stem().string()).substr(0, 26);
        if (!folder_here) f.bytes = it->file_size(error);
        out.push_back(std::move(f));
    }
}
} // namespace

void SampleLibrary::service(Hardware& hardware) {
    const Platform& platform = hardware.platform;
    if (host.recorder().state() == TakeRecorder::State::Done) save_take();
    drain_render(); // a render's frames, as they come, every pass
    RecallJob recall_job;
    if (recalls.pop(recall_job)) save_recall(recall_job);
    loop_service(); // the Looper's commit and files (shell/looper_files.cpp)
    host.streams().start();
    reap(); // samples the audio thread has let go of
    refresh_playback();
    land_next();
    Request request;
    SampleJob job;
    bank_files();
    install_service();
    if (requests.pop(request)) read(request); // one file per pass keeps the thread answering
    else if (jobs.pop(job)) make(job);
    else {
        std::string heard;
        bool pending = false;
        { std::lock_guard<std::mutex> guard(lock); std::swap(pending, preview_pending); heard = preview_wanted; }
        if (pending && !heard.empty()) hear(heard);
    }
    // The card's Samples folder: $MELODEE_SAMPLES, else the first card's, looked for every few seconds. A card
    // without one is still a card: what Melodee writes makes the folder, and browsing starts at the card's top.
    bool browse_wanted;
    { std::lock_guard<std::mutex> guard(lock); browse_wanted = wanted.pending; }
    if (++ticks % 600 == 1 || scan_wanted.load() || browse_wanted) {
        std::string root, top;
        if (const char* forced = std::getenv("MELODEE_SAMPLES")) root = top = forced;
        else
            for (const auto& space : platform.storage())
                if (space.path.rfind("/media/", 0) == 0) { top = space.path; root = space.path + "/Samples"; break; }
        bool changed = root != root_seen;
        if (changed) { root_seen = root; std::lock_guard<std::mutex> guard(lock); card_top = top; known.root = root; }
        if (changed) {
            cache.set_folder(top.empty() ? std::string() : top + "/Melodee/Cache");
            if (!top.empty() && !cache_tidied) { cache_tidied = true; cache.tidy(); }
        }
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
    std::error_code error;
    auto start_of = [&](Drive d) { return d == Drive::Internal ? std::string() : fs::is_directory(root_seen, error) ? root_seen : card_top; };
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
    if (drive == Drive::Card && (card_top.empty() || folder.rfind(card_top, 0) != 0)) folder = start_of(drive);
    if (!folder.empty() && !fs::is_directory(folder, error)) folder = start_of(drive);
    std::vector<SampleFile> files;
    if (drive == Drive::Internal && folder.empty()) for (const auto& t : tops) list_into(t, files);
    else list_into(folder, files, drive == Drive::Card && folder == card_top);
    // MPC drum programs (kits) first, then instruments (keygroup programs, SFZ), as an expansion's folder holds
    // them among its samples; then folders, then files.
    std::sort(files.begin(), files.end(), [](const SampleFile& a, const SampleFile& b) {
        if (a.kit != b.kit) return a.kit;
        if (a.instrument != b.instrument) return a.instrument;
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
std::string SampleLibrary::free_name(const std::string& folder, const std::string& name, const char* extension) const {
    std::string clean;
    for (char ch : name) clean += (ch == '/' || ch == '\\' || ch == ':' || ch == '"' || ch == '*' || ch == '?' || ch == '<' || ch == '>' || ch == '|') ? '-' : ch;
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    if (clean.empty()) clean = "Sample";
    std::error_code error;
    for (unsigned n = 1; n < 1000; ++n) {
        auto path = folder + "/" + clean + (n > 1 ? " " + std::to_string(n) : std::string()) + extension;
        if (!fs::exists(path, error)) return path;
    }
    return {};
}

void SampleLibrary::write_bank(const std::string& name, std::vector<uint8_t> bytes) {
    std::lock_guard<std::mutex> guard(lock);
    bank_writes.emplace_back(name, std::move(bytes));
}
void SampleLibrary::read_bank(const std::string& path) {
    std::lock_guard<std::mutex> guard(lock);
    bank_reads.push_back(path);
}
bool SampleLibrary::take_bank(BankBytes& out) {
    std::lock_guard<std::mutex> guard(lock);
    if (banks_read.empty()) return false;
    out = std::move(banks_read.front());
    banks_read.erase(banks_read.begin());
    return true;
}

// Kit bank files: a few kilobytes each, written and read whole. They go
// in the card's Samples/Banks, so the browser lists them with the samples.
void SampleLibrary::bank_files() {
    decltype(bank_writes) writes;
    decltype(bank_reads) reads;
    std::string root;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (bank_writes.empty() && bank_reads.empty()) return;
        std::swap(writes, bank_writes); std::swap(reads, bank_reads);
        root = known.root;
    }
    for (auto& [name, bytes] : writes) {
        if (root.empty()) { finish("Save Bank: no card"); continue; }
        std::error_code error;
        auto folder = root + "/Banks";
        fs::create_directories(folder, error);
        auto path = free_name(folder, name, ".mldb");
        if (path.empty()) { finish("Save Bank: no free name"); continue; }
        auto temporary = path + ".new";
        bool wrote;
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            wrote = bool(file);
        }
        if (wrote) fs::rename(temporary, path, error);
        if (!wrote || error) { fs::remove(temporary, error); finish("Save Bank: could not write"); continue; }
        finish("Saved " + fs::path(path).stem().string());
        scan_wanted.store(true); // the Banks folder, if the browser is in it
    }
    for (auto& path : reads) {
        BankBytes got;
        got.path = path;
        std::ifstream file(path, std::ios::binary);
        if (!file) got.error = "could not read";
        else got.bytes.assign(std::istreambuf_iterator<char>(file), {});
        std::lock_guard<std::mutex> guard(lock);
        banks_read.push_back(std::move(got));
    }
}

// Save Sample, Trim Sample, Extract: a slot's region as a WAV file on the
// card. Derive also puts the new file in a slot of its own (in memory when
// there is no card), as a load would.
// The region is cut from the source's own frames when those are what plays (a
// take, a 48 kHz file at 48 kHz) and written at their rate, so nothing is
// converted; else from its 44.1 kHz frames, written at 44.1 kHz.
// Extract's slice: 2 ms fades at the ends that were cut inside the source,
// as the sampler plays a region's (engine/sampler.cpp), so the file does not
// click where the slice was cut mid-wave.
static void fade_edges(SampleData& data, unsigned rate, bool in, bool out) {
    uint32_t edge = std::min<uint32_t>(rate / 500, data.frames / 2);
    for (uint32_t k = 0; k < edge; ++k) {
        float g = float(k) / float(edge);
        if (in) { data.left[k] *= g; data.right[k] *= g; }
        if (out) { data.left[data.frames - 1 - k] *= g; data.right[data.frames - 1 - k] *= g; }
    }
}

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
    unsigned rate = unsigned(dsp::model_rate);
    const float *l = source->left.data(), *r = source->right.data();
    uint32_t first = from, end = to, all = source->frames;
    if (source->plays_own()) { // the region's 44.1 kHz frames as the source's own
        rate = source->source_rate; all = source->play_frames;
        auto own = [&](uint32_t f) { return uint32_t(std::min<uint64_t>(all, (uint64_t(f) * rate + 22050) / 44100)); };
        first = own(from); end = to == source->frames ? all : own(to);
        l = source->play_left.data(); r = source->play_right.data();
    }
    data->frames = end - first;
    data->left.assign(l + first, l + end);
    data->right.assign(r + first, r + end);
    if (job.edges) fade_edges(*data, rate, first > 0, end < all);
    as_written(*data); // what plays is what the file holds
    std::string root;
    { std::lock_guard<std::mutex> guard(lock); root = known.root; }
    std::string name(job.name), path, error;
    if (!root.empty()) {
        std::string folder = job.folder[0] ? root + "/" + job.folder : root;
        std::error_code made;
        fs::create_directories(folder, made);
        path = free_name(folder, name);
        error = path.empty() ? std::string("no free name") : write_wav(path, *data, rate);
        if (!error.empty()) path.clear();
        else name = fs::path(path).stem().string();
    } else error = "no card";
    if (job.kind == SampleJob::Kind::Save) {
        if (path.empty()) { fail(error); return; }
        finish("Saved " + upper(name));
        scan_wanted.store(true); // it shows in the browser
        return;
    }
    adopt(*data, std::move(data->left), std::move(data->right), rate, dsp::sample_rate()); // its 44.1 kHz frames, its own playing
    LoadedSample loaded;
    loaded.name = upper(name).substr(0, 27);
    loaded.path = path;
    loaded.frames = data->frames;
    make_overview(*data, loaded.overview);
    loaded.hit_count = find_hits(*data, loaded.hits);
    if (!offer(job.target % sample_slots, data.get())) { fail("busy, try again"); return; }
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
    // An MPC program with a preview of its own (an expansion's [Previews]): that, as the MPC plays it.
    const std::string preview = is_mpc_program(path) ? program_preview(path) : std::string();
    if (preview.empty() && is_expansion(path)) return;
    if (preview.empty() && is_drum_program(path)) return; // a kit with no preview: nothing to hear
    if (preview.empty() && is_instrument(path)) { hear_instrument(path); return; }
    auto data = std::make_unique<SampleData>();
    if (!(preview.empty() ? read_wav(path, *data, 30, nullptr, dsp::sample_rate()) : read_audio(preview, *data, 30, nullptr, dsp::sample_rate())).empty()
        || !data->frames) return;
    if (!offer(preview_slot, data.get())) return;
    owned.push_back(std::move(data));
    { std::lock_guard<std::mutex> guard(lock); preview_heard = path; }
    preview_count.fetch_add(1, std::memory_order_release);
}

// A finished take: copied out of the recorder, written to Samples/Recorded on
// the card when there is one, and handed back as the slot the take was armed for.
void SampleLibrary::save_take() {
    auto& recorder = host.recorder();
    auto data = std::make_unique<SampleData>();
    data->frames = recorder.taken(); // its own frames, at the rate it was taken at
    data->left.assign(recorder.left(), recorder.left() + data->frames);
    data->right.assign(recorder.right(), recorder.right() + data->frames);
    unsigned slot = recorder.slot() % sample_slots, rate = recorder.take_rate();
    recorder.release();
    land(slot, std::move(data), rate, "Recorded", "TAKE", "Take");
}

bool SampleLibrary::offer(unsigned slot, SampleData* data) {
    if (data && !data->multi) {
        const float rate = dsp::sample_rate();
        size_t held = 0;
        for (const auto& d : owned) held += d->play_bytes();
        size_t wanted = size_t(double(data->frames) * rate / dsp::model_rate) * 2 * sizeof(float);
        if (held + wanted > playback_budget) prepare_playback(*data, dsp::model_rate); // past the budget: none
        else if (!data->plays_at(rate)) prepare_playback(*data, rate);                 // read from the file already, or made here
    }
    return host.offer_sample(slot, data);
}

void SampleLibrary::refresh_playback() {
    const float rate = dsp::sample_rate();
    size_t held = 0;
    for (const auto& d : owned) held += d->play_bytes();
    for (unsigned s = 0; s < sample_slots; ++s) {
        const SampleData* now = nullptr;
        std::string path;
        { std::lock_guard<std::mutex> guard(lock); now = data_of[s]; path = known.slots[s].path; }
        if (!now || now->multi || now->plays_at(rate)) continue;
        bool stale = now->play_frames > 0; // made at another rate: none at 44.1 kHz, a new one elsewhere
        bool fits = rate != dsp::model_rate && held + size_t(double(now->frames) * rate / dsp::model_rate) * 2 * sizeof(float) <= playback_budget;
        if (!stale && !fits) continue;
        // From its file again when it still reads the same (one conversion at most), else from its own frames when
        // those played (a take with no card), else from its 44.1 kHz frames.
        auto again = std::make_unique<SampleData>();
        bool read = !path.empty() && !is_instrument(path) && read_audio(path, *again, 60, nullptr, rate).empty() && again->frames == now->frames
                 && again->left == now->left;
        if (!read) {
            again = std::make_unique<SampleData>();
            if (now->plays_own()) adopt(*again, now->play_left, now->play_right, now->source_rate, rate);
            else { again->left = now->left; again->right = now->right; again->frames = now->frames; }
        }
        if (!offer(s, again.get())) return; // the queue is full: next pass
        { std::lock_guard<std::mutex> guard(lock); data_of[s] = again.get(); }
        owned.push_back(std::move(again));
        return; // one a pass
    }
}

void SampleLibrary::land(unsigned slot, std::unique_ptr<SampleData> data, unsigned rate, const char* folder, const char* stem,
                         const char* what) {
    slot %= sample_slots;
    unsigned number = take_count.fetch_add(1) + 1;
    LoadedSample loaded;
    loaded.name = std::string(stem) + " " + std::to_string(number);
    std::string root, path;
    { std::lock_guard<std::mutex> guard(lock); root = known.root; }
    if (!root.empty()) {
        std::error_code error;
        std::string dir = root + "/" + folder;
        fs::create_directories(dir, error);
        // The first free number, so the ones from earlier sessions stay.
        for (unsigned n = 1; n < 10000 && path.empty(); ++n) {
            auto name = dir + "/" + stem + " " + std::to_string(n) + ".wav";
            if (fs::exists(name, error)) continue;
            loaded.name = std::string(stem) + " " + std::to_string(n);
            path = name;
        }
    }
    // Written at its own rate; its own frames play, its 44.1 kHz frames are what the project counts.
    auto failed = keep_capture(*data, rate, path, dsp::sample_rate());
    if (!path.empty()) { if (failed.empty()) loaded.path = path; else loaded.error = "not saved: " + failed; }
    loaded.frames = data->frames;
    make_overview(*data, loaded.overview);
    loaded.hit_count = find_hits(*data, loaded.hits);
    const SampleData* held = nullptr;
    if (offer(slot, data.get())) { held = data.get(); owned.push_back(std::move(data)); loaded.ready = true; }
    else { finish(std::string(what) + ": busy, try again", int(slot)); return; }
    if (!loaded.path.empty()) scan_wanted.store(true); // it shows in the browser
    std::lock_guard<std::mutex> guard(lock);
    known.slots[slot] = loaded;
    data_of[slot] = held;
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
    cancel_landing(request.slot);
    if (is_instrument(request.path)) { begin_instrument(request); return; }
    auto data = std::make_unique<SampleData>();
    auto error = read_wav(request.path, *data, 60, nullptr, dsp::sample_rate()); // the copy the device plays, from the file
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
        if (!offer(request.slot, data.get())) { loaded.error = "busy, try again"; }
        else { held = data.get(); owned.push_back(std::move(data)); loaded.ready = true; }
    }
    std::lock_guard<std::mutex> guard(lock);
    known.slots[request.slot] = loaded;
    if (held) data_of[request.slot % sample_slots] = held;
    changes.fetch_add(1, std::memory_order_release);
}

} // namespace melodee
