// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// Back (Shift + Push) walks a real stack of pages (UiState::back_stack).
namespace melodee {

// Opening `page` from ui.page: a page already on the stack is gone back to
// (it and what came after leave the stack); a sibling in ui.page's group
// (Stage / Keys / Pads / Mix, Steps / Roll / Motion, the effect chains, the
// sampler's pages, Punch / Pad Punch, Arp / Chord) takes its place;
// anything else goes on top, the oldest dropping when full. The caller then
// sets ui.page.
void push_back_stack(UiState& ui, PageId page);
// Where Back goes from here: the top of the stack, or Stage when it is empty.
PageId back_target(const UiState& ui);

} // namespace melodee
