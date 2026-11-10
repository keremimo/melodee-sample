// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/pad_fx_info.h"

namespace melodee {
namespace {
const char* const speeds[] {"2 bars", "1 bar", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/64"};
const char* const divisions[] {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/64"};
const char* const delay_times[] {"1/1", "1/2", "1/4D", "1/4", "1/4T", "1/8D", "1/8", "1/8T", "1/16D", "1/16", "1/16T",
                                 "1/32D", "1/32", "1/32T", "1/64D", "1/64", "1/64T"};
const char* const half_speeds[] {"x1.5", "x2", "x4"};
const char* const colors[] {"Cassette", "Flutter", "Tube Amp", "Vinyl", "Saturation", "Radio"};
const char* const delay_ranges[] {"Normal", "X-Feedback", "Ping-Pong"};
constexpr ParamInfo P(const char* label, int min, int max, int initial, Format format = Format::Percent, const char* const* choices = nullptr) {
    return {label, int16_t(min), int16_t(max), int16_t(initial), format, choices};
}
constexpr ParamInfo none {nullptr, 0, 0, 0, Format::Number, nullptr};
using K = PunchKind;
const PadFxInfo mpc[16] {
    {K::HalfSpeed, "Half Speed", {P("SPEED", 0, 2, 1, Format::Choice, half_speeds), P("MIX", 0, 100, 100), none}},
    {K::Chorus, "Chorus", {P("RATE", 40, 320, 120, Format::CentiHz), P("DEPTH", 0, 100, 60), P("FEEDBACK", 0, 100, 20)}},
    {K::Flanger, "Flanger", {P("RATE", 2, 1000, 50, Format::CentiHz), P("DEPTH", 0, 100, 70), P("FEEDBACK", 0, 100, 60)}},
    {K::Phaser, "Phaser", {P("FEEDBACK", 0, 100, 50), P("SPEED", 0, 10, 3, Format::Choice, speeds), P("RANGE", 0, 100, 70)}},
    {K::Comb, "Comb Filter", {P("SPEED", 0, 10, 2, Format::Choice, speeds), none, none}},
    {K::LpLfo, "LP Filter", {P("RESONANCE", 0, 100, 50), P("SPEED", 0, 10, 3, Format::Choice, speeds), P("RANGE", 0, 100, 70)}},
    {K::HpLfo, "HP Filter", {P("RESONANCE", 0, 100, 50), P("SPEED", 0, 10, 3, Format::Choice, speeds), P("RANGE", 0, 100, 70)}},
    {K::BpLfo, "BP Filter", {P("RESONANCE", 0, 100, 50), P("SPEED", 0, 10, 3, Format::Choice, speeds), P("RANGE", 0, 100, 70)}},
    {K::RingMod, "Ring Mod", {P("MAX FREQ", 40, 400, 180, Format::Hertz), none, none}},
    {K::LoFi, "LoFi", {P("BITCRUSH", 2, 24, 8, Format::Number), P("DECIMATOR", 0, 100, 40), none}},
    {K::Color, "Color", {P("MODE", 0, 5, 0, Format::Choice, colors), none, none}},
    {K::Granulator, "Granulator", {P("DENSITY", 1, 300, 40, Format::Number), P("FEEDBACK", 0, 100, 20), P("GRAIN LEN", 10, 200, 60, Format::Ms)}},
    {K::BeatRepeat, "Beat Repeat", {P("DIVISION", 0, 7, 4, Format::Choice, divisions), P("REVERSE", 0, 1, 0, Format::Switch), P("RESONANCE", 0, 100, 0)}},
    {K::RevStepper, "Rev Stepper", {P("DELAY TIME", 0, 7, 2, Format::Choice, divisions), P("REPEATS", 2, 8, 4, Format::Number), none}},
    {K::Delay, "Delay", {P("TIME", 0, 16, 6, Format::Choice, delay_times), P("FEEDBACK", 0, 100, 45), P("RANGE", 0, 2, 0, Format::Choice, delay_ranges)}},
    {K::Reverb, "Reverb", {P("PRE-DELAY", 0, 250, 20, Format::Ms), P("DECAY", 0, 100, 60), P("DIFFUSION", 0, 100, 70)}},
};
const PadFxInfo plain {K::Loop4, "", {none, none, none}};

const FlexInfo flex[flex_pads] {
    {"Empty", K::Loop4, 0},
    {"Tape Stop", K::Stop, 1}, {"Backspin", K::Backspin, 1}, {"Half Speed", K::Half, 2}, {"Reverse", K::Reverse, 2},
    {"Scratch", K::Scratch, 2}, {"Stutter", K::Stutter, 2}, {"Repeat 1/4", K::Loop4, 0}, {"Repeat 1/8", K::Loop8, 1},
    {"Repeat 1/16", K::Loop16, 2}, {"Gate 1/16", K::Gate, 2}, {"Trance Gate", K::TranceGate, 2}, {"Shuffle", K::Shuffle, 2},
    {"Pitch Down", K::Downer, 1}, {"Pitch Up", K::Upper, 2}, {"Pump", K::Pump, 0},
};
} // namespace

const double pad_fx_speeds[11] {2, 1, 0.5, 0.25, 1.0 / 6, 0.125, 1.0 / 12, 1.0 / 16, 1.0 / 24, 1.0 / 32, 1.0 / 64};
const double pad_fx_divisions[8] {0.25, 1.0 / 6, 0.125, 1.0 / 12, 1.0 / 16, 1.0 / 24, 1.0 / 32, 1.0 / 64};
const double pad_fx_delay_times[17] {1, 0.5, 0.375, 0.25, 1.0 / 6, 0.1875, 0.125, 1.0 / 12, 0.09375, 0.0625, 1.0 / 24,
                                     0.046875, 0.03125, 1.0 / 48, 0.0234375, 0.015625, 1.0 / 96};

const PadFxInfo& mpc_pad_fx(unsigned pad) { return mpc[pad % 16]; }
const PadFxInfo& pad_fx_of(PunchKind kind) { return punch_is_mpc(kind) ? mpc[unsigned(kind) - punch_mpc_first] : plain; }

int punch_on_pad(unsigned bank, unsigned pad) {
    pad %= 16;
    switch (bank % punch_banks) {
    case 0: return int(pad);
    case 1: return pad < 11 ? int(16 + pad) : pad < 15 ? int(unsigned(PunchKind::Backspin) + pad - 11) : -1;
    default: return int(punch_mpc_first + pad);
    }
}

const FlexInfo& flex_beat(unsigned pad) { return flex[pad % flex_pads]; }

} // namespace melodee
