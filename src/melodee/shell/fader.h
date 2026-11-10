// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>

// The fader (0..127), parity phase 8. What it drives is shell/fader_job.h:
// the page's own job where it has one (Mix level, Sends send, Step Edit
// nudge, Knob FX mix, Compressor amount, Flex Beat mix), else the track's
// target from the Fader page (Shift + Pad 9). It takes over a value by the
// fader's takeover mode (Settings; Pickup by default, as on the MPC), goes
// through Context::apply so Seq Record writes its Motion lane (a pad's own
// lane, the kit's volume, the mod wheel's), and with Erase held asks to
// erase that lane.
namespace melodee {

class Context;

void fader_move(Context& c, int value, bool syncing);
// Whether the fader still has to take over the value it drives now (a page
// or target changed without it moving): for the screen, before a snapshot.
void fader_check(Context& c);
// The Fader page: pick the selected track's target (pad targets on kit tracks only), B3 on / off.
void set_fader_target(Context& c, unsigned target);
void toggle_fader(Context& c);

} // namespace melodee
