// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/snapshot.h"
#include "melodee/ui/glyphs.h"
#include "melodee/ui/paint.h"

// The input level with the threshold line a take has to pass: Record and
// Input Config draw the same meter.
namespace melodee {

juce::String db_text(float db);              // "−30 dB", "+6 dB"
float db_of(float level);                    // linear peak to dB, −90 for silence
// Draws the level bar (-96 to 0 dB, as the threshold runs) across w from
// (x, y) with the threshold a take passes as a marker and its level under
// it, or AT ONCE; about 32 high.
void input_meter(ui::Paint& p, const Snapshot& s, juce::Colour c, float x, float y, float w);
ui::Icon source_icon(InputSource source); // the mic, a jack, USB-C, the mix, a track's note (pages/input_config_page.cpp)

} // namespace melodee
