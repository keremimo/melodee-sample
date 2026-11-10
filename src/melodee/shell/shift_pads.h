// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/context.h"

// Shift + Pad 1-16: the jobs printed under the pads. Phase 1a routes only
// the ones whose old button went to a printed job: 3 Double Seq (was Shift
// + Loop), 12 Song (was on Loop) and 16 Project (was Shift + Sample). The
// full dispatch, its overlay and pad lights are phase 1b (MELODEE_PARITY.md).
namespace melodee {

// A pad struck with Shift held. True when it was a Shift + pad job here;
// false leaves the pad to the page (Patterns launches a column, Punch locks).
bool shift_pad(Context& c, unsigned pad);

} // namespace melodee
