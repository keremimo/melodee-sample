// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/pages/signal_flow.h"

namespace melodee {
using namespace ui;

void signal_flow(Paint& p, Stop at, juce::Colour c, bool with_pad) {
    static const char* names[] {"PAD", "TRACK", "SENDS", "BUS", "MASTER"};
    float x = zone::pad, y = zone::picture_y + 1, h = 12;
    for (unsigned n = with_pad ? 0 : 1; n < 5; ++n) {
        if (n > (with_pad ? 0u : 1u)) {
            juce::Path arrow;
            float ay = y + h / 2;
            arrow.startNewSubPath(x, ay); arrow.lineTo(x + 6, ay);
            arrow.startNewSubPath(x + 4, ay - 2); arrow.lineTo(x + 6, ay); arrow.lineTo(x + 4, ay + 2);
            p.g.setColour(colour::dim);
            p.g.strokePath(arrow, juce::PathStrokeType(1, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            x += 9;
        }
        bool on = n == unsigned(at);
        auto style = caps_text(on ? colour::bg : colour::mid, 7);
        float w = p.width(names[n], style) + 9;
        if (on) p.fill(x, y, w, h, c, 3);
        p.text(names[n], x, y, w, h, style, juce::Justification::centred);
        x += w + 3;
    }
}

} // namespace melodee
