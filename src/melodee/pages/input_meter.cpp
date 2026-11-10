// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/pages/input_meter.h"
#include "melodee/ui/tokens.h"
#include <algorithm>
#include <cmath>

namespace melodee {
using namespace ui;

float db_of(float level) { return level > 0 ? 20 * std::log10(level) : -90; }

juce::String db_text(float db) {
    int v = int(std::lround(db));
    return (v < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(v)) + " DB";
}

void input_meter(Paint& p, const Snapshot& s, juce::Colour c, float x, float y, float w) {
    p.text("INPUT LEVEL", x, y, 120, 9, caps_text(colour::mid, 7));
    p.text("PEAK " + db_text(std::max(-96.0f, db_of(s.take.level))), x, y, w, 9, caps_text(colour::mid, 7), juce::Justification::centredRight);
    auto fraction = [](float db) { return std::clamp((db + 96) / 96, 0.0f, 1.0f); };
    p.fill(x, y + 12, w, 6, colour::line, 3);
    p.fill(x, y + 12, w * fraction(db_of(s.take.level)), 6, c, 3);
    const auto& in = s.ui.settings.input;
    if (!in.at_once()) {
        float tx = x + w * fraction(float(in.threshold));
        p.fill(tx, y + 9, 1, 12, colour::text);
        p.text("STARTS AT " + db_text(float(in.threshold)), std::clamp(tx - 60, x, x + w - 120), y + 22, 120, 9, caps_text(c, 7), juce::Justification::centred);
    } else {
        p.text("STARTS AT ONCE", x, y + 22, w, 9, caps_text(c, 7), juce::Justification::centred);
    }
}

} // namespace melodee
