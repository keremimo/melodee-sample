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
constexpr unsigned ticks_per_quarter = 24, ticks_per_bar = 4 * ticks_per_quarter;
constexpr unsigned song_clips = 256;    // clips in the arrange view
constexpr unsigned song_markers = 16;
constexpr unsigned song_punches = 128;  // punches written into the song (Seq Record)
constexpr unsigned song_bars = 999;
// Effects along the signal (open question 5: slot counts and four buses).
constexpr unsigned track_slots = 6, bus_slots = 6, master_slots = 6, bus_count = 4;
constexpr unsigned max_slots = 6;
constexpr unsigned chain_count = track_count + bus_count + 1; // tracks, buses A-D, master
constexpr unsigned pad_slots = 4;                              // each sampler pad's own chain (PadFX)
// Motion lanes name engine parameters (0..max_params-1), then the track
// chain's effect parameters: motion_fx_base + slot * 6 + parameter.
constexpr unsigned motion_fx_base = max_params;
constexpr unsigned motion_fx_param(unsigned slot, unsigned parameter) { return motion_fx_base + slot * 6 + parameter; }
}
