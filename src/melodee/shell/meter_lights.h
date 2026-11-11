// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The level meters right of the screen (surface/leds.h: five LEDs a strip,
// green, green, green, yellow, red from the bottom): a level as the LEDs lit.
// The engine's levels already fall (EngineHost::out_level); this only places
// them on the steps. No LED is held: the red shows only while the level is up
// there (Kerem: a held red looked stuck).
#pragma once
#include "surface/leds.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace melodee {

using MeterSteps = std::array<float, surface_meter_segments>;
// dBFS at which each LED lights, bottom to top. The mix: set by Kerem playing a synth (median -22, loudest
// -9 dBFS) and the 909 kit (median -12 to -16, loudest -6): the synth in the greens with its loud notes yellow,
// the kit green and yellow with its loudest hits flashing red, whatever Main Volume is at. The input: red near
// full scale, where the converter clips.
constexpr MeterSteps meter_mix_db {-36, -27, -18, -11, -7};
constexpr MeterSteps meter_input_db {-42, -30, -18, -9, -2};

// The strip's LEDs for `level` (linear, 1 = full scale): a bar up to it.
inline uint8_t meter_lit(float level, const MeterSteps& steps) {
    float db = level > 0 ? 20 * std::log10(level) : -200.0f;
    uint8_t bits = 0;
    for (unsigned n = 0; n < surface_meter_segments; ++n)
        if (db >= steps[n]) bits |= uint8_t(1u << n);
    return bits;
}

} // namespace melodee
