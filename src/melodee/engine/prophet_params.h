// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_info.h"

// The PROPHET engine's parameters: a two-oscillator polysynth in the shape
// of Melodee's Prophet port (oscillators, mixer, four-pole filter, two
// envelopes, LFO, wheel and poly mod, performance), written for sample-base.
namespace melodee::prophet {

enum Param : int8_t {
    FreqA, SawA, PulseA, PwA, Sync,
    FreqB, FineB, SawB, TriB, PulseB, PwB, LoFreqB, KeybB,
    LevelA, LevelB, Noise,
    Cutoff, Resonance, EnvAmount, KeyTrack, VelFilter,
    FilterAttack, FilterDecay, FilterSustain, FilterRelease,
    AmpAttack, AmpDecay, AmpSustain, AmpRelease, VelAmp,
    LfoRate, LfoShape, LfoAmount, LfoFreq, LfoPw, LfoFilter,
    WheelFreq, WheelFilter, WheelLfo,
    PolyEnv, PolyOscB, PolyFreqA, PolyPwA, PolyFilter,
    Glide, Unison, Detune, Vintage, BendRange, Voices,
    AtVib, AtBright, VibRate, // pad pressure (phase 1d): new ones go last so older files load
    Count
};
constexpr unsigned legacy_count = AtVib; // what files from before phase 1d carry

const EngineInfo& info();

} // namespace melodee::prophet
