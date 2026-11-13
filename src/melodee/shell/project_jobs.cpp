// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The Project page's jobs from the MPC's project page (parity phase 10):
// New project asks first (B3 DO IT, B1 CANCEL), and Load Kit takes another
// project's kit (pads, samples, their effects; no patterns) onto the
// selected track as one undo; a bank file loads into one bank of it.
#include "melodee/shell/ask.h"
#include "melodee/shell/card_access.h"
#include "melodee/shell/context.h"
#include "melodee/shell/project_kits.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/track_copy.h"
#include <memory>

namespace melodee {

void Context::ask_new_project() {
    ask(*this, UiState::Ask::NewProject, !dirty ? 0 : project_path.empty() ? 2 : 1);
}

void Context::start_new_project() {
    if (dirty && !project_path.empty()) save_project();
    new_project();
    open(PageId::Stage);
    notice("New project");
}

void Context::load_kit_file(const std::string& path, const std::string& name) {
    if (!store) return;
    kit_from = path;
    store->load(path); // lands in load_kit() instead of switching (shell/lights.cpp)
    notice("Loading kit from " + name);
}

void Context::load_kit(const Project& from, const std::string& name) {
    int k = kit_track(from);
    if (k < 0) { notice("No kit in " + name); return; }
    if (ui.recording) { notice("Load Kit: not in a take"); return; }
    std::string what = "Load kit " + name;
    Edit edit = begin_edit(what.c_str(), ui.track, -1, false);
    edit.motion_before.reset(); // the kit brings no motion
    edit.track_before = std::make_unique<TrackState>(track_state(*this, ui.track));
    auto next = std::make_unique<TrackState>(*edit.track_before);
    auto result = import_kit(from, unsigned(k), next->track, [&](const SampleRef& ref) { return sample_slot(ref.path, ref.name); });
    restore_track(*this, ui.track, *next);
    edit.track_after = std::move(next);
    edits.push(std::move(edit));
    notice(result.missing ? what + ": " + std::to_string(result.missing) + " pads without a slot" : what);
    if (ui.page == PageId::Project) open(PageId::Stage); // the kit's sound (Sample held shows its pads)
}

// A kit bank file (Push on one in the Samples browser): read on the service
// thread, then into the bank in view of the selected kit (files_landed()).
void Context::load_bank_file(const std::string& path, const std::string& name) {
    if (!library) return;
    if (track().engine != EngineKind::Sampler) { notice("Load Bank: kit tracks"); return; }
    bank_from = path;
    library->read_bank(path);
    notice("Loading " + name);
}

void Context::load_bank(const BankFile& bank, const std::string& name) {
    auto& t = track();
    if (t.engine != EngineKind::Sampler) { notice("Load Bank: kit tracks"); return; }
    if (ui.recording) { notice("Load Bank: not in a take"); return; }
    unsigned into = t.bank % sampler_banks;
    std::string bank_name = std::string("Bank ") + char('A' + into), what = "Load " + bank_name; // the notice holds 31 characters
    Edit edit = begin_edit(what.c_str(), ui.track, -1, false);
    edit.motion_before.reset(); // a bank brings no motion
    edit.track_before = std::make_unique<TrackState>(track_state(*this, ui.track));
    auto next = std::make_unique<TrackState>(*edit.track_before);
    auto result = import_bank(bank, into, next->track, [&](const SampleRef& ref) { return sample_slot(ref.path, ref.name); });
    restore_track(*this, ui.track, *next);
    edit.track_after = std::move(next);
    edits.push(std::move(edit));
    dirty = true;
    notice(result.missing ? bank_name + ": " + std::to_string(result.missing) + " pads had no slot" : bank_name + ": " + name);
}

// SD card access (parity phase 10b): the service thread lends the card; the
// page shows how it goes and owns the controls until it is back.
void Context::card_access() {
    if (!card || card_away(ui.card)) return;
    ui.sheet_open = false; ui.move_open = false;
    card->request.store(uint8_t(CardRequest::Lend));
    open(PageId::CardAccess);
}

} // namespace melodee
