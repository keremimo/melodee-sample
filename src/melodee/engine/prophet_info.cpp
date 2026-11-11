// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The PROPHET engine as the user interface sees it: parameter ranges, Edit
// pages and the factory sounds (made for this engine, not copied from any
// instrument's programs).
#include "melodee/engine/expression.h"
#include "melodee/engine/prophet_params.h"

namespace melodee::prophet {
namespace {
const char* const key_track[] {"Off", "Half", "Full"};
const char* const lfo_shapes[] {"Triangle", "Saw", "Square", "Random"};

constexpr ParamInfo P(const char* label, int min, int max, int initial, Format format = Format::Percent,
                      const char* const* choices = nullptr) {
    return {label, int16_t(min), int16_t(max), int16_t(initial), format, choices};
}

const ParamInfo params[Count] {
    P("FREQ A", -24, 24, 0, Format::Semitones), P("SAW A", 0, 1, 1, Format::Switch), P("PULSE A", 0, 1, 0, Format::Switch),
    P("PW A", 5, 95, 50), P("SYNC", 0, 1, 0, Format::Switch),
    P("FREQ B", -24, 24, 0, Format::Semitones), P("FINE B", -50, 50, 7, Format::Cents), P("SAW B", 0, 1, 1, Format::Switch),
    P("TRI B", 0, 1, 0, Format::Switch), P("PULSE B", 0, 1, 0, Format::Switch), P("PW B", 5, 95, 50),
    P("LO FREQ", 0, 1, 0, Format::Switch), P("KEYB B", 0, 1, 1, Format::Switch),
    P("LEVEL A", 0, 100, 80), P("LEVEL B", 0, 100, 60), P("NOISE", 0, 100, 0),
    P("CUTOFF", 0, 100, 62), P("RESO", 0, 100, 28), P("ENV AMT", 0, 100, 45),
    P("KEYBOARD", 0, 2, 1, Format::Choice, key_track), P("VEL > FLT", 0, 100, 40),
    P("ATTACK", 0, 100, 0, Format::Time), P("DECAY", 0, 100, 45, Format::Time), P("SUSTAIN", 0, 100, 30),
    P("RELEASE", 0, 100, 35, Format::Time),
    P("ATTACK", 0, 100, 0, Format::Time), P("DECAY", 0, 100, 50, Format::Time), P("SUSTAIN", 0, 100, 80),
    P("RELEASE", 0, 100, 35, Format::Time), P("VEL > AMP", 0, 100, 65),
    P("RATE", 0, 100, 55, Format::Rate), P("SHAPE", 0, 3, 0, Format::Choice, lfo_shapes), P("AMOUNT", 0, 100, 0),
    P("> FREQ", 0, 100, 10), P("> PW", 0, 100, 0), P("> FILTER", 0, 100, 0),
    P("> FREQ", 0, 100, 15), P("> FILTER", 0, 100, 30), P("> LFO", 0, 100, 60),
    P("FILT ENV", 0, 100, 0), P("OSC B", 0, 100, 0), P("> FREQ A", 0, 1, 0, Format::Switch),
    P("> PW A", 0, 1, 0, Format::Switch), P("> FILTER", 0, 1, 0, Format::Switch),
    P("GLIDE", 0, 100, 0), P("UNISON", 0, 1, 0, Format::Switch), P("DETUNE", 0, 100, 20), P("VINTAGE", 0, 100, 15),
    P("BEND", 0, 12, 2, Format::Semitones), P("VOICES", 1, 8, 8, Format::Number),
    P("AT > VIB", 0, 100, expression::default_at_vib), P("AT > BRIGHT", 0, 100, expression::default_at_bright),
    P("VIB RATE", 0, 100, expression::default_vib_rate, Format::Rate),
};

constexpr int8_t _ = -1;
const EditPage pages[] {
    {"Oscillator A", "OSC A", Picture::Wave, {{{FreqA, PwA, Sync}, {SawA, PulseA, _}, {_, _, _}}}},
    {"Oscillator B", "OSC B", Picture::WaveB, {{{FreqB, FineB, PwB}, {SawB, TriB, PulseB}, {LoFreqB, KeybB, _}}}},
    {"Mixer", "MIXER", Picture::Mixer, {{{LevelA, LevelB, Noise}, {_, _, _}, {_, _, _}}}},
    {"Filter", "FILTER", Picture::Filter, {{{Cutoff, Resonance, EnvAmount}, {KeyTrack, VelFilter, _}, {_, _, _}}}},
    {"Filter envelope", "FILTER ENV", Picture::Envelope, {{{FilterAttack, FilterDecay, FilterSustain}, {FilterRelease, _, _}, {_, _, _}}}},
    {"Amp envelope", "AMP ENV", Picture::Envelope, {{{AmpAttack, AmpDecay, AmpSustain}, {AmpRelease, VelAmp, _}, {_, _, _}}}},
    {"LFO", "LFO", Picture::Lfo, {{{LfoRate, LfoShape, LfoAmount}, {LfoFreq, LfoPw, LfoFilter}, {_, _, _}}}},
    {"Wheel", "WHEEL", Picture::Bars, {{{WheelFreq, WheelFilter, WheelLfo}, {_, _, _}, {_, _, _}}}},
    {"Poly mod", "POLY MOD", Picture::Bars, {{{PolyEnv, PolyOscB, _}, {PolyFreqA, PolyPwA, PolyFilter}, {_, _, _}}}},
    {"Performance", "PERFORM", Picture::Bars, {{{Glide, Unison, Detune}, {Vintage, BendRange, Voices}, {_, _, _}}}},
    // Velocity and pad pressure together (the velocity ones also sit on Filter and Amp envelope).
    {"Touch", "TOUCH", Picture::Bars, {{{VelAmp, VelFilter, _}, {AtVib, AtBright, VibRate}, {_, _, _}}}},
};

// Each sound lists only what differs from the initial values.
const Sound sounds[] {
    {"INIT VOICE", {{{-1, 0}}}},
    {"FUNK BASS II", {{{FreqA, -12}, {FreqB, -12}, {FineB, 5}, {PulseB, 1}, {SawB, 0}, {LevelB, 70},
                       {Cutoff, 38}, {Resonance, 35}, {EnvAmount, 62}, {FilterDecay, 38}, {FilterSustain, 10},
                       {AmpDecay, 45}, {AmpSustain, 60}, {AmpRelease, 18}, {Voices, 1}, {Glide, 8}, {-1, 0}}}},
    {"RUBBER BASS", {{{FreqA, -12}, {SawA, 0}, {PulseA, 1}, {PwA, 30}, {FreqB, -24}, {SawB, 0}, {TriB, 1},
                      {Cutoff, 30}, {Resonance, 55}, {EnvAmount, 55}, {FilterDecay, 42}, {FilterSustain, 0},
                      {AmpSustain, 70}, {AmpRelease, 20}, {Voices, 1}, {-1, 0}}}},
    {"SYNC LEAD", {{{Sync, 1}, {FreqA, 7}, {LevelB, 30}, {Cutoff, 70}, {Resonance, 20}, {EnvAmount, 30},
                    {PolyEnv, 55}, {PolyFreqA, 1}, {FilterDecay, 55}, {FilterSustain, 20}, {Voices, 1}, {Glide, 15},
                    {LfoAmount, 15}, {-1, 0}}}},
    {"BRASS STAB", {{{FineB, 9}, {Cutoff, 42}, {Resonance, 15}, {EnvAmount, 60}, {FilterAttack, 30}, {FilterDecay, 45},
                     {FilterSustain, 45}, {AmpAttack, 18}, {AmpSustain, 85}, {AmpRelease, 30}, {-1, 0}}}},
    {"SOFT PAD", {{{FineB, 12}, {PulseA, 1}, {PwA, 35}, {Cutoff, 45}, {Resonance, 10}, {EnvAmount, 25},
                   {FilterAttack, 70}, {FilterDecay, 70}, {FilterSustain, 60}, {FilterRelease, 70},
                   {AmpAttack, 65}, {AmpSustain, 90}, {AmpRelease, 72}, {LfoAmount, 20}, {LfoPw, 40}, {LfoFreq, 0},
                   {Vintage, 35}, {-1, 0}}}},
    {"GLASS PLUCK", {{{SawA, 0}, {PulseA, 1}, {PwA, 18}, {FreqB, 12}, {SawB, 0}, {TriB, 1}, {LevelB, 45},
                      {Cutoff, 48}, {Resonance, 40}, {EnvAmount, 55}, {FilterDecay, 32}, {FilterSustain, 0},
                      {AmpDecay, 40}, {AmpSustain, 0}, {AmpRelease, 40}, {-1, 0}}}},
    {"WARM KEYS", {{{SawA, 0}, {PulseA, 1}, {PwA, 45}, {SawB, 0}, {TriB, 1}, {FineB, 4}, {Cutoff, 40},
                    {EnvAmount, 35}, {KeyTrack, 2}, {FilterDecay, 50}, {FilterSustain, 25}, {AmpDecay, 58},
                    {AmpSustain, 35}, {AmpRelease, 40}, {VelFilter, 45}, {-1, 0}}}},
    {"SUB BASS", {{{FreqA, -12}, {SawA, 0}, {PulseA, 0}, {FreqB, -12}, {SawB, 0}, {TriB, 1}, {LevelA, 0},
                   {LevelB, 100}, {Cutoff, 35}, {Resonance, 0}, {EnvAmount, 10}, {Voices, 1}, {-1, 0}}}},
    {"STRING MACHINE", {{{PulseA, 1}, {SawA, 1}, {PwA, 25}, {FineB, 14}, {Cutoff, 55}, {Resonance, 5}, {EnvAmount, 10},
                         {AmpAttack, 55}, {AmpSustain, 95}, {AmpRelease, 60}, {LfoAmount, 25}, {LfoPw, 60},
                         {LfoFreq, 0}, {LfoRate, 40}, {Vintage, 30}, {-1, 0}}}},
    {"POLY SWEEP", {{{Cutoff, 20}, {Resonance, 65}, {EnvAmount, 70}, {FilterAttack, 72}, {FilterDecay, 75},
                     {FilterSustain, 30}, {FilterRelease, 70}, {AmpAttack, 40}, {AmpSustain, 90}, {AmpRelease, 70},
                     {PolyOscB, 25}, {PolyFilter, 1}, {-1, 0}}}},
    {"UNISON LEAD", {{{Unison, 1}, {Detune, 35}, {Cutoff, 64}, {Resonance, 22}, {EnvAmount, 35}, {FilterDecay, 50},
                      {FilterSustain, 40}, {Glide, 20}, {-1, 0}}}},
    {"NOISE HAT", {{{LevelA, 0}, {LevelB, 0}, {Noise, 100}, {Cutoff, 88}, {Resonance, 30}, {KeyTrack, 0},
                    {EnvAmount, 0}, {AmpDecay, 22}, {AmpSustain, 0}, {AmpRelease, 20}, {-1, 0}}}},
};
// Stage: B1 SOUND (preset, the filter, mix), B2 SHAPE (the envelopes, the LFO), B3 PLAY (arp, performance, effects).
using K = StageKind;
using S = StagePicture;
const StageGroups stage {{{
    {{{stage_preset, {"TONE", K::Params, S::Filter, {{Cutoff, Resonance, EnvAmount}}}, stage_mix}}, 3},
    {{{{"FILTER ENV", K::Params, S::Envelope, {{FilterAttack, FilterDecay, FilterSustain}}, {{FilterRelease, _, _}}},
       {"AMP ENV", K::Params, S::Envelope, {{AmpAttack, AmpDecay, AmpSustain}}, {{AmpRelease, VelAmp, _}}},
       {"LFO", K::Params, S::Lfo, {{LfoRate, LfoShape, LfoAmount}}, {{LfoFreq, LfoPw, LfoFilter}}}}}, 3},
    {{{stage_arp, stage_chord, {"PERFORM", K::Params, S::ShiftRows, {{Glide, Unison, Detune}}, {{Vintage, BendRange, Voices}}}, stage_fx}}, 4},
}}};
} // namespace

const EngineInfo& info() {
    static const EngineInfo engine {EngineKind::Prophet, "PROPHET", true, params, Count, pages,
                                    sizeof(pages) / sizeof(pages[0]), {Cutoff, Resonance, EnvAmount},
                                    sounds, sizeof(sounds) / sizeof(sounds[0]), nullptr, legacy_count, true, &stage};
    return engine;
}

} // namespace melodee::prophet
