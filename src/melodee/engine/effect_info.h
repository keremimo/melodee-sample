// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_info.h"
#include <array>
#include <cstdint>

// The effects as the user interface sees them: 41 in six families, each with
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
    // Parity phase 7: the MPC's Knob FX effects Melodee lacked (appended, so saved kinds keep their numbers).
    DiffDelay, SampleDelay, Spring, Pumper, AmpSim, TubeDrive, SoftClip, Ensemble, MultiChorus, AutoWah,
    Vintage, Vinyl, TapeEmu,
    // A plug-in effect (docs/MELODEE.md, Plug-ins): parameter 0 is its entry in Project::plugins; the plug-in does the rest.
    Plugin,
    Count
};

static_assert(unsigned(EffectKind::Count) == effect_kinds, "model/limits.h counts the effects");
static_assert(unsigned(EffectKind::Plugin) == plugin_effect_kind, "model/limits.h names the plug-in effect");
static_assert(fx_keyable(uint8_t(EffectKind::Compressor)) && fx_keyable(uint8_t(EffectKind::Gate)) && !fx_keyable(uint8_t(EffectKind::Limiter)),
              "model/project.h names the effects a key can drive");
constexpr unsigned effect_params = 6;
enum class Family : uint8_t { Space, Time, Motion, Dirt, Tone, Dynamics, Plugins, Count };

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
// The effects of a family in picker order (two columns of six at most).
using FamilyList = std::array<EffectKind, 12>;
unsigned family_effects(Family family, FamilyList& out);
inline unsigned effect_count() { return unsigned(EffectKind::Count) - 2; } // Melodee's own: not None, not Plugin

// Knob FX's list (parity phase 7): the MPC's effects in its order, each as
// one of Melodee's effects (a filter with its type set), then Melodee's own.
struct KnobFxEntry {
    const char* name;
    EffectKind kind;
    int8_t param = -1; // a parameter the entry sets over the effect's initial values
    int16_t value = 0;
};
unsigned knob_fx_count();
const KnobFxEntry& knob_fx_entry(unsigned index);
unsigned knob_fx_mpc_count(); // the first ones are the MPC's

} // namespace melodee
