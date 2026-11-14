// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/page_stack.h"

namespace melodee {
namespace {
// Pages turned through as one group: moving among them replaces the top of
// the back stack instead of growing it, so Back leaves the group.
int page_group(PageId page) {
    switch (page) {
    case PageId::Stage: case PageId::Tracks: case PageId::Keys: case PageId::Pads: return 1;
    case PageId::StepEdit: case PageId::Patterns: case PageId::Scenes: case PageId::Roll: case PageId::Motion: return 2;
    case PageId::Rack: case PageId::Sends: case PageId::Bus: case PageId::Master: case PageId::PadFx: return 3;
    case PageId::Trim: case PageId::PadPage: case PageId::PlayPage: case PageId::EnvelopePage:
    case PageId::FilterPage: case PageId::FollowPage: case PageId::CloudPage:
    case PageId::PadMix: case PageId::PadFilterPage: case PageId::FilterEnvPage: case PageId::LayersPage: return 4;
    case PageId::Punch: return 5;
    default: return 0;
    }
}
} // namespace

void push_back_stack(UiState& ui, PageId page) {
    for (unsigned n = 0; n < ui.back_depth; ++n)
        if (ui.back_stack[n] == page) { ui.back_depth = uint8_t(n); return; }
    int group = page_group(ui.page);
    if (group && group == page_group(page)) return;
    if (ui.back_depth == UiState::back_capacity) {
        for (unsigned n = 1; n < UiState::back_capacity; ++n) ui.back_stack[n - 1] = ui.back_stack[n];
        --ui.back_depth;
    }
    ui.back_stack[ui.back_depth++] = ui.page;
}

PageId back_target(const UiState& ui) { return ui.back_depth ? ui.back_stack[ui.back_depth - 1u] : PageId::Stage; }

bool seq_editor(PageId page) { return page == PageId::Patterns || page == PageId::Scenes || page == PageId::Roll || page == PageId::Motion; }

PageId seq_editor_page(const UiState& ui) {
    static const PageId editors[] {PageId::Patterns, PageId::Roll, PageId::Motion, PageId::Scenes};
    unsigned kept = ui.seq_editor[ui.track % track_count];
    if (kept >= 1 && kept <= 4) return editors[kept - 1];
    return PageId::Patterns;
}

void remember_editor(UiState& ui, PageId page) {
    if (!seq_editor(page)) return;
    ui.seq_editor[ui.track % track_count] = uint8_t(page == PageId::Patterns ? 1 : page == PageId::Roll ? 2 : page == PageId::Motion ? 3 : 4);
}

PageId seq_tap_page(const UiState& ui) {
    if (seq_editor(ui.page)) return PageId::Patterns; // home (hubs plan R-a)
    return seq_editor_page(ui);                       // Step Edit, Song or any other page: the hub's page last open
}

} // namespace melodee
