// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/page_stack.h"

namespace melodee {
namespace {
// Pages turned through as one group: moving among them replaces the top of
// the back stack instead of growing it, so Back leaves the group.
int page_group(PageId page) {
    switch (page) {
    case PageId::Stage: case PageId::Keys: case PageId::Pads: case PageId::Mix: return 1;
    case PageId::StepEdit: case PageId::Steps: case PageId::Roll: case PageId::Motion: return 2;
    case PageId::Rack: case PageId::Sends: case PageId::Bus: case PageId::Master: case PageId::PadFx: return 3;
    case PageId::Trim: case PageId::PadPage: case PageId::PlayPage: case PageId::EnvelopePage:
    case PageId::FilterPage: case PageId::FollowPage: case PageId::CloudPage: return 4;
    case PageId::Punch: case PageId::PadPunch: return 5;
    case PageId::Arp: case PageId::Chord: return 6;
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

} // namespace melodee
