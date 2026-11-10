// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/ui/paint.h"

// The strip under the header on every effects page: where along the signal
// this page sits (TRACK, SENDS, BUS, MASTER; PAD joins it with the sampler).
namespace melodee {
enum class Stop : unsigned { Pad, Track, Sends, Bus, Master };
void signal_flow(ui::Paint& p, Stop at, juce::Colour colour, bool with_pad = false); // with_pad: a sampler track's
}
