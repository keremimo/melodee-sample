// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/snapshot.h"
#include "melodee/ui/paint.h"

// The input level with the threshold line a take has to pass: Record and
// Input Config draw the same meter.
namespace melodee {

juce::String db_text(float db);              // "−30 DB"
float db_of(float level);                    // linear peak to dB, −90 for silence
// Draws INPUT LEVEL, PEAK, the bar (-96 to 0 dB, as the threshold runs) and
// STARTS AT across w from (x, y); about 31 high.
void input_meter(ui::Paint& p, const Snapshot& s, juce::Colour c, float x, float y, float w);

} // namespace melodee
