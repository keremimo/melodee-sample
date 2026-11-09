// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/ui/paint.h"
#include <array>
#include <cstdint>

// A sample drawn as the boards draw it: thin bars from the centre line,
// one per column of its overview (512 peaks). `from`..`to` is the part of
// the sample in view (0..1); `keep_from`..`keep_to` is drawn in `colour`,
// the rest dim.
namespace melodee::ui {
void waveform(Paint& p, float x, float y, float w, float h, const std::array<uint8_t, 512>& overview,
              float from, float to, float keep_from, float keep_to, juce::Colour colour, float bar = 1.5f);
}
