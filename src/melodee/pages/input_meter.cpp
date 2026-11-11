// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/pages/input_meter.h"
#include "melodee/ui/glyphs.h"
#include "melodee/ui/tokens.h"
#include <algorithm>
#include <cmath>

namespace melodee {
using namespace ui;

float db_of(float level) { return level > 0 ? 20 * std::log10(level) : -90; }

juce::String db_text(float db) {
    int v = int(std::lround(db));
    return (v < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : v > 0 ? juce::String("+") : juce::String()) + juce::String(std::abs(v)) + " dB";
}

void input_meter(Paint& p, const Snapshot& s, juce::Colour c, float x, float y, float w) {
    auto fraction = [](float db) { return std::clamp((db + 96) / 96, 0.0f, 1.0f); };
    float by = y + 5;
    for (int side = 0; side < 2; ++side) { // left over right
        float level = fraction(db_of(side ? s.take.right : s.take.left)), sy = by + side * 4.5f;
        p.fill(x, sy, w, 3.5f, colour::line, 1.75f);
        if (level > 0) p.fill(x, sy, w * level, 3.5f, c, 1.75f);
    }
    const auto& in = s.ui.settings.input;
    auto mark = plain(colour::amber, type::small, Weight::SemiBold);
    if (!in.at_once()) { // where a take starts: a tick through the bar, a caret under it and its level
        float tx = x + w * fraction(float(in.threshold));
        p.fill(tx - 0.75f, by - 4, 1.5f, 16, colour::amber);
        juce::Path caret;
        caret.addTriangle(tx - 4, by + 17, tx + 4, by + 17, tx, by + 12);
        p.g.setColour(colour::amber); p.g.fillPath(caret);
        auto text = db_text(float(in.threshold));
        float tw = p.width(text, mark);
        p.text(text, std::clamp(tx - tw / 2, x, x + w - tw), by + 17, tw + 1, 14, mark);
    } else { // at once: the play sign at the start of the bar
        icon(p, Icon::Play, x, by + 12, 11, colour::amber);
        p.text("AT ONCE", x + 14, by + 11, 80, 14, caps_text(colour::amber));
    }
}

} // namespace melodee
