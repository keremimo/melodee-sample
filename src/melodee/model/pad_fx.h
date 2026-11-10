// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <array>
#include <cstdint>

// Pad FX and Flex Beat settings kept with the project (parity phase 7b):
// K1-K3 of each of the MPC's sixteen Pad FX (their ranges and names in
// engine/pad_fx_info.cpp, whose initial values these match), and Flex Beat's
// mode, mix and quantize.
namespace melodee {

using PadFxParams = std::array<std::array<int16_t, 3>, 16>;
constexpr PadFxParams pad_fx_defaults {{
    {1, 100, 0}, {120, 60, 20}, {50, 70, 60}, {50, 3, 70}, {2, 0, 0}, {50, 3, 70}, {50, 3, 70}, {50, 3, 70},
    {180, 0, 0}, {8, 40, 0}, {0, 0, 0}, {40, 20, 60}, {4, 0, 0}, {2, 4, 0}, {6, 45, 0}, {20, 60, 70},
}};

struct FlexSettings {
    uint8_t mode = 0;       // K1: 0 One Shot, 1 Loop
    uint8_t mix = 100;      // K3: 0..100
    bool quantize = true;   // B3: effects start on the next sixteenth
};

} // namespace melodee
