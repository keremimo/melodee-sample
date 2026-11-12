// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>

// Melodee's limits on the MPC (not the FM-1's). Each is one constant, so a
// decision still open in the handoff stays a one-line change.
namespace melodee {
constexpr unsigned track_count = 16;     // one a pad
constexpr unsigned pattern_count = 128;  // per track: eight banks of sixteen (A-H), as the MPC's 128 sequences
constexpr unsigned pattern_pads = 16;    // one bank of them: what the pads and the grids show at a time
constexpr unsigned pattern_banks = pattern_count / pattern_pads;
constexpr unsigned legacy_patterns = 16; // a track's patterns before the banks: what TIME and FEEL's groove kept
constexpr unsigned max_bars = 128;       // per pattern, as on the MPC
constexpr unsigned bar_steps = 16;       // steps the Steps and Roll pages show at a time
constexpr unsigned notes_per_step = 4;   // a chord on one step
constexpr unsigned motion_points = 8192; // a pattern's Motion since roadmap P1: points at tick resolution, locks among them
constexpr unsigned mod_routes = 8;       // open question 1: 8 drawn, 16 offered
constexpr unsigned max_params = 96;      // engine parameters a track stores
// Which engine parameters a file's values cover: 0 before phase 1d added pad
// expression at the end of each synth's list (AT > VIB and the rest), 1 since.
// Files of an older version get the newer parameters' initial values when
// loaded (engine_info.h, upgrade_params), not the zeros they carry.
constexpr unsigned params_version = 1;
constexpr unsigned name_length = 24;
// The sequencer's clock: 96 ticks a quarter note, as the MPC60 counted.
// Notes sit on any tick (Rec Quantize off keeps the played time); Q is the
// grid the pages step by and quantising snaps to. Files from before parity
// phase 2 counted 24 a quarter (legacy_ticks_per_quarter).
constexpr unsigned ticks_per_quarter = 96, sixteenth_ticks = ticks_per_quarter / 4;
constexpr unsigned legacy_ticks_per_quarter = 24;
constexpr unsigned max_pattern_events = 65535; // notes a pattern holds; an edit past it is refused
constexpr unsigned song_clips = 256;    // clips in the arrange view
constexpr unsigned song_markers = 16;
constexpr unsigned song_punches = 128;  // punches written into the song (Seq Record)
constexpr unsigned song_bars = 999;
constexpr unsigned song_tempo_points = 64; // roadmap P5: the song's tempo lane (model/tempo_lane.h)
// Effects along the signal (open question 5: slot counts and four buses).
constexpr unsigned track_slots = 6, bus_slots = 6, master_slots = 6, bus_count = 4;
constexpr unsigned max_slots = 6;
// The Looper's four tracks, L1-L4 (docs/LOOPER.md): a mix strip and a chain each, there while it is on.
constexpr unsigned loop_tracks = 4;
constexpr unsigned chain_count = track_count + bus_count + 1 + loop_tracks; // tracks, buses A-D, master, L1-L4
constexpr unsigned effect_kinds = 43;                          // EffectKind::Count, None included (engine/effect_info.h checks)
constexpr unsigned pad_slots = 4;                              // each sampler pad's own chain (PadFX)
// Motion lanes name engine parameters (0..max_params-1), then the track
// chain's effect parameters: motion_fx_base + slot * 6 + parameter.
constexpr unsigned motion_fx_base = max_params;
constexpr unsigned motion_fx_param(unsigned slot, unsigned parameter) { return motion_fx_base + slot * 6 + parameter; }
// Then a kit pad's own values (model/pad_params.h), each lane on one pad
// (MotionEvent::pad): motion_pad_base + PadParam.
constexpr unsigned motion_pad_base = motion_fx_base + track_slots * 6;
// Roadmap P4: the mix's own lanes, kept with the track's pattern as any lane: the track's level, pan and four sends;
// then the master's chain's effect parameters (motion_master_fx(slot, parameter)) and the buses' returns. A track's
// pattern moving the master's values moves them for the whole mix (the track in hand when they were recorded).
constexpr unsigned motion_mix_base = 160;
constexpr unsigned motion_level = motion_mix_base, motion_pan = motion_mix_base + 1;
constexpr unsigned motion_send(unsigned bus) { return motion_mix_base + 2 + bus % 4; }
constexpr unsigned motion_master_fx_base = motion_mix_base + 6;
constexpr unsigned motion_master_fx(unsigned slot, unsigned parameter) { return motion_master_fx_base + slot * 6 + parameter; }
constexpr unsigned motion_return_base = motion_master_fx_base + master_slots * 6;
constexpr unsigned motion_return(unsigned bus) { return motion_return_base + bus % 4; }
constexpr unsigned motion_mix_end = motion_return_base + bus_count; // past the last
constexpr bool is_mix_lane(unsigned param) { return param >= motion_mix_base && param < motion_mix_end; }
constexpr bool is_master_lane(unsigned param) { return param >= motion_master_fx_base && param < motion_mix_end; }
// Last, the track's mod wheel as the fader moves it (parity phase 8): one
// lane of its own, kept in the file's MOTW section.
constexpr unsigned motion_wheel = 255;
static_assert(motion_mix_end <= motion_wheel, "the mix's lanes fit below the mod wheel's");
// Just below it, a PLUGIN track's pitch bend as the fader moves it (-64..63; a MIDI track's bend is its own BEND
// parameter). Kept in MOTK with the rest.
constexpr unsigned motion_bend = 254;
static_assert(motion_mix_end <= motion_bend, "the mix's lanes fit below the pitch bend's");
// Plug-ins (docs/MELODEE.md, Plug-ins): a project's table of them. Each entry
// is one loaded instance, played by a PLUGIN track or sitting in a chain.
constexpr unsigned max_plugins = 32;
constexpr unsigned plugin_path_length = 192;
constexpr uint8_t no_plugin = 255;
constexpr uint8_t plugin_effect_kind = 42; // EffectKind::Plugin (engine/effect_info.h checks)
}
