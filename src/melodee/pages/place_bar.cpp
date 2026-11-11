// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The place bar (shell/place_bar.h) on every screen, last over the page and
// its sheets: 2 px between the soft keys and the header (rows 21-22), or
// under the knob cards (rows 238-239). Segments 2 px apart; the one it is in
// a shade lighter, with 1 px notches at its beats (or bars); the fill in the
// tone's colour; a 2 px leading edge in the text colour, a row taller for
// the first moment of each beat, toward the middle of the screen. Only the
// columns the edge moved change, so the panel sends a sliver a frame.
#include "melodee/pages/page.h"
#include "melodee/shell/place_bar.h"
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

juce::Colour tone_colour(PlaceBar::Tone t) {
    switch (t) {
    case PlaceBar::Tone::Playing: return colour::play;
    case PlaceBar::Tone::Recording: return colour::rec;
    case PlaceBar::Tone::Waiting: return colour::amber;
    case PlaceBar::Tone::Stopped: break;
    }
    return colour::mid;
}
} // namespace

void draw_place_bar(Paint& p, const Snapshot& s) {
    const PlaceBar bar = place_bar_of(s);
    bool top = !s.ui.settings.place_bottom;
    constexpr int x0 = zone::pad, w = zone::width - 2 * zone::pad, h = 2, gap = 2;
    const int y = top ? zone::soft_h - 1 : zone::height - h; // rows 21-22, or 238-239
    p.fill(0, top ? y : y - 1, zone::width, h + 1, colour::bg); // the rows it and its tick take
    auto colour = tone_colour(bar.tone);
    if (bar.blocks) { // Count-In: the beats still to come, lit
        int bw = (w - gap * int(bar.blocks - 1)) / int(bar.blocks);
        for (unsigned n = 0; n < bar.blocks; ++n)
            p.fill(float(x0 + int(n) * (bw + gap)), float(y), float(bw), h, n < bar.blocks_left ? colour : colour::line, 1);
        return;
    }
    uint32_t total = std::max<uint32_t>(bar.total(), 1);
    int usable = w - gap * int(bar.count - 1);
    uint32_t before = 0;
    int left = x0;
    for (unsigned n = 0; n < bar.count; ++n) {
        uint32_t seg = std::max<uint32_t>(bar.segments[n], 1);
        int right = x0 + int(std::lround(double(usable) * (before + seg) / total)) + int(n) * gap; // whole pixels: crisp edges
        int sw = std::max(right - left, 1);
        bool in = bar.position >= before && (bar.position < before + seg || n + 1 == bar.count);
        float played = std::clamp(float(bar.position - std::min(bar.position, before)) / float(seg), 0.0f, 1.0f);
        p.fill(float(left), float(y), float(sw), h, in && !bar.held ? colour::dim : colour::line, 1);
        if (played > 0) p.fill(float(left), float(y), std::round(sw * played), h, colour, 1);
        if (in && bar.notch && bar.notch < seg)
            for (uint32_t at = bar.notch; at < seg; at += bar.notch) p.fill(std::round(left + float(sw) * at / seg), float(y), 1, h, colour::bg);
        if (in) { // the leading edge: still while held, else a row taller as each beat starts
            float ex = std::round(left + sw * played) - 1;
            bool tick = !bar.held && (bar.position % std::max<uint32_t>(bar.beat, 1)) * 6 < bar.beat;
            p.fill(ex, float(top || !tick ? y : y - 1), 2, float(h + (tick ? 1 : 0)), bar.held ? colour::sec : colour::text);
        }
        before += seg;
        left = right + gap;
    }
}

} // namespace melodee
