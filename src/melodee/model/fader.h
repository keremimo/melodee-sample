// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <cstdint>

// What the fader moves on a track (Shift + Pad 9, parity phase 8): the MPC's
// seven targets in its order, then Melodee's mod wheel, then the track's own
// mix (its level, pan and a send into each bus), which every track has. The
// six Pad targets are the last pad hit's own value. Plain data: the
// track keeps the choice (Track::fader_target), the shell turns it into a
// card (shell/fader_job.h). New targets go at the end: the file keeps them
// by this number (FADR). Round 2 (fader C): the fader also learns the pot
// turned last on its track (Track::fader_learned), which goes over the
// target until the wheel on the Fader page picks one of these again; PIN
// keeps what it has.
namespace melodee {

enum class FaderTarget : uint8_t {
    PadVolume, PadPan, PadTune, PadAttack, PadDecay, PadCutoff, KitVolume, ModWheel,
    TrackLevel, TrackPan, SendA, SendB, SendC, SendD, // the track's mix, as Tracks and Sends show it
    Count
};
constexpr unsigned fader_targets = unsigned(FaderTarget::Count);
static_assert(fader_targets <= 16, "the shell's status packs the target in four bits (shell/lights.cpp)");
static_assert(unsigned(FaderTarget::SendD) - unsigned(FaderTarget::SendA) + 1 == bus_count, "a send target for each bus");
constexpr uint8_t fader_by_engine = 255; // Track::fader_target before one is picked

inline const char* fader_target_name(FaderTarget t) {
    static const char* const names[] {"Pad Volume", "Pad Pan", "Pad Tune", "Pad Attack", "Pad Decay / Release", "Pad Cutoff",
                                      "Kit Volume", "Mod Wheel", "Track Level", "Track Pan", "Send A", "Send B", "Send C", "Send D"};
    return unsigned(t) < fader_targets ? names[unsigned(t)] : "";
}

// Pad and kit targets need a kit (a sampler track); the mod wheel and the mix go everywhere.
inline bool fader_needs_kit(FaderTarget t) { return unsigned(t) <= unsigned(FaderTarget::KitVolume); }
// The last pad hit's own value (its name carries the pad).
inline bool fader_pad_target(FaderTarget t) { return unsigned(t) < unsigned(FaderTarget::KitVolume); }

// The engine's default, as Kerem decided: pad volume on a kit (the MPC's), the mod wheel on a synth.
inline FaderTarget fader_default(EngineKind engine) {
    return engine == EngineKind::Sampler ? FaderTarget::PadVolume : FaderTarget::ModWheel;
}

// The track's target as it plays: its choice, or the default where none is
// picked or the choice needs a kit the track no longer has.
inline FaderTarget fader_target(const Track& track) {
    if (track.fader_target < fader_targets) {
        auto chosen = FaderTarget(track.fader_target);
        if (track.engine == EngineKind::Sampler || !fader_needs_kit(chosen)) return chosen;
    }
    return fader_default(track.engine);
}

} // namespace melodee
