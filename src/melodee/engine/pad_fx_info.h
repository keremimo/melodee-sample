// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_info.h"
#include "melodee/engine/punch.h"
#include <array>

// Parity phase 7b: what the user interface knows about the MPC's Pad FX
// (its sixteen, in pad order, with their K1-K3 as Akai's guide lists them)
// and Flex Beat's pads (pad 1 Empty, then fifteen time effects).
namespace melodee {

struct PadFxInfo {
    PunchKind kind;
    const char* name;
    std::array<ParamInfo, punch_params> params; // label nullptr: none there
};
const PadFxInfo& mpc_pad_fx(unsigned pad);          // pads 0..15
const PadFxInfo& pad_fx_of(PunchKind kind);         // an MPC kind's; others: no parameters
// Which punch a pad of the Pad FX page plays: banks 0 and 1 Melodee's own (and Flex Beat's four after Vinyl), bank 2
// the MPC's sixteen (Project::punch_bank keeps the bank; files from before phase 7b hold 0 or 1). -1: none on that pad.
constexpr unsigned punch_banks = 3;
int punch_on_pad(unsigned bank, unsigned pad);
// The sync speeds of Phaser, Comb and the filters' LFO, in bars; Beat Repeat's and Rev Stepper's divisions; Delay's times.
extern const double pad_fx_speeds[11];
extern const double pad_fx_divisions[8];
extern const double pad_fx_delay_times[17];

struct FlexInfo { const char* name; PunchKind kind; uint8_t grid; }; // grid: 0 1/4 .. 3 1/32 of a bar
constexpr unsigned flex_pads = 16;
const FlexInfo& flex_beat(unsigned pad); // pad 0 is Empty (its kind unused)

} // namespace melodee
