// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/ui/paint.h"
#include <array>

// The knob strip over K1-K3: three cards, each a capitals label in the
// knob's shade, the value, and a thin gauge. A card can be turning (tinted
// with a ring), waiting for pickup (value dim, a hollow tick where the pot
// sits) or idle (an empty quiet card).
namespace melodee::ui {

struct KnobCard {
    enum class State { Idle, Rest, Turning, Pickup };
    State state = State::Idle;
    juce::String label, value, unit;
    float fill = 0;        // 0..1 along the gauge
    bool bipolar = false;  // filled from the centre, with a centre mark
    float pot = -1;        // Pickup: where the pot sits, 0..1
    // The MPC's Shift icon: 1 Shift + this pot turns a second parameter,
    // 2 Shift is held and the card shows that parameter.
    uint8_t shift = 0;
    juce::Colour shade = colour::text, track = colour::text;
};
void knob_strip(Paint& p, const std::array<KnobCard, 3>& cards);
void shift_mark(Paint& p, float x, float y, juce::Colour c, bool filled); // the Shift icon, 7 x 8

} // namespace melodee::ui
