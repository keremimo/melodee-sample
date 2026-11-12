// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Multisampled instruments in the sample library: an .sfz, an MPC keygroup
// program (.xpm) or a Logic EXS24 instrument (.exs) read, then its files
// brought in one a pass so the service thread keeps answering. The zones reach
// the audio thread at once; each is silent until its file has landed. Every
// kind streams from the card alike.
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/exs.h"
#include "melodee/engine/keygroup.h"
#include "melodee/shell/mpc_program.h"
#include "melodee/shell/sample_decode.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/sf2_file.h"
#include "melodee/shell/dspreset_file.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

namespace melodee {
namespace fs = std::filesystem;
namespace {
constexpr unsigned part_seconds = 600;      // a single file of an instrument, read to be laid out
constexpr size_t most_text = 16u << 20;

std::optional<std::string> read_text(const fs::path& path) {
    std::error_code error;
    auto bytes = fs::file_size(path, error);
    if (error || bytes > most_text) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// A path as a file names it, exactly or, on a card written elsewhere, whatever the case.
fs::path find_beside(const fs::path& folder, const std::string& relative) {
    fs::path at = folder;
    std::stringstream parts(relative);
    std::string part;
    std::error_code error;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") { at = at.parent_path(); continue; }
        if (fs::exists(at / part, error)) { at /= part; continue; }
        bool found = false;
        const auto wanted = lowered(part);
        for (auto it = fs::directory_iterator(at, error); !error && it != fs::directory_iterator(); it.increment(error))
            if (lowered(it->path().filename().string()) == wanted) { at = it->path(); found = true; break; }
        if (!found) return {};
    }
    return at;
}

// What memory can be pinned; 0 where the system does not say (a host).
uint64_t memory_free() {
    std::ifstream info("/proc/meminfo");
    std::string name;
    uint64_t kb = 0;
    while (info >> name >> kb) {
        if (name == "MemAvailable:") return kb * 1024;
        std::string rest;
        std::getline(info, rest);
    }
    return 0;
}

std::string joined(const std::set<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
    return out;
}

std::string upper_stem(const std::string& path) {
    auto name = fs::path(path).stem().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return name;
}

// A keygroup program (an .xpm, or an MPC 3 .xty); a drum program is a kit (Load Kit), not one pad's instrument.
SfzRead read_keygroup_file(const std::string& path, const std::string& text, std::string* error) {
    KeygroupProgram program;
    auto why = read_program(path, text, program);
    if (why.empty() && program.type != "Keygroup") why = program.type == "Drum" ? "A drum kit: Push loads it" : "Not a keygroup";
    if (!why.empty()) { if (error) *error = why; return {}; }
    return read_keygroup(program);
}

// An MPC program's layers whose files are not beside it: left out, as the MPC plays them (silent). Expansions
// ship programs whose layers still name samples the pack leaves out (Vintage Organs' demo: a celesta under
// every organ keygroup), and those are no fault of the card's. Only a program with none found is.
SfzRead found_only(SfzRead read, const std::string& path, std::string* error) {
    const fs::path folder = fs::path(path).parent_path();
    std::map<std::string, bool> there;
    const size_t had = read.regions.size();
    read.regions.erase(std::remove_if(read.regions.begin(), read.regions.end(), [&](const SfzRegion& r) {
        auto known = there.find(r.sample);
        if (known == there.end()) known = there.emplace(r.sample, !find_beside(folder, r.sample).empty()).first;
        return !known->second;
    }), read.regions.end());
    if (read.regions.size() < had) {
        read.left += unsigned(had - read.regions.size());
        read.left_by.insert("missing files");
        if (read.regions.empty() && error) *error = "Files missing";
    }
    return read;
}

// Where an EXS instrument's sample is on this card, as Logic looks: beside it,
// in the folder it was saved from (that path's last parts, under the instrument's
// folder or the two above it), else by its name anywhere under those. The
// search by name walks the folders once, the first miss building its index.
class ExsFinder {
public:
    explicit ExsFinder(fs::path folder) : folder(std::move(folder)) {}
    std::string operator()(const std::string& name, const std::string& saved_in) {
        if (name.empty() || name.find('/') != std::string::npos) return {};
        std::vector<fs::path> roots {folder};
        if (folder.has_parent_path() && folder.parent_path() != folder) roots.push_back(folder.parent_path());
        if (roots.back().has_parent_path() && roots.back().parent_path() != roots.back()) roots.push_back(roots.back().parent_path());
        std::vector<std::string> tails {""}; // "", "Samples", "Strings/Samples", ...
        std::vector<std::string> parts;
        std::stringstream saved(saved_in);
        for (std::string part; std::getline(saved, part, '/');) if (!part.empty()) parts.push_back(part);
        for (size_t take = 1; take <= std::min<size_t>(parts.size(), 3); ++take) {
            std::string tail;
            for (size_t n = parts.size() - take; n < parts.size(); ++n) tail += parts[n] + "/";
            tails.push_back(tail);
        }
        for (const auto& root : roots)
            for (const auto& tail : tails) {
                const auto wanted = (root.lexically_relative(folder) / tail / name).lexically_normal().generic_string();
                if (!find_beside(folder, wanted).empty()) return wanted;
            }
        if (!indexed) {
            indexed = true;
            unsigned budget = 20000; // entries looked at, whatever the card holds above the instrument
            for (const auto& root : roots) index(root, 0, budget);
        }
        auto it = by_name.find(lowered(name));
        return it == by_name.end() ? std::string() : it->second.lexically_relative(folder).generic_string();
    }

private:
    void index(const fs::path& at, unsigned depth, unsigned& budget) {
        std::error_code error;
        std::vector<fs::path> below;
        for (auto it = fs::directory_iterator(at, fs::directory_options::skip_permission_denied, error);
             !error && it != fs::directory_iterator() && budget; it.increment(error), --budget) {
            const auto name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            if (it->is_directory(error)) { if (depth < 5) below.push_back(it->path()); continue; }
            by_name.emplace(lowered(name), it->path()); // the nearest root's first
        }
        for (const auto& folder_below : below) index(folder_below, depth + 1, budget);
    }
    fs::path folder;
    bool indexed = false;
    std::map<std::string, fs::path> by_name;
};

// Roadmap P3: its keyswitches (a range not named is the keys its zones switch on) and whether any zone plays on a
// release. The default is sw_default, else the range's lowest, as the format has it.
void instrument_switches(const SfzRead& read, Multisample& multi) {
    int low = read.switch_low, high = read.switch_high;
    for (const auto& z : multi.zones) {
        multi.has_release = multi.has_release || z.on_release;
        if (z.switch_key < 0 || read.switch_low >= 0) continue;
        low = low < 0 ? z.switch_key : std::min<int>(low, z.switch_key);
        high = std::max<int>(high, z.switch_key);
    }
    if (low < 0) return;
    if (high < low) high = low;
    multi.switch_low = int16_t(low); multi.switch_high = int16_t(high);
    multi.switch_default = read.switch_default >= 0 ? read.switch_default : int16_t(low);
}

bool is_exs(const std::string& path) { return lowered(fs::path(path).extension().string()) == ".exs"; }

SfzRead read_exs_file(const std::string& path, const std::string& bytes, std::string* error) {
    ExsFinder finder(fs::path(path).parent_path());
    std::string why;
    auto read = read_exs(bytes, std::ref(finder), &why);
    if (!why.empty()) { if (error) *error = why; return {}; }
    if (read.regions.empty() && read.left_by.count("missing files") && error) *error = "Files missing";
    return read;
}
} // namespace

// Roadmap P3: an .sf2 preset's zones, read from the mapped file.
SfzRead read_sf2_preset(const std::string& file, unsigned index, std::string* error) {
    auto mapped = open_sf2(file);
    if (!mapped->error.empty()) { if (error) *error = mapped->error; return {}; }
    const Sf2Preset* preset = mapped->preset(index);
    if (!preset) { if (error) *error = "Preset not found"; return {}; }
    return mapped->sf2.read(*preset);
}

// A sound of an .sf2 as the cache lays out a file: at the project's rate, its loop at its own.
bool sf2_sound(const std::string& file, const std::string& name, SampleData& out, WavInfo& info) {
    auto mapped = open_sf2(file);
    if (!mapped->error.empty()) return false;
    std::vector<float> left, right;
    unsigned rate = 0;
    if (!mapped->sf2.sound(name, left, right, rate, info.loop_start, info.loop_end) || left.empty()) return false;
    info.channels = right.empty() ? 1 : 2;
    info.rate = rate;
    if (right.empty()) right = left;
    adopt(out, std::move(left), std::move(right), rate, 0);
    return out.frames > 0;
}

SfzRead read_instrument(const std::string& path, std::string* error) {
    std::string file, which;
    unsigned preset = 0;
    if (split_sf2_preset(path, file, preset)) return read_sf2_preset(file, preset, error);
    if (is_dspreset(path)) { // roadmap P3: a DecentSampler preset, its samples beside it
        auto text = read_text(path);
        if (!text) { if (error) *error = "cannot read it"; return {}; }
        std::string why;
        auto read = read_dspreset_text(*text, &why);
        if (!why.empty()) { if (error) *error = why; return {}; }
        return found_only(std::move(read), path, error);
    }
    if (split_program_pad(path, file, which)) { // one pad of a drum program or an MPC Sample project, its layers as zones
        std::string why;
        auto pads = program_pads(file, why);
        auto found = pads.find(which);
        if (found == pads.end()) { if (error) *error = why.empty() ? "Pad not found" : why; return {}; }
        return found_only(read_drum_pad(found->second), file, error);
    }
    auto text = read_text(path);
    if (!text) { if (error) *error = "cannot read it"; return {}; }
    if (is_mpc_program(path)) return found_only(read_keygroup_file(path, *text, error), path, error);
    if (is_exs(path)) return read_exs_file(path, *text, error);
    const fs::path folder = fs::path(path).parent_path();
    return read_sfz(*text, [&](const std::string& name) -> std::optional<std::string> {
        auto found = find_beside(folder, name);
        if (found.empty()) return std::nullopt;
        return read_text(found);
    });
}

void SampleLibrary::cancel_landing(unsigned slot) {
    landings.erase(std::remove_if(landings.begin(), landings.end(), [&](const Landing& l) { return l.slot == slot; }), landings.end());
}

void SampleLibrary::begin_instrument(const Request& request) {
    LoadedSample loaded;
    loaded.path = request.path;
    loaded.name = upper_stem(request.path);
    loaded.instrument = true;
    auto finish_with = [&](const std::string& why) {
        loaded.error = why;
        std::lock_guard<std::mutex> guard(lock);
        known.slots[request.slot] = loaded;
        changes.fetch_add(1, std::memory_order_release);
    };
    std::string error;
    Landing l;
    l.slot = request.slot;
    l.folder = fs::path(request.path).parent_path().string();
    {
        std::string file;
        unsigned preset = 0;
        if (split_sf2_preset(request.path, file, preset)) l.sf2 = file; // its sounds come out of the .sf2
    }
    l.read = read_instrument(request.path, &error);
    if (!error.empty()) { finish_with(error); return; }
    if (l.read.regions.empty()) { finish_with("No sounds"); return; }
    if (cache.folder().empty()) { finish_with("No card"); return; }
    uint64_t held = 0; // what stays in memory: each zone's start, and its loop's
    for (const auto& r : l.read.regions) held += stream_chunk_bytes * 4 * (1 + (r.zone.loop != ZoneLoop::Off || r.zone.loop_end));
    if (uint64_t free = memory_free()) {
        const uint64_t headroom = uint64_t(256) << 20;
        if (free < headroom || held > free - headroom) { finish_with("Too big for memory"); return; }
    }
    auto multi = std::make_shared<Multisample>();
    std::map<std::string, uint16_t> part_of;
    for (size_t n = 0; n < l.read.regions.size(); ++n) {
        auto& region = l.read.regions[n];
        auto found = part_of.find(region.sample);
        if (found == part_of.end()) {
            found = part_of.emplace(region.sample, uint16_t(l.names.size())).first;
            l.names.push_back(region.sample);
            l.zones.emplace_back();
        }
        region.zone.part = found->second;
        l.zones[found->second].push_back(uint16_t(n));
        multi->zones.push_back(region.zone);
        { // as Stage draws it (round 3 "instrument C")
            const auto& z = region.zone;
            ZoneShape shape;
            shape.low_key = z.low_key; shape.high_key = z.high_key; shape.low_velocity = z.low_velocity; shape.high_velocity = z.high_velocity;
            shape.root = z.root; shape.part = z.part;
            shape.flags = uint8_t((z.on_release ? ZoneShape::Release : 0) | (z.switch_key >= 0 ? ZoneShape::Switched : 0));
            shape.round = z.sequence_length > 1 ? uint8_t(std::min<uint32_t>(z.sequence, 255)) : 0;
            loaded.rounds = std::max(loaded.rounds, z.sequence_length > 1 ? std::min<unsigned>(z.sequence_length, 255) : 0u);
            loaded.shapes.push_back(shape);
        }
        loaded.low_key = std::min(loaded.low_key, region.zone.low_key);
        loaded.high_key = std::max(loaded.high_key, region.zone.high_key);
        for (unsigned key = region.zone.low_key; key <= region.zone.high_key; ++key)
            for (unsigned band = (region.zone.low_velocity - 1u) / 8; band <= (region.zone.high_velocity - 1u) / 8; ++band)
                loaded.map[key] = uint16_t(loaded.map[key] | (1u << band));
    }
    multi->parts = std::make_unique<ZonePart[]>(l.names.size());
    multi->part_count = unsigned(l.names.size());
    multi->turns.assign(multi->zones.size(), 0);
    instrument_switches(l.read, *multi);
    l.multi = multi;
    loaded.zones = unsigned(multi->zones.size());
    loaded.parts = multi->part_count;
    loaded.part_state.assign(multi->part_count, PartState::Waiting);
    loaded.layers = zone_layers(loaded.shapes);
    loaded.left = l.read.left;
    loaded.ignored = joined(l.read.ignored);
    const auto& e = l.read.envelope;
    if (e.attack) loaded.stages[0] = int8_t(*e.attack > 0 ? sfz_attack_step(*e.attack) : 0);
    if (e.decay) loaded.stages[1] = int8_t(sfz_decay_step(*e.decay));
    if (e.sustain) loaded.stages[2] = int8_t(sfz_percent_step(*e.sustain));
    if (e.release) loaded.stages[3] = int8_t(sfz_release_step(*e.release));
    loaded.velocity = int8_t(std::clamp(l.read.velocity, 0.0, 100.0));
    auto data = std::make_unique<SampleData>();
    data->multi = multi;
    if (!host.offer_sample(request.slot, data.get())) { finish_with("busy, try again"); return; }
    loaded.ready = true;
    {
        std::lock_guard<std::mutex> guard(lock);
        data_of[request.slot % sample_slots] = data.get();
        known.slots[request.slot] = loaded;
        part_names[request.slot % sample_slots] = l.names;
        changes.fetch_add(1, std::memory_order_release);
    }
    owned.push_back(std::move(data));
    landings.push_back(std::move(l));
}

// One file of the instrument waiting longest: found, laid out if it must be,
// mapped, what must stay in memory pinned, and its zones let play.
void SampleLibrary::land_next() {
    if (landings.empty()) return;
    auto& l = landings.front();
    const unsigned index = l.next++;
    auto& part = l.multi->parts[index];
    bool landed = false;
    std::string why;
    do {
        const fs::path source = l.sf2.empty() ? find_beside(l.folder, l.names[index]) : fs::path(l.sf2);
        if (source.empty()) { why = "Files missing"; break; }
        const std::string file = l.sf2.empty() ? cache.file_of(source.string()) : cache.file_of(l.sf2, l.names[index]);
        LaidOut info;
        if (file.empty()) { why = "Files missing"; break; }
        if (!cache.ready(file, info)) {
            SampleData decoded;
            WavInfo wav;
            auto error = l.sf2.empty() ? read_audio(source.string(), decoded, part_seconds, &wav)
                       : sf2_sound(l.sf2, l.names[index], decoded, wav) ? std::string() : std::string("unreadable");
            if (!error.empty()) { why = "Files unreadable"; break; }
            info.frames = decoded.frames; info.channels = wav.channels; info.source_rate = wav.rate;
            info.loop_start = wav.loop_start; info.loop_end = wav.loop_end;
            if (!cache.has_room(SampleCache::bytes_of(info.frames, info.channels))) { why = "Card full"; break; }
            error = cache.lay_out(file, decoded, info);
            if (!error.empty() || !cache.ready(file, info)) { why = "Card full"; break; }
        }
        if (!SampleCache::map(file, info, part)) { why = "Cannot map"; break; }
        part.total = info.frames;
        // The region's numbers are the source's frames: the audio was laid out at the project's rate.
        const double ratio = info.source_rate ? double(dsp::model_rate) / info.source_rate : 1.0;
        auto scaled = [&](int64_t frame) { return uint32_t(std::clamp<double>(std::llround(double(frame) * ratio), 0.0, double(info.frames))); };
        bool held = true;
        for (uint16_t n : l.zones[index]) {
            const auto& region = l.read.regions[n];
            auto& z = l.multi->zones[n];
            z.start = region.offset >= 0 ? scaled(region.offset) : 0;
            z.end = region.end >= 0 ? scaled(region.end) : info.frames;
            uint32_t loop_start = region.zone.loop_start ? scaled(region.zone.loop_start) : 0;
            uint32_t loop_end = region.zone.loop_end ? scaled(region.zone.loop_end) : 0;
            // the file's own loop where the region names none, and no mode turned it off
            if (loop_end <= loop_start && info.loop_end > info.loop_start && (!region.loop_named || region.loop_asked)) {
                loop_start = scaled(info.loop_start);
                loop_end = scaled(info.loop_end);
                if (z.loop == ZoneLoop::Off) z.loop = ZoneLoop::On;
            }
            loop_start = std::max(loop_start, z.start);
            loop_end = std::min(loop_end, z.end);
            if (loop_end <= loop_start) z.loop = ZoneLoop::Off;
            z.loop_start = loop_start; z.loop_end = loop_end;
            held = held && host.streams().hold(part, z.start, stream_chunk);
            if (z.loop != ZoneLoop::Off) held = held && host.streams().hold(part, z.loop_start, stream_chunk);
        }
        if (!held) { why = "Too big for memory"; break; }
        part.frames.store(info.frames, std::memory_order_release);
        landed = true;
    } while (false);
    const unsigned slot = l.slot;
    const bool done = l.next >= l.multi->part_count;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto& shown = known.slots[slot];
        if (landed) ++shown.parts_ready;
        else { ++shown.parts_failed; if (shown.error.empty()) shown.error = why; }
        if (index < shown.part_state.size()) shown.part_state[index] = landed ? PartState::Landed : PartState::Failed;
        changes.fetch_add(1, std::memory_order_release);
    }
    if (done) landings.erase(landings.begin());
}

// The browser's preview: the zone nearest middle C at a middling velocity, plainly.
void SampleLibrary::hear_instrument(const std::string& path) {
    std::string error;
    auto read = read_instrument(path, &error);
    if (read.regions.empty()) return;
    const SfzRegion* best = nullptr;
    int distance = 1 << 20;
    for (const auto& r : read.regions) {
        int away_by = 60 < r.zone.low_key ? r.zone.low_key - 60 : 60 > r.zone.high_key ? 60 - r.zone.high_key : 0;
        if (100 < r.zone.low_velocity || 100 > r.zone.high_velocity) away_by += 128;
        if (away_by < distance) { distance = away_by; best = &r; }
    }
    if (!best) return;
    auto data = std::make_unique<SampleData>();
    std::string file;
    unsigned preset = 0;
    if (split_sf2_preset(path, file, preset)) { // roadmap P3: the sound out of the .sf2
        WavInfo info;
        if (!sf2_sound(file, best->sample, *data, info)) return;
    } else {
        const fs::path source = find_beside(fs::path(path).parent_path(), best->sample);
        if (source.empty()) return;
        if (!read_audio(source.string(), *data, 30, nullptr).empty() || !data->frames) return;
    }
    if (!host.offer_sample(preview_slot, data.get())) return;
    owned.push_back(std::move(data));
    { std::lock_guard<std::mutex> guard(lock); preview_heard = path; }
    preview_count.fetch_add(1, std::memory_order_release);
}

bool SampleLibrary::reap() {
    bool any = false;
    const SampleData* gone = nullptr;
    while (host.take_retired(gone)) {
        any = true;
        {
            std::lock_guard<std::mutex> guard(lock);
            for (auto& d : data_of) if (d == gone) d = nullptr;
        }
        auto it = std::find_if(owned.begin(), owned.end(), [&](const auto& p) { return p.get() == gone; });
        if (it == owned.end()) continue;
        if ((*it)->multi) {
            // Nothing may still be pinning it when its mapping goes.
            host.streams().flush();
            landings.erase(std::remove_if(landings.begin(), landings.end(), [&](const Landing& l) { return l.multi == (*it)->multi; }), landings.end());
        }
        owned.erase(it);
    }
    return any;
}

bool SampleLibrary::release_instruments() {
    std::vector<std::pair<uint8_t, std::string>> putting;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (unsigned n = 0; n < sample_slots; ++n)
            if (data_of[n] && data_of[n]->multi) putting.emplace_back(uint8_t(n), known.slots[n].path);
    }
    for (const auto& [slot, path] : putting) {
        (void)path;
        if (!host.offer_sample(slot, nullptr)) return false;
        cancel_landing(slot);
        std::lock_guard<std::mutex> guard(lock);
        known.slots[slot] = LoadedSample {};
        changes.fetch_add(1, std::memory_order_release);
    }
    away.insert(away.end(), putting.begin(), putting.end());
    forget_sf2_files(); // roadmap P3: no .sf2 kept mapped while the card is lent
    // The audio thread lets them go on its next block; the mappings must be gone before the card is unmounted.
    for (unsigned n = 0; n < 500; ++n) {
        reap();
        if (std::none_of(owned.begin(), owned.end(), [](const auto& p) { return bool(p->multi); })) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    restore_instruments();
    return false;
}

void SampleLibrary::restore_instruments() {
    for (const auto& [slot, path] : away) {
        Request again;
        again.slot = slot;
        std::snprintf(again.path, sizeof(again.path), "%s", path.c_str());
        requests.push(again);
    }
    away.clear();
}

} // namespace melodee
