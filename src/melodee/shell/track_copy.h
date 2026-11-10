// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/history.h"

// Track copy (Shift + Erase with Pad Bank held, then two pads): one track
// onto another as one undo. MIDI thread; the audio thread gets the track
// whole through a TrackPack (engine/engine_host.h), so a copy never floods the
// command queue with steps and never stops the song.
namespace melodee {

class Context;

// The copy takes the sound (engine, sound, mod routes, effects, keys, arp and
// chord), the kit (pads, their chains and mutes, the bank) and the sixteen
// patterns with their motion. `to` keeps its own mix strip: level, pan,
// send, bus, mute and solo.
void copy_track(Context& c, unsigned from, unsigned to);
TrackState track_state(const Context& c, unsigned t);
// Sets the track and its patterns, then hands both, with the project's motion, to the audio thread.
void restore_track(Context& c, unsigned t, const TrackState& state);

} // namespace melodee
