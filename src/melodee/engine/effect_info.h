// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_info.h"
#include <array>
#include <cstdint>

// The effects as the user interface sees them: 28 in six families, each with
// up to six parameters in two rows of three, and a small picture of what it
// does for its card. `built` marks the ones with DSP behind them yet.
namespace melodee {

enum class EffectKind : uint8_t {
    None,
    Room, Reverb, Plate, Shimmer,
    Echo, TapeEcho, PingPong, Looper,
    Chorus, Flanger, Phaser, PedalChorus, PedalPhaser, AutoPan, Tremolo,
    Drive, Tape, Crush, Spit, Fuzz,
    Eq, Filter, Formant, Width,
    Compressor, Gate, Transient, Limiter,
    Count
};

constexpr unsigned effect_params = 6;
enum class Family : uint8_t { Space, Time, Motion, Dirt, Tone, Dynamics, Count };

struct EffectInfo {
    EffectKind kind;
    const char* name;
    Family family;
    bool built;
    std::array<ParamInfo, effect_params> params; // label nullptr: no parameter there
    std::array<uint8_t, 11> picture;             // bar heights 0..100 for the card
};

const EffectInfo& effect_info(EffectKind kind);
const char* family_name(Family family);
// The effects of a family in picker order.
unsigned family_effects(Family family, std::array<EffectKind, 8>& out);
inline unsigned effect_count() { return unsigned(EffectKind::Count) - 1; }

} // namespace melodee
