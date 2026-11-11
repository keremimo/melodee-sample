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
// Seq's three levels (shell/seq_layer.h): Patterns, an editor (Steps, Roll,
// Motion: one remembered per track), and Step Edit under it.
bool seq_editor(PageId page); // Steps, Roll or Motion
// The selected track's editor: the one last open on it, else Steps on a kit (the sampler or
// the 909) and Roll on a synth or MIDI track.
PageId seq_editor_page(const UiState& ui, bool kit);
// An editor opened: remembered for the selected track.
void remember_editor(UiState& ui, PageId page);
// A tap of Seq from ui.page: Patterns from anywhere; on Patterns the track's editor; on an
// editor back to Patterns; on Step Edit back to the editor it sits under.
PageId seq_tap_page(const UiState& ui, bool kit);

} // namespace melodee
