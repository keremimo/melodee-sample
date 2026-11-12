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

// Track move (B2 MOVE with Pad Bank held, or Shift + B2 on Tracks, then two pads): `from` goes to `to` whole,
// mix strip and all, as one undo; a track already at `to` trades places with it (shell/track_swap.h).
void move_track(Context& c, unsigned from, unsigned to);
// The two trade places and the audio thread hears both again, with all that names them: a move, its undo, its redo.
void trade_tracks(Context& c, unsigned a, unsigned b);
// Why a and b cannot trade places now, "" when they can: Seq Record's take, or a take that hears one of them
// (a loop recording or overdubbing it, Sample Record from it) and would lose its source half way.
std::string swap_waits(const Context& c, unsigned a, unsigned b);

// Track clear (Erase + pad with Pad Bank held, asked first): the track empty
// again as one undo. Its sound, kit and effects go, its sixteen patterns are
// one bar of the project's time signature with nothing in them; it keeps its
// mix strip, as a copy onto it does.
void clear_track(Context& c, unsigned t);
// Whether a clear would take anything: an engine, or a pattern's notes, motion or kept punches
// (an empty track's patterns may still play MIDI out).
inline bool track_clearable(const Track& track, const std::array<Pattern, pattern_count>& patterns) {
    if (track.used()) return true;
    for (const auto& p : patterns)
        if (!p.events.empty() || !p.motion.empty() || !p.punches.empty()) return true;
    return false;
}

} // namespace melodee
