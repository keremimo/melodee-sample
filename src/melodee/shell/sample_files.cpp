// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Sample files (parity phase 5), on the MIDI thread: what the MPC makes
// from a pad's audio and what its browser loads.
//
// - Trim Sample (Shift + Pad 13): the last pad's region becomes a file of
//   its own ("NAME-Trimmed"); the pad then plays all of it. One undo, which
//   puts the pad back on the old file and region.
// - Extract (Shift + B1 on Chop): the slice in hand as a file ("NAME-Slice
//   n"), onto the next empty pad. One undo.
// - Save Sample (Shift + Sample Select on a kit): the region as
//   Samples/<name>.wav, named on the Name sheet; the pad is left as it is.
// - Loading a file from the browser onto the last pad keeps the pad's
//   settings (volume, tune, envelopes, filter ...; its region, loop point,
//   Normalize and # Beats start again, as they belong to the old file);
//   onto an empty pad (Shift + pad: the next one) it starts at the defaults.
//   One undo.
//
// The service thread writes the files (SampleLibrary::write()) into a slot
// held for them; files_landed() puts them on their pads once they are in.
// Melodee writes only to the card: Trim and Extract go to Samples/Edits;
// without a card they stay in memory for the session.
#include "melodee/shell/context.h"
#include "melodee/shell/new_track.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/sample_lists.h"
#include "melodee/shell/browse_trim.h"
#include "melodee/shell/history.h"
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <algorithm>

namespace melodee {
namespace {
constexpr const char* writing = "(writing)"; // a slot's path while its file is made

const char* edits_folder = "Edits";

// A free sample slot held for a file being made, -1 when every slot is in use.
int hold_slot(Project& project, const std::string& name) {
    for (unsigned n = 0; n < sample_slots; ++n) {
        if (project.samples[n].used()) continue;
        auto& ref = project.samples.edit(n);
        std::snprintf(ref.path, sizeof(ref.path), "%s", writing);
        std::snprintf(ref.name, sizeof(ref.name), "%s", name.c_str());
        ref.frames = 0;
        if (project.chops[n].used()) project.chops.edit(n) = SampleChop {}; // roadmap P3: no chop of what the slot held before
        return int(n);
    }
    return -1;
}

SampleJob job(SampleJob::Kind kind, unsigned source, int target, uint32_t from, uint32_t to, const std::string& name, const char* folder) {
    SampleJob j;
    j.kind = kind; j.source = uint16_t(source); j.target = uint16_t(std::max(target, 0));
    j.from = from; j.to = to;
    std::snprintf(j.name, sizeof(j.name), "%s", name.c_str());
    std::snprintf(j.folder, sizeof(j.folder), "%s", folder);
    return j;
}

// The pad's name with a word after it, fitting a slot's name.
std::string derived(const SampleRef& ref, const std::string& tail) {
    std::string base = ref.name[0] ? ref.name : "SAMPLE";
    size_t room = sizeof(ref.name) - 1 > tail.size() ? sizeof(ref.name) - 1 - tail.size() : 0;
    return base.substr(0, room) + tail;
}
} // namespace

int Context::hold_file(const std::string& name, unsigned t, int pad, uint8_t land) {
    unsigned free = 0;
    while (free < pending_files.size() && pending_files[free].slot >= 0) ++free;
    int slot = free < pending_files.size() ? hold_slot(project, name) : -1;
    if (slot < 0) return -1;
    pending_files[free] = {int16_t(slot), uint8_t(t % track_count), int16_t(pad), false, PadSound {}, land};
    return slot;
}

void Context::drop_file(int slot) {
    if (slot < 0 || slot >= int(sample_slots)) return;
    project.samples.edit(size_t(slot)) = SampleRef {};
    for (auto& p : pending_files) if (p.slot == slot) p = PendingFile {};
}

int Context::next_empty_pad(unsigned after) const {
    const auto& t = project.tracks[ui.track % track_count];
    for (unsigned k = 1; k <= sampler_pads; ++k) {
        unsigned n = (after + k) % sampler_pads;
        if (!t.pads[n].used()) return int(n);
    }
    return -1;
}

void Context::trim_sample() {
    auto& t = track();
    unsigned kit = t.last_pad % sampler_pads;
    const auto& pad = t.pads[kit];
    if (t.engine != EngineKind::Sampler || !pad.used()) { notice("Trim Sample: no sample"); return; }
    if (refuse_instrument("Trim Sample")) return;
    if (!library) return;
    const auto& ref = project.samples[pad.sample % sample_slots];
    uint32_t frames = ref.frames, end = pad.end && pad.end < frames ? pad.end : frames;
    if (!frames) { notice("Trim Sample: still loading"); return; }
    if (pad.start == 0 && end >= frames) { notice("Trim Sample: nothing to cut"); return; }
    unsigned free = 0;
    while (free < pending_files.size() && pending_files[free].slot >= 0) ++free;
    std::string name = derived(ref, "-Trimmed");
    int slot = free < pending_files.size() ? hold_slot(project, name) : -1;
    if (slot < 0) { notice(free < pending_files.size() ? "Trim Sample: samples full" : "Trim Sample: busy"); return; }
    if (!library->write(job(SampleJob::Kind::Derive, pad.sample, slot, pad.start, end, name, edits_folder))) {
        project.samples.edit(size_t(slot)) = SampleRef {};
        notice("Trim Sample: busy");
        return;
    }
    pending_files[free] = {int16_t(slot), ui.track, int16_t(kit), true, pad};
    notice(pad_text("Trimming", kit));
}

void Context::extract_slice() {
    auto& t = track();
    const auto& sl = ui.slices;
    uint32_t frames = ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].frames : 0;
    if (t.engine != EngineKind::Sampler || !frames || ui.chop_slice >= sl.count) { notice("Extract: no slice"); return; }
    if (!library) return;
    unsigned free = 0;
    while (free < pending_files.size() && pending_files[free].slot >= 0) ++free;
    const auto& ref = project.samples[ui.chop_sample];
    std::string name = derived(ref, "-Slice " + std::to_string(ui.chop_slice + 1));
    int slot = free < pending_files.size() ? hold_slot(project, name) : -1;
    if (slot < 0) { notice(free < pending_files.size() ? "Extract: samples full" : "Extract: busy"); return; }
    SampleJob slice = job(SampleJob::Kind::Derive, ui.chop_sample, slot, sl.start[ui.chop_slice], sl.end(ui.chop_slice, frames), name, edits_folder);
    slice.edges = true; // faded where it was cut, as the pads play it
    if (!library->write(slice)) {
        project.samples.edit(size_t(slot)) = SampleRef {};
        notice("Extract: busy");
        return;
    }
    pending_files[free] = {int16_t(slot), ui.track, int16_t(t.last_pad % sampler_pads), false, PadSound {}};
    notice("Extracting slice " + std::to_string(ui.chop_slice + 1));
}

void Context::save_sample_as(const std::string& name) {
    const auto& t = track();
    const auto& pad = t.pads[t.last_pad % sampler_pads];
    if (t.engine != EngineKind::Sampler || !pad.used() || !library) { notice("Save Sample: no sample"); return; }
    if (library->root().empty()) { notice("Save Sample: no card"); return; }
    if (!library->write(job(SampleJob::Kind::Save, pad.sample, -1, pad.start, pad.end, name, ""))) { notice("Save Sample: busy"); return; }
    notice("Saving " + name);
}

// A file onto layer ui.layer_load (2-4) of the last pad (roadmap P3): its sample and region new, its tune, level
// and velocities kept when the layer had a sample; one undo; then back to the page LOAD came from.
void Context::load_layer(const std::string& path, const std::string& name, uint32_t start, uint32_t end) {
    auto& t = track();
    unsigned kit = t.last_pad % sampler_pads, n = unsigned(std::max<int>(ui.layer_load, 1)) % pad_layers;
    if (!n) n = 1;
    ui.layer_load = -1;
    SampleRef asked;
    std::snprintf(asked.path, sizeof(asked.path), "%s", path.c_str());
    if (asked.instrument()) { notice("Layers take samples"); return; }
    if (!t.pads[kit].used()) { notice("Layer 1 first"); return; }
    int slot = sample_slot(path, name);
    if (slot < 0) { notice("Samples full"); return; }
    Edit edit;
    edit.track = ui.track; edit.pad = int16_t(kit);
    edit.pad_before = pad_state(t, kit);
    edit.pad_after = edit.pad_before;
    auto& layer = edit.pad_after.sound.layers[n - 1];
    if (!layer.used()) layer = PadLayer {};
    layer.sample = uint16_t(slot); layer.start = start; layer.end = end; // the trim sheet's, else the whole file
    char label[4]; pad_label(kit, label);
    edit.what = "Load " + std::string(label) + " L" + std::to_string(n + 1);
    restore_pad(ui.track, kit, edit.pad_after);
    edits.push(std::move(edit));
    dirty = true;
    notice("Loaded " + std::string(label) + " layer " + std::to_string(n + 1));
    back();
}

void Context::load_file(const std::string& path, const std::string& name, bool next_empty, uint32_t start, uint32_t end) {
    auto& t = track();
    if (!t.sampled()) return;
    const bool keyed = t.engine == EngineKind::Keygroup; // one sound, pad 1: no layers of its own, no next pad
    if (library) { stop_preview(); library->preview(""); } // the pad plays the file now, not its preview too
    if (keyed && ui.layer_load > 0) { ui.layer_load = -1; notice("Keygroup: one sound"); return; }
    if (ui.layer_load > 0 && !next_empty) { load_layer(path, name, start, end); return; }
    int kit = keyed ? 0 : next_empty ? next_empty_pad(t.last_pad) : int(t.last_pad % sampler_pads);
    if (kit < 0) { notice("No empty pad"); return; }
    int slot = sample_slot(path, name);
    if (slot < 0) { notice("Samples full"); return; }
    Edit edit;
    edit.track = ui.track; edit.pad = int16_t(kit);
    edit.pad_before = pad_state(t, unsigned(kit));
    edit.pad_after = edit.pad_before;
    auto& sound = edit.pad_after.sound;
    bool keep = sound.used() && !next_empty;
    if (keep) { // the pad's settings stay; what belonged to the old file starts again
        sound.start = sound.end = sound.loop_start = 0;
        sound.loop_lock = true;
        sound.gain = 0; sound.own_tempo = 0; sound.beats = 0;
    } else sound = PadSound {};
    if (sound.sample != uint16_t(slot)) edit.pad_after.trims.clear(); // round 3: another file's zones are not these
    sound.sample = uint16_t(slot);
    if (start || end) { sound.start = start; sound.end = end; sound.loop_start = start; } // the trim sheet's (shell/browse_trim.h)
    if (project.samples[size_t(slot)].instrument()) { // played on keys: held while held, no region of its own
        sound.play = PadPlay::Gate; sound.decay = 100; sound.sustain = 100;
        if (!keep) sound.mono = false; // a fresh instrument pad is Poly (chords on Keys); a kept pad keeps its Mono
        sound.loop = false; sound.warp = warp_off; sound.beats = 0;
        const PadSound fresh; // its zones are its layers: the pad's own layers go
        sound.layers = fresh.layers; sound.low_velocity = fresh.low_velocity; sound.high_velocity = fresh.high_velocity;
        sound.layer_play = fresh.layer_play;
        landing_pads.push_back({ui.track, uint8_t(kit), uint16_t(slot)});
    }
    if (keyed && !keep) { sound.mono = false; sound.play = PadPlay::Gate; } // a keygroup's WAV across the keys: held, chords
    edit.what = keyed ? "Load " + name : pad_text("Load", unsigned(kit));
    restore_pad(ui.track, unsigned(kit), edit.pad_after);
    t.last_pad = uint8_t(kit);
    edits.push(std::move(edit));
    dirty = true;
    const std::string loaded = keyed ? "Loaded " + name : pad_text("Loaded", unsigned(kit));
    notice(keep ? loaded + ", settings kept" : loaded);
    new_track_landed(*this); // a track SAMPLES made: back to the page it was picked on
}

// An instrument read: the envelope most of its regions have goes on its pad,
// with its velocity tracking and an open filter, once, as part of the Load.
void Context::land_instruments() {
    if (!library || landing_pads.empty()) return;
    for (size_t n = 0; n < landing_pads.size();) {
        const auto l = landing_pads[n];
        auto& tr = project.tracks[l.track % track_count];
        auto& pad = tr.pads[l.pad % sampler_pads];
        const auto& ref = project.samples[l.slot % sample_slots];
        if (pad.sample != l.slot || !ref.instrument()) { landing_pads.erase(landing_pads.begin() + long(n)); continue; }
        const auto loaded = library->slot(l.slot);
        if (loaded.path != ref.path || (!loaded.ready && loaded.error.empty())) { ++n; continue; } // not read yet
        landing_pads.erase(landing_pads.begin() + long(n));
        if (!loaded.ready) { notice(loaded.error.c_str()); continue; }
        auto apply = [&](PadSound& sound) {
            if (loaded.stages[0] >= 0) sound.attack = uint8_t(loaded.stages[0]);
            if (loaded.stages[1] >= 0) sound.decay = uint8_t(loaded.stages[1]);
            if (loaded.stages[2] >= 0) sound.sustain = uint8_t(loaded.stages[2]);
            if (loaded.stages[3] >= 0) sound.release = uint8_t(loaded.stages[3]);
            sound.velocity_sense = uint8_t(loaded.velocity);
            sound.filter = PadFilter::Off; sound.cutoff = 100;
        };
        apply(pad);
        send_pad(l.track, l.pad);
        if (auto* edit = edits.latest())
            if (edit->track == l.track && edit->pad == int16_t(l.pad) && edit->pad_after.sound.sample == l.slot) apply(edit->pad_after.sound);
    }
}

// Made files that have arrived go onto their pads; what the library has to
// say about a file (saved, failed) shows over the header.
void Context::files_landed() {
    if (!library) return;
    land_instruments();
    SampleLibrary::Done done;
    while (library->done(done)) {
        notice(done.text);
        for (auto& p : pending_files) // a file that could not be made: its slot is free again
            if (done.failed >= 0 && p.slot == done.failed) {
                if (p.land == 2) { ui.rec_slot = ui.rec_pad = -1; ui.rec_chops = 0; } // the take is gone
                project.samples.edit(size_t(p.slot)) = SampleRef {}; p = PendingFile {};
            }
    }
    SampleLibrary::BankBytes bank_bytes;
    while (library->take_bank(bank_bytes)) { // Load Bank: the file asked for last, onto the bank in view
        if (bank_bytes.path != bank_from) continue;
        bank_from.clear();
        auto name = std::filesystem::path(bank_bytes.path).stem().string();
        BankFile bank;
        if (!bank_bytes.error.empty()) notice("Load Bank: " + bank_bytes.error);
        else if (!melodee::load_bank(bank_bytes.bytes, bank)) notice("Load Bank: not a bank file");
        else load_bank(bank, name);
    }
    stems_landed(); // a sample's four stems together (shell/stems_jobs.cpp)
    for (auto& p : pending_files) {
        if (p.slot < 0 || p.land >= 3) continue; // 3 and up: stems, landed above
        auto loaded = library->slot(unsigned(p.slot));
        if (!loaded.ready) continue; // still being made
        if (p.land == 2 && loaded.streamed && !loaded.shaped) continue; // a long take's cuts land on its hits, once found
        auto& ref = project.samples.edit(size_t(p.slot));
        PendingFile was = p;
        p = PendingFile {};
        std::snprintf(ref.path, sizeof(ref.path), "%s", loaded.path.empty() ? ("(memory) " + loaded.name).c_str() : loaded.path.c_str());
        std::snprintf(ref.name, sizeof(ref.name), "%s", loaded.name.c_str());
        ref.frames = loaded.frames;
        ref.root = loaded.root;
        dirty = true;
        auto& t = project.tracks[was.track % track_count];
        if (was.land == 2) { take_landed(was.track, unsigned(was.pad) % sampler_pads, unsigned(was.slot), loaded); continue; }
        Edit edit;
        edit.track = was.track;
        if (was.land == 1) { // Audio Recall, Resample: onto the pad chosen, at the defaults
            unsigned kit = unsigned(was.pad) % sampler_pads;
            edit.pad = int16_t(kit);
            edit.pad_before = pad_state(t, kit);
            edit.pad_after = edit.pad_before;
            edit.pad_after.sound = PadSound {};
            edit.pad_after.sound.sample = uint16_t(was.slot);
            edit.what = pad_text(loaded.name.rfind("RESAMPLE", 0) == 0 ? "Resample to" : loaded.name.find(" Loop") != std::string::npos ? "Commit to"
                                 : "Audio Recall to", kit);
            t.last_pad = uint8_t(kit);
        } else if (was.trim) { // the pad, if it still plays what was trimmed, onto the whole new file
            unsigned kit = unsigned(was.pad) % sampler_pads;
            const auto& now_sound = t.pads[kit];
            if (now_sound.sample != was.sound.sample || now_sound.start != was.sound.start || now_sound.end != was.sound.end) {
                notice(pad_text("Trimmed file for", kit));
                continue;
            }
            edit.pad = int16_t(kit);
            edit.pad_before = pad_state(t, kit);
            edit.pad_after = edit.pad_before;
            auto& s = edit.pad_after.sound;
            uint32_t cut = s.start;
            s.sample = uint16_t(was.slot);
            s.loop_start = s.loop_start > cut ? std::min(s.loop_start - cut, loaded.frames - 1) : 0;
            s.start = 0; s.end = 0;
            edit.what = pad_text("Trim Sample", kit);
        } else { // Extract: onto the next empty pad after the one chopped
            int kit = next_empty_pad(unsigned(was.pad));
            if (was.track != ui.track || kit < 0) { notice("Extract: no empty pad"); continue; }
            edit.pad = int16_t(kit);
            edit.pad_before = pad_state(t, unsigned(kit));
            edit.pad_after = edit.pad_before;
            edit.pad_after.sound = PadSound {};
            edit.pad_after.sound.sample = uint16_t(was.slot);
            edit.what = pad_text("Extract to", unsigned(kit));
        }
        restore_pad(was.track, unsigned(edit.pad), edit.pad_after);
        notice(was.trim ? pad_text("Trimmed", unsigned(edit.pad)) : edit.what);
        if (was.land == 1) send_pad(was.track, unsigned(edit.pad));
        edits.push(std::move(edit));
    }
}

// Sample Record's take: onto its pad, and the pads cut during it each from
// their cut (pulled onto the strongest hit up to 50 ms before the tap, then
// with Zero Snap onto a zero crossing) to the next. One undo, which puts every pad it filled back as it was.
void Context::take_landed(unsigned t, unsigned kit, unsigned slot, const LoadedSample& loaded) {
    close_gesture(); // a pot's turns came before the take
    auto& track = project.tracks[t % track_count];
    std::array<uint32_t, UiState::rec_chop_capacity> at {};
    unsigned cuts = ui.rec_track == t ? ui.rec_chops : 0;
    for (unsigned n = 0; n < cuts; ++n) {
        at[n] = std::min(snap_to_hit(ui.rec_chop_at[n], loaded.hits, loaded.hit_count), loaded.frames ? loaded.frames - 1 : 0);
        if (ui.settings.zero_snap && library) at[n] = library->zero_crossing(slot, at[n]); // Chop's Zero Snap
    }
    Edit edit;
    edit.track = uint8_t(t % track_count);
    edit.what = pad_text("Take on", kit);
    auto place = [&](unsigned pad, uint32_t start, uint32_t end) {
        pad %= sampler_pads;
        edit.pads_before.emplace_back(uint8_t(pad), pad_state(track, pad));
        auto& sound = track.pads[pad];
        sound = PadSound {};
        sound.sample = uint16_t(slot); sound.start = start; sound.end = end;
        send_pad(t, pad);
        edit.pads_after.emplace_back(uint8_t(pad), pad_state(track, pad));
    };
    // Cuts in the order they fall; a cut pulled onto an earlier one's place is dropped.
    std::array<unsigned, UiState::rec_chop_capacity> order {};
    for (unsigned n = 0; n < cuts; ++n) order[n] = n;
    std::sort(order.begin(), order.begin() + cuts, [&](unsigned a, unsigned b) { return at[a] < at[b]; });
    uint32_t first_cut = cuts ? at[order[0]] : 0;
    place(kit, 0, first_cut > 0 ? first_cut : 0);
    unsigned placed = 0;
    for (unsigned k = 0; k < cuts; ++k) {
        unsigned n = order[k];
        uint32_t end = k + 1 < cuts ? at[order[k + 1]] : 0;
        if (at[n] == 0 || (end && end <= at[n])) continue;
        place(ui.rec_chop_pad[n], at[n], end);
        ++placed;
    }
    track.last_pad = uint8_t(kit);
    if (ui.rec_slot == int(slot)) { ui.rec_slot = ui.rec_pad = -1; ui.rec_chops = 0; }
    edits.push(std::move(edit));
    dirty = true;
    notice(placed ? pad_text("Take on", kit) + ", " + std::to_string(placed) + (placed == 1 ? " cut" : " cuts") : pad_text("Take on", kit));
}

void Context::preview_cursor() {
    if (!library) return;
    auto files = library->files();
    const SampleFile* file = ui.files_cursor < files.size() ? &files[ui.files_cursor] : nullptr;
    std::string path = ui.settings.preview && file &&!file->folder && !file->bank && !file->midi ? file->path : std::string(); // a MIDI file has no sound to preview
    stop_preview(); // what played stops as the cursor moves on
    library->preview(path);
}

void Context::stop_preview() {
    if (previewing < 0) return;
    Command stop {Command::Type::Audition, uint8_t(previewing)};
    stop.pad.sample = no_sample;
    send(stop);
    previewing = -1;
}

namespace {
// What the trim sheet takes: a file of audio of its own, not a folder, a list, a kit, a bank, an instrument, an
// expansion, a MIDI file or a preset inside an .sf2.
bool trimmable(const SampleFile& f) {
    return !f.folder && !f.list && !f.bank && !f.kit && !f.instrument && !f.expansion && !f.midi && !f.presets && !part_of_file(f.path);
}
} // namespace

bool Context::browse_open(unsigned focus) {
    if (!library || ui.browse.up() || ui.page != PageId::Samples || ui.sheet_open || !track().sampled()) return false;
    const auto files = library->files();
    if (ui.files_cursor >= files.size() || !trimmable(files[ui.files_cursor])) return false;
    const auto& file = files[ui.files_cursor];
    bool had = false; // already in the project: its slot is kept when the sheet goes down
    for (const auto& ref : project.samples) had |= ref.used() && file.path == ref.path;
    const int slot = sample_slot(file.path, file.name);
    if (slot < 0) { notice("Samples full"); return false; }
    if (project.samples[size_t(slot)].instrument()) return false;
    ui.browse = BrowseTrim {};
    ui.browse.slot = int16_t(slot); ui.browse.fresh = !had; ui.browse.focus = uint8_t(focus % 2);
    ui.browse.opened_at = now ? now : 1;
    browse_path = file.path;
    browse_cursor = ui.files_cursor; browse_generation = library->generation();
    return true;
}

void Context::browse_close() {
    if (!ui.browse.up()) return;
    const unsigned slot = unsigned(ui.browse.slot) % sample_slots;
    const bool fresh = ui.browse.fresh;
    ui.browse = BrowseTrim {};
    browse_path.clear();
    stop_preview(); // what the sheet played
    if (!fresh) return;
    auto used = samples_in_use(project, edits); // the file went onto a pad, or an undo could bring it back: kept
    for (const auto& f : pending_files) if (f.slot >= 0) used[size_t(f.slot) % sample_slots] = true;
    if (ui.chop_sample < sample_slots) used[ui.chop_sample] = true;
    if (used[slot] || !project.samples[slot].used()) return;
    project.samples.edit(slot) = SampleRef {};
    if (project.chops[slot].used()) project.chops.edit(slot) = SampleChop {};
    if (library) library->load(slot, "");
}

void Context::browse_erase(int handle) {
    auto& b = ui.browse;
    if (!b.up()) return;
    const FrameView was = browse_view(project, b);
    if (handle != 1) b.start = 0;
    if (handle != 0) b.end = 0;
    if (handle >= 0) b.focus = uint8_t(handle);
    browse_moved(b, project, was);
    b.heard_at = std::max<uint32_t>(1, now + chop_heard_ms);
}

// MIDI thread, from the lights' pass. The sheet goes down once its file is no longer under the cursor (the wheel,
// Minus / Plus, K1, another folder or drive), the page or the track was left, or LOAD KIT opened over it.
void Context::browse_tick(uint32_t ms) {
    auto& b = ui.browse;
    if (!b.up()) return;
    bool here = library && ui.page == PageId::Samples && track().sampled() && !ui.sheet_open;
    if (here && (ui.files_cursor != browse_cursor || library->generation() != browse_generation)) {
        const auto files = library->files();
        here = ui.files_cursor < files.size() && files[ui.files_cursor].path == browse_path;
        browse_cursor = ui.files_cursor; browse_generation = library->generation();
    }
    if (!here) { browse_close(); return; }
    if (!b.heard_at || int32_t(ms - b.heard_at) < 0) return;
    b.heard_at = 0;
    const auto h = browse_handles(project, b);
    if (!ui.settings.preview || !h.frames) return; // B3 off: nothing heard
    const auto [from, to] = browse_heard(h, b.focus);
    Command a {Command::Type::Audition, ui.track, 0, 0, int16_t(ui.audition_velocity)};
    a.pad.sample = uint16_t(b.slot); a.pad.start = from; a.pad.end = to;
    send(a);
    previewing = ui.track;
}

void Context::browser_tick() {
    if (!library) return;
    bool here = ui.page == PageId::Samples && track().sampled(); // a kit's browser or a keygroup's
    auto folder = library->folder();
    if (folder != files_folder_seen) { // a new folder: the cursor on the one it came out of, else the top
        files_folder_seen = folder;
        ui.files_cursor = uint16_t(files_row_wanted >= 0 ? files_row_wanted : std::max(library->came_from(), 0)); // B2's drive: its row
        files_row_wanted = -1;
        ui.files_drive = uint8_t(library->drive());
        if (here && ui.settings.preview) preview_cursor();
    }
    if (here && (ui.files_cursor != audio_cursor || library->generation() != audio_generation)) { // K2 / K3's cards
        audio_cursor = ui.files_cursor; audio_generation = library->generation();
        const auto files = library->files();
        ui.files_audio = ui.files_cursor < files.size() && trimmable(files[ui.files_cursor]);
    }
    if (!here && previewing >= 0) { // away from the browser: what it played stops (B3 stays as it was)
        stop_preview();
        library->preview("");
    }
    unsigned arrived = library->previews();
    if (arrived == previews_seen) return;
    previews_seen = arrived;
    auto files = library->files();
    if (!here || !ui.settings.preview || ui.files_cursor >= files.size() || files[ui.files_cursor].path != library->previewed()) return;
    Command a {Command::Type::Audition, ui.track, 0, 0, int16_t(ui.audition_velocity)};
    a.pad.sample = uint16_t(preview_slot); a.pad.start = 0; a.pad.end = 0;
    send(a);
    previewing = ui.track;
}

} // namespace melodee
