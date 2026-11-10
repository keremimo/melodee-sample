// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/bindings.h"
#include "melodee/shell/settings.h"
#include "melodee/shell/ui_state.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

// One move of an absolute control (K1-K3 or the fader, 0..127) over the
// value its Binding holds, by the takeover mode (Settings): whether the
// value changes and to what. Pure, so the knobs, Knob FX and the fader share
// it and the test runs it as it is (parity phase 8).
namespace melodee {

// Whether the control sits on the value already (within two of 127): then it
// drives it straight away. Called whenever the control's binding changes.
inline bool takeover_meets(int position, const Binding& b) {
    return position >= 0 && std::abs(position - int(b.fraction() * 127 + 0.5f)) <= 2;
}

// The control went from `before` to `position` (before -1: never seen). Returns
// true and the new value in `out` when the value changes; `control.engaged`
// turns true once control and value meet, from then on the value follows the
// control. Pickup waits for that; Scaled moves the value the control's way at
// a rate that makes them meet where the control's travel ends; Instant jumps.
inline bool takeover_move(Takeover mode, Pot& control, int before, int position, const Binding& b, int& out) {
    if (!b.bound()) return false;
    int target = int(b.fraction() * 127 + 0.5f);
    bool crossed = before >= 0 && ((before <= target && position >= target) || (before >= target && position <= target));
    if (mode == Takeover::Instant || before < 0 || crossed || std::abs(position - target) <= 2) control.engaged = true;
    if (!control.engaged) {
        if (mode == Takeover::Pickup || position == before) return false;
        // Scaled: the value covers its distance to the end the control heads for in the
        // same proportion as the control covers its own.
        // The value's place is kept unrounded between moves (control.scaled), so small
        // steps add up; if something else changed the value meanwhile it starts from there.
        float v = b.fraction(), p0 = before / 127.0f, p1 = position / 127.0f;
        if (control.scaled >= 0 && b.min + int(std::lround(control.scaled * float(b.max - b.min))) == b.value) v = control.scaled;
        float next = p1 > p0 ? v + (p1 - p0) * (1 - v) / std::max(1 - p0, 1e-3f)
                             : v - (p0 - p1) * v / std::max(p0, 1e-3f);
        next = std::clamp(next, 0.0f, 1.0f);
        control.scaled = next;
        int value = b.min + int(std::lround(next * float(b.max - b.min)));
        if (std::abs(position - int(next * 127 + 0.5f)) <= 2) control.engaged = true;
        if (value == b.value) return false;
        out = value;
        return true;
    }
    int value = b.at(position);
    if (value == b.value) return false;
    out = value;
    return true;
}

} // namespace melodee
