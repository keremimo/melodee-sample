// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/ask.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/context.h"
#include "melodee/shell/step_edit.h"

namespace melodee {

void ask(Context& c, UiState::Ask kind, int target) {
    c.ui.ask = kind;
    c.ui.ask_target = target;
}

bool ask_button(Context& c, SurfaceButton button) {
    using B = SurfaceButton;
    auto& ui = c.ui;
    if (ui.ask == UiState::Ask::None) return false;
    if (button == B::Play || button == B::Stop) return false; // the transport still answers
    if (button == B::TopLeft) { ui.ask = UiState::Ask::None; return true; } // Cancel
    if (button != B::TopRight) return true;                                  // the rest wait
    auto kind = ui.ask;
    ui.ask = UiState::Ask::None;                                              // Do it
    if (kind == UiState::Ask::EraseKeyEvents) erase_key_events(c, int(ui.ask_target));
    else if (kind == UiState::Ask::EraseLane) c.clear_lane(int(ui.ask_target));
    else if (kind == UiState::Ask::ResetSettings) c.reset_settings();
    else if (kind == UiState::Ask::NewProject) c.start_new_project();
    return true;
}

std::string ask_title(const Project& project, const UiState& ui) {
    const auto& t = project.tracks[ui.track % track_count];
    switch (ui.ask) {
    case UiState::Ask::EraseKeyEvents: return "Erase " + key_label(t, int(ui.ask_target)) + " events?";
    case UiState::Ask::EraseLane: return "Erase " + lane_name(t, int(ui.ask_target)) + " motion?";
    case UiState::Ask::ResetSettings: return "Reset factory settings?";
    case UiState::Ask::NewProject: return "New project?";
    case UiState::Ask::TakeCardBack: return "Take the card back anyway?";
    default: return {};
    }
}

std::string ask_detail(const Project& project, const UiState& ui, const Pattern& pattern) {
    const auto& t = project.tracks[ui.track % track_count];
    std::string where = " in P" + std::to_string(t.pattern % pattern_count + 1);
    unsigned count = 0;
    if (ui.ask == UiState::Ask::EraseKeyEvents) {
        for (const auto& e : pattern.events) count += e.key == ui.ask_target;
        return std::to_string(count) + (count == 1 ? " event" : " events") + where + " go.";
    }
    if (ui.ask == UiState::Ask::EraseLane) {
        const auto& m = project.motion;
        for (unsigned n = 0; n < m.count; ++n)
            count += m.events[n].same_lane(uint8_t(ui.track), t.pattern, uint8_t(lane_param(int(ui.ask_target))), uint8_t(lane_pad(int(ui.ask_target))));
        return std::to_string(count) + (count == 1 ? " move" : " moves") + where + " go.";
    }
    if (ui.ask == UiState::Ask::ResetSettings) return "Every setting as Melodee comes; projects stay.";
    if (ui.ask == UiState::Ask::TakeCardBack) return "What the computer was writing may be lost.";
    if (ui.ask == UiState::Ask::NewProject)
        return ui.ask_target == 1 ? "Changes are saved first." : ui.ask_target == 2 ? "Unsaved changes are lost." : "The open project closes.";
    return {};
}

} // namespace melodee
