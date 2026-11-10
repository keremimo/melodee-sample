// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// Shift + Pad 1-16: the jobs printed under the pads, as Akai's guide names
// them (akaipro.com/guides/mpc-sample/features.htm). One table for the
// dispatch (shift_pads.cpp), the Shift overlay and the pad lights, so the
// three always agree. Pages give way: Shift + pad is the printed job on
// every page (Patterns launches a column with B3, Punch locks with Push).
namespace melodee {

struct ShiftPadJob {
    enum class Kind : uint8_t {
        Open,   // opens a page (Song, Project)
        Action, // does it once (Double Seq)
        Toggle, // on or off (Full Level, Count-In, Rec Quantize)
        Later   // printed, not built yet: a notice says which phase brings it
    };
    const char* name;  // as printed, in title case
    Kind kind;
    uint8_t phase;     // MELODEE_PARITY.md phase that brings it (Later), or the toggle's behaviour (0: there now)
};

// Pad 0..15 (pad 1 is index 0, bottom left).
const ShiftPadJob& shift_pad_job(unsigned pad);
// A toggle's state (false for the others).
bool shift_pad_on(const UiState& ui, unsigned pad);
// How bright the pad glows while Shift is held: 0 dark (not built yet),
// 1 faint (a job, or a toggle that is off), 2 bright (a toggle that is on).
unsigned shift_pad_level(const UiState& ui, unsigned pad);

} // namespace melodee
