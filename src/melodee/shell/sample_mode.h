// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/context.h"

// The MPC's Sample Mode buttons on a kit (parity phase 4): Loop, Shift +
// Chop (Note On), 16 Levels and Shift + 16 Levels (its type), and Sample
// held while a pad picks a kit pad without playing it. The shell hands
// these buttons here; the Context jobs they call live in sample_mode.cpp.
namespace melodee {

// Sample: Shift opens Input Config at once; alone it waits for its release,
// so a pad pressed meanwhile only picks a kit pad (and no page opens).
void sample_button(Context& c, bool pressed);
// A pad while Sample is held: on a kit it becomes the last pad, silently.
// False when Sample is not held there.
bool sample_pad(Context& c, unsigned pad);
void loop_button(Context& c);   // Loop: the last pad loops or not; Shift reverse
void chop_button(Context& c);   // Chop opens Chop; Shift: Note On
void levels_button(Context& c); // 16 Levels on and off (kit or synth); Shift: its type

} // namespace melodee
