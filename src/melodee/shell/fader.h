// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>

// The fader (0..127). By default the selected track's mod wheel; pages
// with an obvious job for it take it over (the decisions table's context
// overrides): on Step Edit it nudges the selected event's timing, a tick for
// each step the fader moves, up later and down earlier, as on the MPC.
// Parity phase 8 adds the Fader page (Shift + Pad 9) and the other targets.
namespace melodee {

class Context;

void fader_move(Context& c, int value, bool syncing);

} // namespace melodee
