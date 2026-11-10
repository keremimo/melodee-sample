// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The Project page's jobs from the MPC's project page (parity phase 10):
// New project asks first (B3 DO IT, B1 CANCEL), the demos made in code
// (shell/demo_projects.h) open unsaved, and Load Kit takes another
// project's kit (pads, samples, their effects; no patterns) onto the
// selected track as one undo.
#include "melodee/shell/ask.h"
#include "melodee/shell/context.h"
#include "melodee/shell/demo_projects.h"
#include "melodee/shell/project_store.h"
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

void Context::open_demo(unsigned n) {
    auto fresh = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    make_demo(n, *fresh, *bank);
    switch_to(*fresh, *bank, {});
    notice(std::string("Demo: ") + demo_name(n));
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
    if (ui.page == PageId::Project) open(PageId::Pads);
}

} // namespace melodee
