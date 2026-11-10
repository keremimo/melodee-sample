// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/knobs.h"
#include <algorithm>

namespace melodee::ui {

// The Shift icon: an up arrow 7 px wide, hollow, or filled while Shift is held.
void shift_mark(Paint& p, float x, float y, juce::Colour c, bool filled) {
    juce::Path arrow;
    arrow.startNewSubPath(x + 3.5f, y + 0.5f);
    arrow.lineTo(x + 7, y + 4); arrow.lineTo(x + 5, y + 4); arrow.lineTo(x + 5, y + 7.5f);
    arrow.lineTo(x + 2, y + 7.5f); arrow.lineTo(x + 2, y + 4); arrow.lineTo(x, y + 4);
    arrow.closeSubPath();
    p.g.setColour(c);
    if (filled) p.g.fillPath(arrow);
    else p.g.strokePath(arrow, juce::PathStrokeType(1));
}

void knob_strip(Paint& p, const std::array<KnobCard, 3>& cards) {
    // 3 px above, 3 below, 6 px at the sides, 4 px between: 100 x 46 cards.
    constexpr float top = zone::knobs_y + 3, h = 46, gap = 4;
    constexpr float w = (zone::width - 2 * zone::pad - 2 * gap) / 3.0f;
    for (size_t n = 0; n < cards.size(); ++n) {
        const auto& c = cards[n];
        float x = zone::pad + n * (w + gap);
        using State = KnobCard::State;
        if (c.state == State::Idle) { p.fill(x, top, w, h, colour::quiet, 3); continue; }
        if (c.state == State::Turning) {
            p.fill(x, top, w, h, colour::bg, 3);
            p.fill(x, top, w, h, c.track.withAlpha(0x38 / 255.0f), 3);
            p.ring(x, top, w, h, c.shade, 3);
        } else {
            p.fill(x, top, w, h, colour::surf, 3);
        }
        float inner = x + 6, iw = w - 12;
        p.text(c.label, inner, top + 3, iw - (c.shift ? 9 : 0), 13, caps_text(c.shade));
        if (c.shift) shift_mark(p, x + w - 12, top + 5.5f, c.shift == 2 ? c.shade : colour::mid, c.shift == 2);
        auto value = plain(c.state == State::Pickup ? colour::dim : colour::text, type::value, Weight::Medium);
        auto unit = plain(colour::mid, type::body);
        float room = iw - (c.unit.isNotEmpty() ? p.width(c.unit, unit) + 3 : 0.0f);
        auto shown = p.fit(c.value, value, room, type::floor); // a long name shrinks to fit
        float vw = p.text(c.value, inner, top + 15, room, 22, shown);
        if (c.unit.isNotEmpty())
            p.text(c.unit, inner + vw + 3, top + 18, iw - vw - 3, 20, unit);
        // The gauge: a 2 px track with the value filled in the knob's shade.
        float gy = top + 40, fill = std::clamp(c.fill, 0.0f, 1.0f);
        p.fill(inner, gy, iw, 2, colour::line, 1);
        if (c.bipolar) {
            float from = std::min(0.5f, fill), to = std::max(0.5f, fill);
            if (to > from) p.fill(inner + from * iw, gy, (to - from) * iw, 2, c.shade, 1);
            p.fill(inner + iw / 2 - 0.5f, gy - 1.5f, 1, 5, colour::mid);
        } else if (fill > 0) {
            p.fill(inner, gy, fill * iw, 2, c.shade, 1);
        }
        if (c.state == State::Pickup && c.pot >= 0) {
            float cx = inner + std::clamp(c.pot, 0.0f, 1.0f) * iw;
            p.circle(cx, gy + 1, 3, colour::text);
            p.circle(cx, gy + 1, 2, colour::surf);
        }
    }
}

} // namespace melodee::ui
