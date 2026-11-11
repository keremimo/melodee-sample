// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// REC QUANTIZE (Shift + Pad 10): where Seq Record puts the notes played.
// On, each lands on the nearest step of the grid: the pattern's Q, or one of
// its own, 1/4 .. 1/64 with triplets (K1, the wheel or Minus / Plus); off,
// it keeps the time it was played. B3 turns it off and on and keeps the
// grid (its LED); B1 goes back. The pads play, so a take can start from here.
// The picture: the grid's name, whose grid it is (its own or the pattern's
// Q), and one beat with a tick at each place a note can land and a note
// played between them snapping to one. Kept with Melodee's settings.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    bool on = s.ui.settings.rec_quantize;
    unsigned grid = s.ui.settings.rec_grid;
    Division division = grid_division(grid, s.pattern.division);
    Header h = header_of(s);
    h.left.push_back(pill("REC QUANTIZE"));
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{"BACK", "", "REC Q"}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    keys.toggles = 4;
    keys.lit = on ? 4 : 0;
    frame(p, s, keys, h);

    float x = zone::pad + 2, y = zone::picture_y + 6, w = zone::width - 2 * zone::pad - 4;
    auto ink = on ? colour::text : colour::mid;
    p.text(division_name(division), x, y, 150, 40, plain(ink, 34, Weight::SemiBold));
    // Whose grid: its own or the pattern's Q, as a pair with the one in use filled.
    const juce::String names[] {"OWN GRID", "PATTERN Q"};
    float right = x + w;
    for (int n = 1; n >= 0; --n) {
        bool chosen = (grid != 0) == (n == 0);
        auto style = caps_text(chosen ? colour::bg : colour::mid);
        float pw = p.width(names[n], style) + 10;
        right -= pw;
        if (chosen) p.fill(right, y + 13, pw, 16, on ? colour::text : colour::mid, 3);
        else p.ring(right, y + 13, pw, 16, colour::line, 3);
        p.text(names[n], right, y + 13, pw, 16, style, juce::Justification::centred);
        right -= 4;
    }

    // One beat with a tick at each place a note can land; a note played between them, and (on) where it lands.
    unsigned beat = s.project.beat_ticks(), step = std::max(1u, division_ticks(division));
    float base = y + 106;
    p.fill(x, base, w, 1.5f, colour::line);
    for (unsigned t = 0; t <= beat; t += step) {
        float lx = x + w * float(std::min(t, beat)) / float(beat);
        bool edge = t == 0 || t >= beat;
        p.fill(std::clamp(lx - 0.75f, x, x + w - 1.5f), base - (edge ? 24 : 12), 1.5f, edge ? 24 : 12, edge ? colour::sec : on ? colour::mid : colour::dim);
    }
    float played = x + w * 0.37f;
    unsigned nearest = unsigned(std::lround(0.37 * beat / step)) * step;
    float lands = x + w * float(std::min(nearest, beat)) / float(beat);
    p.g.setColour(c);
    p.g.drawEllipse(played - 5, base - 40, 10, 10, 1.5f);
    if (on) {
        juce::Path swing;
        swing.startNewSubPath(played - 2, base - 30);
        swing.quadraticTo((played + lands) / 2 - 6, base - 18, lands + 2, base - 9);
        p.g.strokePath(swing, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        p.circle(lands, base - 4, 5.5f, c);
    }
    p.text("1 BEAT", x, base + 6, 80, 14, caps_text(colour::mid));
}

// The wheel, Minus / Plus: the pattern's Q, then 1/4 .. 1/64 (K1 the same).
void move(Context& c, int delta) {
    auto& settings = c.ui.settings;
    settings.rec_grid = uint8_t(std::clamp(int(settings.rec_grid) + delta, 0, int(Division::Count)));
    c.settings_save_at = std::max<uint32_t>(c.now, 1) + 1000; // the card gets the last of a turn
}

void soft(Context& c, int key) {
    if (key == 0) { c.back(); return; }
    if (key != 2) return;
    auto& settings = c.ui.settings;
    settings.rec_quantize = !settings.rec_quantize; // B3's LED says it
    c.settings_save_at = std::max<uint32_t>(c.now, 1);
}
} // namespace

const PageSpec rec_quantize_page {draw, play_pad, move, no_push, soft, move};

} // namespace melodee
