// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// Back (Shift + Push) walks a real stack of pages (UiState::back_stack).
namespace melodee {

// Opening `page` from ui.page: a page already on the stack is gone back to
// (it and what came after leave the stack); a sibling in ui.page's group
// (Stage / Keys / Pads / Mix, Patterns / Roll / Motion, the effect chains, the
// sampler's pages, Arp / Chord) takes its place;
// anything else goes on top, the oldest dropping when full. The caller then
// sets ui.page.
void push_back_stack(UiState& ui, PageId page);
// Where Back goes from here: the top of the stack, or Stage when it is empty.
PageId back_target(const UiState& ui);
// The SEQUENCE hub (shell/seq_layer.h): Patterns, Roll and Motion (one remembered per track), and Step Edit
// (Shift + Seq) over them.
bool seq_editor(PageId page); // Patterns, Roll or Motion
// The selected track's hub page: the one last open on it, else Patterns.
PageId seq_editor_page(const UiState& ui);
// An editor opened: remembered for the selected track.
void remember_editor(UiState& ui, PageId page);
// A tap of Seq from ui.page (the SEQUENCE hub, docs/HUBS_PLAN.md): the hub's page last open (Patterns, Roll or
// Motion) from anywhere; inside the hub home, Patterns (B1's page); on Step Edit back to the page it sits over.
PageId seq_tap_page(const UiState& ui);

} // namespace melodee
