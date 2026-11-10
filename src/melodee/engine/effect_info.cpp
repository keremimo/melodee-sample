// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/effect_info.h"

namespace melodee {
namespace {
const char* const filter_types[] {"Low pass", "Band pass", "High pass"};
const char* const tremolo_shapes[] {"Sine", "Square"};

constexpr ParamInfo P(const char* label, int min, int max, int initial, Format format = Format::Percent,
                      const char* const* choices = nullptr) {
    return {label, int16_t(min), int16_t(max), int16_t(initial), format, choices};
}
constexpr ParamInfo none {nullptr, 0, 0, 0, Format::Number, nullptr};
constexpr ParamInfo mix(int initial = 35) { return P("MIX", 0, 100, initial); }

using K = EffectKind;
using F = Family;
const EffectInfo effects[] {
    {K::None, "Empty", F::Space, false, {none, none, none, none, none, none}, {}},
    {K::Room, "Room", F::Space, true, {P("SIZE", 0, 100, 55), P("DECAY", 60, 100, 82, Format::Time), P("DAMP", 0, 100, 40), mix(), P("PREDELAY", 0, 100, 5), P("WIDTH", 0, 100, 80)},
     {20, 34, 50, 66, 80, 70, 56, 44, 32, 22, 14}},
    {K::Reverb, "Reverb", F::Space, true, {P("SIZE", 0, 100, 80), P("DECAY", 60, 100, 86, Format::Time), P("DAMP", 0, 100, 35), mix(30), P("PREDELAY", 0, 100, 15), P("WIDTH", 0, 100, 100)},
     {12, 26, 40, 52, 62, 70, 74, 70, 60, 46, 30}},
    {K::Plate, "Plate", F::Space, true, {P("SIZE", 0, 100, 60), P("DECAY", 60, 100, 84, Format::Time), P("DAMP", 0, 100, 15), mix(30), P("PREDELAY", 0, 100, 0), P("WIDTH", 0, 100, 100)},
     {60, 72, 70, 64, 58, 52, 46, 40, 34, 28, 22}},
    {K::Shimmer, "Shimmer", F::Space, true, {P("SIZE", 0, 100, 75), P("DECAY", 60, 100, 88, Format::Time), P("DAMP", 0, 100, 30), mix(35), P("SHIMMER", 0, 100, 55), P("WIDTH", 0, 100, 100)}, {10, 20, 34, 48, 62, 76, 86, 92, 96, 98, 100}},
    {K::Echo, "Echo", F::Time, true, {P("TIME", 30, 78, 60, Format::Time), P("FEEDBACK", 0, 95, 40), mix(30), P("TONE", 0, 100, 60), none, none},
     {88, 0, 64, 0, 46, 0, 32, 0, 20, 0, 12}},
    {K::TapeEcho, "Tape echo", F::Time, true, {P("TIME", 30, 78, 62, Format::Time), P("FEEDBACK", 0, 95, 50), mix(30), P("WOW", 0, 100, 25), P("TONE", 0, 100, 40), none},
     {84, 0, 66, 0, 50, 0, 38, 0, 28, 0, 20}},
    {K::PingPong, "Ping pong", F::Time, true, {P("TIME", 30, 78, 58, Format::Time), P("FEEDBACK", 0, 95, 45), mix(30), P("TONE", 0, 100, 60), none, none},
     {88, 0, 0, 64, 0, 0, 46, 0, 0, 32, 0}},
    {K::Looper, "Looper", F::Time, true, {P("LENGTH", 30, 78, 70, Format::Time), P("RECORD", 0, 1, 0, Format::Switch), P("KEEP", 0, 100, 90), mix(100), none, none}, {60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60}},
    {K::Chorus, "Chorus", F::Motion, true, {P("RATE", 0, 100, 30, Format::Rate), P("DEPTH", 0, 100, 50), mix(50), none, none, none},
     {30, 44, 58, 66, 58, 44, 30, 22, 30, 44, 58}},
    {K::Flanger, "Flanger", F::Motion, true, {P("RATE", 0, 100, 20, Format::Rate), P("DEPTH", 0, 100, 60), P("FEEDBACK", -95, 95, 50), mix(50), none, none},
     {20, 50, 80, 50, 20, 50, 80, 50, 20, 50, 80}},
    {K::Phaser, "Phaser", F::Motion, true, {P("RATE", 0, 100, 25, Format::Rate), P("DEPTH", 0, 100, 70), P("FEEDBACK", 0, 95, 40), mix(50), none, none},
     {40, 60, 76, 60, 40, 30, 40, 60, 76, 60, 40}},
    {K::PedalChorus, "Pedal chorus", F::Motion, true, {P("RATE", 0, 100, 35, Format::Rate), P("DEPTH", 0, 100, 60), P("TONE", 0, 100, 45), mix(50), none, none}, {30, 44, 58, 66, 58, 44, 30, 22, 30, 44, 58}},
    {K::PedalPhaser, "Pedal phaser", F::Motion, true, {P("SPEED", 0, 100, 30, Format::Rate), P("DEPTH", 0, 100, 80), P("COLOUR", 0, 1, 0, Format::Switch), mix(50), none, none}, {40, 60, 76, 60, 40, 30, 40, 60, 76, 60, 40}},
    {K::AutoPan, "Auto-pan", F::Motion, true, {P("RATE", 0, 100, 40, Format::Rate), P("DEPTH", 0, 100, 70), none, none, none, none},
     {80, 60, 30, 10, 30, 60, 80, 60, 30, 10, 30}},
    {K::Tremolo, "Tremolo", F::Motion, true, {P("RATE", 0, 100, 55, Format::Rate), P("DEPTH", 0, 100, 60), P("SHAPE", 0, 1, 0, Format::Choice, tremolo_shapes), none, none, none},
     {90, 40, 90, 40, 90, 40, 90, 40, 90, 40, 90}},
    {K::Drive, "Drive", F::Dirt, true, {P("DRIVE", 0, 100, 40), P("TONE", 0, 100, 60), mix(100), none, none, none},
     {50, 78, 92, 96, 96, 92, 96, 96, 90, 74, 48}},
    {K::Tape, "Tape", F::Dirt, true, {P("DRIVE", 0, 100, 40), P("WOW", 0, 100, 18), mix(100), none, none, none},
     {28, 46, 60, 52, 70, 64, 82, 58, 44, 36, 50}},
    {K::Crush, "Crush", F::Dirt, true, {P("BITS", 2, 16, 8, Format::Number), P("RATE", 0, 100, 30), mix(100), none, none, none},
     {60, 60, 20, 20, 80, 80, 40, 40, 70, 70, 30}},
    {K::Spit, "Spit", F::Dirt, true, {P("AMOUNT", 0, 100, 45), P("GRIT", 0, 100, 50), mix(100), none, none, none}, {70, 20, 90, 10, 60, 30, 80, 15, 50, 25, 65}},
    {K::Fuzz, "Fuzz", F::Dirt, true, {P("FUZZ", 0, 100, 60), P("TONE", 0, 100, 50), mix(100), none, none, none},
     {96, 96, 96, 20, 20, 96, 96, 96, 20, 20, 96}},
    {K::Eq, "EQ", F::Tone, true, {P("LOW", -12, 12, 0, Format::Number), P("MID", -12, 12, 0, Format::Number), P("HIGH", -12, 12, 0, Format::Number), P("MID FREQ", 0, 100, 50), none, none},
     {40, 52, 60, 48, 36, 30, 44, 62, 74, 66, 50}},
    {K::Filter, "Filter", F::Tone, true, {P("CUTOFF", 0, 100, 70), P("RESO", 0, 100, 20), P("TYPE", 0, 2, 0, Format::Choice, filter_types), none, none, none},
     {70, 72, 74, 76, 74, 60, 40, 24, 12, 6, 2}},
    {K::Formant, "Formant", F::Tone, true, {P("VOWEL", 0, 100, 0), P("RESO", 0, 100, 60), P("SHIFT", -12, 12, 0, Format::Semitones), mix(100), none, none}, {30, 70, 40, 20, 60, 30, 20, 50, 25, 15, 10}},
    {K::Width, "Width", F::Tone, true, {P("WIDTH", 0, 200, 140), none, none, none, none, none},
     {30, 48, 66, 84, 84, 66, 48, 30, 48, 66, 84}},
    {K::Compressor, "Compressor", F::Dynamics, true, {P("THRESH", -40, 0, -18, Format::Number), P("RATIO", 1, 20, 4, Format::Number), P("MAKEUP", 0, 24, 6, Format::Number), P("ATTACK", 0, 100, 20, Format::Time), P("RELEASE", 0, 100, 45, Format::Time), mix(100)},
     {96, 70, 60, 58, 58, 58, 58, 58, 58, 58, 58}},
    {K::Gate, "Gate", F::Dynamics, true, {P("THRESH", -80, 0, -40, Format::Number), P("RELEASE", 0, 100, 30, Format::Time), none, none, none, none},
     {90, 70, 50, 0, 0, 0, 90, 70, 50, 0, 0}},
    {K::Transient, "Transient", F::Dynamics, true, {P("ATTACK", -100, 100, 40, Format::Number), P("SUSTAIN", -100, 100, 0, Format::Number), mix(100), none, none, none},
     {96, 40, 26, 20, 16, 14, 12, 10, 10, 8, 8}},
    {K::Limiter, "Limiter", F::Dynamics, true, {P("CEILING", -24, 0, -1, Format::Number), P("RELEASE", 0, 100, 30, Format::Time), none, none, none, none},
     {70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70}},
};
static_assert(sizeof(effects) / sizeof(effects[0]) == size_t(EffectKind::Count), "one entry per effect");
} // namespace

const EffectInfo& effect_info(EffectKind kind) { return effects[unsigned(kind) < unsigned(EffectKind::Count) ? unsigned(kind) : 0]; }

const char* family_name(Family family) {
    static const char* names[] {"Space", "Time", "Motion", "Dirt", "Tone", "Dynamics"};
    return names[unsigned(family) % unsigned(Family::Count)];
}

unsigned family_effects(Family family, std::array<EffectKind, 8>& out) {
    unsigned n = 0;
    for (unsigned k = 1; k < unsigned(EffectKind::Count) && n < out.size(); ++k)
        if (effects[k].family == family) out[n++] = EffectKind(k);
    return n;
}

} // namespace melodee
