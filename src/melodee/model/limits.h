// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once

// Melodee's limits on the MPC (not the FM-1's). Each is one constant, so a
// decision still open in the handoff stays a one-line change.
namespace melodee {
constexpr unsigned track_count = 16;     // one a pad
constexpr unsigned pattern_count = 16;   // per track
constexpr unsigned max_steps = 256;      // per pattern, 1/16 steps
constexpr unsigned bar_steps = 16;
constexpr unsigned notes_per_step = 4;   // a chord on one step
constexpr unsigned motion_events = 256;  // open question 2: per project (as drawn) or per pattern
constexpr unsigned mod_routes = 8;       // open question 1: 8 drawn, 16 offered
constexpr unsigned max_params = 96;      // engine parameters a track stores
constexpr unsigned name_length = 24;
}
