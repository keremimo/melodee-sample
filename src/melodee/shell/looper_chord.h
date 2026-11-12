// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"
#include "surface/controls.h"
#include <cstdint>

// Loop and 16 Levels pressed together, in either order, open the Looper from any page (docs/LOOPER.md), Shift
// or not, and neither then does its own job: Loop's release is a hold's (nothing), and 16 Levels' toggle never
// comes, as it waits for its release (a tap acts on release). Pure, for the shell and the tests.
namespace melodee {

enum class ChordAct : uint8_t {
    Own,     // not the chord: the button's own handling goes on (Loop: loop_press)
    Looper,  // the other one is down: open the Looper
    Levels,  // 16 Levels' own job now: let go on its own, or Shift + 16 Levels (the type) as it goes down
    Nothing, // 16 Levels down, its job waiting for the release; or let go after the chord
};

inline ChordAct looper_chord(UiState& ui, SurfaceButton b, bool pressed) {
    if (b == SurfaceButton::Loop) {
        if (!pressed || !ui.levels_held) return ChordAct::Own;
        ui.levels_used = true;                    // 16 Levels' release: nothing
        ui.loop_held = ui.loop_grip = true;       // Loop's: nothing, as after a hold
        ui.loop_shift_held = false;               // no Reverse, and Shift held no Looper on or off
        return ChordAct::Looper;
    }
    if (b != SurfaceButton::SixteenLevels) return ChordAct::Own;
    if (!pressed) {
        bool own = ui.levels_held && !ui.levels_used;
        ui.levels_held = ui.levels_used = false;
        return own ? ChordAct::Levels : ChordAct::Nothing;
    }
    ui.levels_held = true;
    if (ui.loop_held || ui.loop_shift_held) { // Loop went down first (Shift + Loop too): the chord, Loop now a hold
        ui.levels_used = true;
        ui.loop_held = ui.loop_grip = true;
        ui.loop_shift_held = false;
        return ChordAct::Looper;
    }
    ui.levels_used = ui.shift; // Shift + 16 Levels: the type at once, as before
    return ui.shift ? ChordAct::Levels : ChordAct::Nothing;
}

} // namespace melodee
