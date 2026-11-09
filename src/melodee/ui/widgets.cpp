// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cmath>

namespace melodee::ui {

float page_dots(Paint& p, float x, float cy, int count, int at, juce::Colour c, bool right) {
    float total = 0;
    for (int n = 0; n < count; ++n) total += (n == at ? 7 : 3) + (n ? 2.5f : 0);
    float cursor = right ? x - total : x;
    for (int n = 0; n < count; ++n) {
        float w = n == at ? 7 : 3;
        p.fill(cursor, cy - 1.5f, w, 3, n == at ? c : colour::line, 1.5f);
        cursor += w + 2.5f;
    }
    return total;
}

void list_row(Paint& p, float x, float y, float w, const ListRow& row) {
    if (row.current) {
        p.fill(x, y, w, list_row_h, colour::raised, 4);
        p.g.saveState();
        p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), 3, int(list_row_h)));
        p.fill(x, y, 8, list_row_h, row.accent, 4);
        p.g.restoreState();
    }
    p.text(row.number, x + 9, y, 13, list_row_h, plain(colour::dim, 8, Weight::Medium));
    auto name = plain(row.current ? colour::text : row.dim ? colour::dim : colour::sec, type::list, Weight::Medium);
    p.text(row.name, x + 28, y, w - 90, list_row_h, name);
    float right = x + w - 7;
    if (row.starred) { star(p, right - 4.5f, y + list_row_h / 2, 4.5f, row.accent); right -= 17; }
    if (row.tag.isNotEmpty()) {
        auto style = caps_text(row.accent, 7);
        float tw = p.width(row.tag, style);
        p.text(row.tag, right - tw, y, tw + 1, list_row_h, style);
    }
}

void gauge(Paint& p, float x, float y, float w, float fill, juce::Colour c, float thickness) {
    p.fill(x, y, w, thickness, colour::line, thickness / 2);
    fill = std::clamp(fill, 0.0f, 1.0f);
    if (fill > 0) p.fill(x, y, w * fill, thickness, c, thickness / 2);
}

void sheet(Paint& p, const Sheet& s) {
    // At 2x: left 90, top 44 inside the picture, 460 wide, padding 22 x 24.
    constexpr float x = 45, y = zone::picture_y + 22, w = 230;
    auto body = plain(colour::sec, 11);
    float h = 11 + (s.power_icon ? 22 : 0) + 21 + 5 + 30 + 11;
    for (int n = 1; n <= 4; ++n) p.fill(x - n, y + n * 2, w + 2 * n, h, juce::Colours::black.withAlpha(0.06f), 6 + n); // a soft shadow
    p.fill(x, y, w, h, colour::surf, 6);
    p.ring(x, y, w, h, colour::line, 6);
    float at = y + 11;
    if (s.power_icon) {
        juce::Path icon;
        float cx = x + 12 + 8.5f, cy = at + 8.5f;
        icon.addCentredArc(cx, cy, 6.25f, 6.25f, 0, juce::MathConstants<float>::pi * 0.25f,
                           juce::MathConstants<float>::pi * 1.75f, true);
        icon.startNewSubPath(cx, cy - 7.5f); icon.lineTo(cx, cy - 1.5f);
        p.g.setColour(colour::text);
        p.g.strokePath(icon, juce::PathStrokeType(1.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        at += 22;
    }
    p.text(s.title, x + 12, at, w - 24, 21, plain(colour::text, type::title, Weight::SemiBold));
    at += 26;
    p.g.setColour(body.colour);
    p.g.setFont(font(body.size, body.weight));
    p.g.drawFittedText(s.body, juce::Rectangle<int>(int(x + 12), int(at), int(w - 24), 30), juce::Justification::topLeft, 2, 1.0f);
}

void menu_sheet(Paint& p, const MenuSheet& m) {
    // At 2x: left 130, top 96, 380 wide, padding 12, rows of 40 with 2 between.
    constexpr float x = 65, y = 48, w = 190, row_h = 20;
    float h = 6 + 14 + m.count * (row_h + 1) + 5;
    for (int n = 1; n <= 4; ++n) p.fill(x - n, y + n * 2, w + 2 * n, h, juce::Colours::black.withAlpha(0.06f), 6 + n);
    p.fill(x, y, w, h, colour::surf, 6);
    p.ring(x, y, w, h, colour::line, 6);
    p.text(m.title, x + 13, y + 6, w - 26, 10, caps_text(m.accent));
    p.text(m.detail, x + 13, y + 6, w - 26, 10, caps_text(colour::mid), juce::Justification::centredRight);
    for (unsigned n = 0; n < m.count; ++n) {
        const auto& r = m.rows[n];
        float ry = y + 20 + n * (row_h + 1), rx = x + 6, rw = w - 12;
        if (n == m.current) {
            p.fill(rx, ry, rw, row_h, colour::raised, 4);
            p.g.saveState();
            p.g.reduceClipRegion(juce::Rectangle<int>(int(rx), int(ry), 3, int(row_h)));
            p.fill(rx, ry, 8, row_h, m.accent, 4);
            p.g.restoreState();
        }
        p.text(r.name, rx + 7, ry, rw - 14, row_h, plain(r.off ? colour::dim : r.danger ? colour::rec : colour::text, 12, Weight::Medium));
        if (r.value.isNotEmpty())
            p.text(r.value, rx + 7, ry, rw - 14, row_h, plain(r.on ? m.accent : colour::mid, 11, Weight::Medium),
                   juce::Justification::centredRight);
    }
}

void wide_sheet(Paint& p, const MenuSheet& m, const juce::String& chip_text, juce::Colour chip_colour) {
    constexpr float x = zone::pad, y = zone::picture_y + 2, w = zone::width - 2 * zone::pad, h = 143;
    for (int n = 1; n <= 4; ++n) p.fill(x - n, y + n * 2, w + 2 * n, h, juce::Colours::black.withAlpha(0.06f), 6 + n);
    p.fill(x, y, w, h, colour::surf, 6);
    p.ring(x, y, w, h, colour::line, 6);
    auto chip = caps_text(colour::bg);
    float cw = p.width(chip_text, chip) + 9;
    p.fill(x + 13, y + 10, cw, 12.5f, chip_colour, 3);
    p.text(chip_text, x + 13, y + 10, cw, 12.5f, chip, juce::Justification::centred);
    float tw = p.text(m.title, x + 13 + cw + 5, y + 8, 80, 16, plain(colour::text, 11, Weight::SemiBold));
    p.text(m.detail, x + 13 + cw + 5 + tw + 6, y + 8, w, 16, caps_text(m.detail_colour, 7));
    float column = (w - 14) / 2, row_h = 25;
    for (unsigned n = 0; n < m.count; ++n) {
        const auto& r = m.rows[n];
        float rx = x + 7 + (n / 4) * column, ry = y + 31 + (n % 4) * row_h;
        bool current = n == m.current;
        if (current) {
            p.fill(rx, ry, column - 6, row_h - 3, colour::raised, 4);
            p.fill(rx + 6, ry + 6, 2, row_h - 15, m.accent, 1);
        }
        auto colour = r.off ? colour::dim : r.danger ? colour::rec : current ? colour::text : colour::sec;
        float nw = p.text(r.name, rx + 15, ry, column - 20, row_h - 3, plain(colour, 10.5f, Weight::Medium));
        if (r.value.isNotEmpty())
            p.text(r.value, rx + 15 + nw + 4, ry, column - nw - 24, row_h - 3, plain(colour::mid, 9));
    }
}

void star(Paint& p, float cx, float cy, float r, juce::Colour c) {
    juce::Path path;
    path.addStar({cx, cy}, 5, r * 0.45f, r, 0);
    p.g.setColour(c); p.g.fillPath(path);
}

} // namespace melodee::ui
