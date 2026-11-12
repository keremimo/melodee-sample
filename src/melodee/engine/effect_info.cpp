// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/effect_info.h"

namespace melodee {
namespace {
const char* const filter_types[] {"Low pass", "Band pass", "High pass"};
const char* const tremolo_shapes[] {"Sine", "Square"};
// SYNC on the delays (roadmap P4): Off (TIME in ms), else a note value of the song's clock (delay_sync_quarters()).
const char* const delay_syncs[] {"Off", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/2D", "1/1"};
const char* const pumper_speeds[] {"Bar", "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"};
const char* const amp_cabs[] {"D.I.", "Brit", "1x8\"", "1x12\"", "2x10\"", "2x12\"", "4x10\"", "4x12\"", "1x15\" Bass", "4x10\" Bass", "Radio"};
const char* const clip_shapes[] {"Tanh", "Sine", "Parabolic"};
const char* const chorus_voices[] {"3", "4", "6"};
const char* const vintage_types[] {"MPC3000", "MPC60", "SP1200", "SP1200 Ring"};

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
    {K::Echo, "Echo", F::Time, true, {P("TIME", 30, 78, 60, Format::Time), P("FEEDBACK", 0, 95, 40), mix(30), P("TONE", 0, 100, 60), P("SYNC", 0, 13, 0, Format::Choice, delay_syncs), none},
     {88, 0, 64, 0, 46, 0, 32, 0, 20, 0, 12}},
    {K::TapeEcho, "Tape echo", F::Time, true, {P("TIME", 30, 78, 62, Format::Time), P("FEEDBACK", 0, 95, 50), mix(30), P("WOW", 0, 100, 25), P("TONE", 0, 100, 40), P("SYNC", 0, 13, 0, Format::Choice, delay_syncs)},
     {84, 0, 66, 0, 50, 0, 38, 0, 28, 0, 20}},
    {K::PingPong, "Ping pong", F::Time, true, {P("TIME", 30, 78, 58, Format::Time), P("FEEDBACK", 0, 95, 45), mix(30), P("TONE", 0, 100, 60), P("SYNC", 0, 13, 0, Format::Choice, delay_syncs), none},
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
    {K::Limiter, "Limiter", F::Dynamics, true, {P("CEILING", -24, 0, -1, Format::Number), P("RELEASE", 0, 100, 30, Format::Time), P("LOOKAHEAD", 0, 10, 0, Format::Ms), none, none, none},
     {70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70}},
    // Parity phase 7: the rest of the MPC's Knob FX list. Row 1 is K1-K3 as on the MPC, row 2 its Shift + K1-K3.
    {K::DiffDelay, "Diff delay", F::Time, true, {P("TIME", 30, 75, 58, Format::Time), P("FEEDBACK", 0, 95, 45), mix(30), P("DIFFUSION", 0, 100, 70), P("HIGH DAMP", 0, 100, 40), P("SYNC", 0, 13, 0, Format::Choice, delay_syncs)},
     {70, 20, 56, 30, 44, 34, 36, 30, 28, 24, 20}},
    {K::SampleDelay, "Sample delay", F::Time, true, {P("LEFT", 0, 250, 0, Format::Ms), P("RIGHT", 0, 250, 12, Format::Ms), mix(100), none, none, none},
     {90, 0, 90, 0, 0, 0, 0, 0, 0, 0, 0}},
    {K::Spring, "Spring", F::Space, true, {P("PREDELAY", 0, 250, 0, Format::Ms), P("TIME", 10, 100, 25, Format::Seconds), mix(30), P("WIDTH", 0, 100, 70), P("DIFFUSION", 0, 100, 60), P("LOW CUT", 20, 1000, 120, Format::Hertz)},
     {80, 30, 70, 26, 58, 22, 46, 18, 34, 14, 22}},
    {K::Pumper, "Pumper", F::Dynamics, true, {P("SPEED", 0, 10, 3, Format::Choice, pumper_speeds), P("SHAPE", 0, 100, 50), P("DEPTH", 0, 100, 70), P("ATTACK", 0, 100, 5), P("HOLD", 0, 100, 10), P("RELEASE", 0, 100, 60)},
     {10, 40, 70, 90, 10, 40, 70, 90, 10, 40, 70}},
    {K::AmpSim, "Amp sim", F::Dirt, true, {P("CAB", 0, 10, 3, Format::Choice, amp_cabs), P("DRIVE", 0, 110, 40, Format::Tenths), P("SOFT CLIP", 0, 100, 50), P("BASS", -12, 12, 0, Format::Decibels), P("MID", -12, 12, 0, Format::Decibels), P("TREBLE", -12, 12, 0, Format::Decibels)},
     {30, 70, 90, 96, 90, 86, 80, 60, 40, 24, 10}},
    {K::TubeDrive, "Tube drive", F::Dirt, true, {P("DRIVE", 0, 100, 40), P("HEADROOM", -30, 0, -6, Format::Decibels), P("SATURATION", 0, 100, 50), none, none, none},
     {40, 64, 80, 88, 92, 92, 90, 84, 72, 56, 40}},
    {K::SoftClip, "Soft clipper", F::Dirt, true, {P("DRIVE", 0, 40, 6, Format::Decibels), P("SHAPE", 0, 2, 0, Format::Choice, clip_shapes), mix(100), P("OUTPUT", -24, 0, 0, Format::Decibels), none, none},
     {30, 60, 80, 90, 92, 92, 92, 90, 80, 60, 30}},
    {K::Ensemble, "Ensemble", F::Motion, true, {P("RATE", 0, 100, 30, Format::Rate), P("DEPTH", 0, 24, 4, Format::Ms), mix(50), P("DELAY", 0, 24, 8, Format::Ms), P("SHIMMER", 0, 100, 30), P("WIDTH", 0, 100, 80)},
     {40, 56, 66, 60, 48, 52, 64, 70, 58, 46, 54}},
    {K::MultiChorus, "Multi-chorus", F::Motion, true, {P("RATE", 0, 100, 25, Format::Rate), P("DEPTH", 0, 24, 5, Format::Ms), mix(50), P("VOICES", 0, 2, 1, Format::Choice, chorus_voices), P("DELAY", 0, 24, 12, Format::Ms), none},
     {30, 50, 62, 50, 38, 50, 62, 50, 38, 50, 62}},
    {K::AutoWah, "Auto-wah", F::Tone, true, {P("SENS", 0, 100, 60), P("RESO", 0, 100, 60), mix(100), P("CENTER", 0, 100, 30), P("ATTACK", 0, 100, 20), P("RELEASE", 0, 100, 40)},
     {10, 20, 40, 76, 96, 76, 40, 20, 12, 8, 6}},
    {K::Vintage, "Vintage", F::Dirt, true, {P("TYPE", 0, 3, 1, Format::Choice, vintage_types), none, mix(100), none, none, none},
     {60, 60, 40, 40, 70, 70, 50, 50, 30, 30, 20}},
    {K::Vinyl, "Vinyl", F::Dirt, true, {P("TONE", 0, 100, 50), P("CRACKLE", 0, 100, 40), P("PITCH", 10, 100, 30), none, none, none},
     {50, 54, 90, 50, 46, 52, 48, 84, 50, 48, 52}},
    {K::TapeEmu, "Tape emulator", F::Dirt, true, {P("WOW", 10, 100, 30), P("NOISE", 10, 100, 20), P("PITCH", 20, 100, 30), none, none, none},
     {40, 52, 60, 56, 62, 58, 64, 60, 54, 50, 46}},
    // Not DSP of Melodee's own: Effect passes sound through and the engine host runs the plug-in instead.
    {K::Plugin, "Plug-in", F::Plugins, true, {P("ENTRY", 0, 31, 0, Format::Number), none, none, none, none, none},
     {30, 50, 70, 50, 30, 50, 70, 50, 30, 50, 70}},
};
static_assert(sizeof(effects) / sizeof(effects[0]) == size_t(EffectKind::Count), "one entry per effect");
} // namespace

const EffectInfo& effect_info(EffectKind kind) { return effects[unsigned(kind) < unsigned(EffectKind::Count) ? unsigned(kind) : 0]; }

namespace {
const KnobFxEntry knob_entries[] {
    {"Delay", K::Echo}, {"Diff Delay", K::DiffDelay}, {"Tape Delay", K::TapeEcho}, {"Sample Delay", K::SampleDelay},
    {"Reverb Small", K::Room}, {"Reverb Medium", K::Plate}, {"Reverb Large", K::Reverb, 0, 100}, {"Spring Reverb", K::Spring},
    {"HP Filter", K::Filter, 2, 2}, {"LP Filter", K::Filter, 2, 0}, {"BP Filter", K::Filter, 2, 1},
    {"Bus Compressor", K::Compressor}, {"Limiter", K::Limiter}, {"Pumper", K::Pumper}, {"Transient", K::Transient},
    {"Noise Gate", K::Gate}, {"Amp Sim", K::AmpSim}, {"Tube Drive", K::TubeDrive}, {"Soft Clipper", K::SoftClip},
    {"Ensemble", K::Ensemble}, {"Multi-Chorus", K::MultiChorus}, {"Phaser", K::Phaser}, {"Flanger", K::Flanger},
    {"Auto-Wah", K::AutoWah}, {"Auto-Pan", K::AutoPan}, {"Vintage", K::Vintage}, {"Vinyl", K::Vinyl}, {"Tape Emulator", K::TapeEmu},
    // Melodee's own after the MPC's.
    {"Shimmer", K::Shimmer}, {"Ping Pong", K::PingPong}, {"Looper", K::Looper}, {"Chorus", K::Chorus},
    {"Pedal Chorus", K::PedalChorus}, {"Pedal Phaser", K::PedalPhaser}, {"Tremolo", K::Tremolo}, {"Drive", K::Drive},
    {"Tape", K::Tape}, {"Crush", K::Crush}, {"Spit", K::Spit}, {"Fuzz", K::Fuzz}, {"EQ", K::Eq}, {"Formant", K::Formant},
    {"Width", K::Width},
};
constexpr unsigned knob_mpc = 28;
} // namespace

unsigned knob_fx_count() { return sizeof(knob_entries) / sizeof(knob_entries[0]); }
const KnobFxEntry& knob_fx_entry(unsigned index) { return knob_entries[index < knob_fx_count() ? index : 0]; }
unsigned knob_fx_mpc_count() { return knob_mpc; }

const char* family_name(Family family) {
    static const char* names[] {"Space", "Time", "Motion", "Dirt", "Tone", "Dynamics", "Plug-ins"};
    static_assert(sizeof(names) / sizeof(names[0]) == size_t(Family::Count), "a name a family");
    return names[unsigned(family) % unsigned(Family::Count)];
}

unsigned family_effects(Family family, FamilyList& out) {
    unsigned n = 0;
    for (unsigned k = 1; k < unsigned(EffectKind::Count) && n < out.size(); ++k)
        if (effects[k].family == family) out[n++] = EffectKind(k);
    return n;
}

double delay_sync_quarters(int sync) {
    static const double quarters[] {0, 0.125, 1.0 / 6, 0.25, 0.375, 1.0 / 3, 0.5, 0.75, 2.0 / 3, 1, 1.5, 2, 3, 4};
    return sync > 0 && sync < int(sizeof(quarters) / sizeof(quarters[0])) ? quarters[sync] : 0;
}
int delay_sync_param(EffectKind kind) {
    switch (kind) {
    case EffectKind::Echo: case EffectKind::PingPong: return 4;
    case EffectKind::TapeEcho: case EffectKind::DiffDelay: return 5;
    default: return -1;
    }
}
const char* delay_sync_name(int sync) { return sync > 0 && sync < 14 ? delay_syncs[sync] : "Off"; }

} // namespace melodee
