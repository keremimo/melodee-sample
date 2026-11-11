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
        if (row.played >= 0) { // inside the row's rounded fill: the part heard, then its head
            float to = w * std::clamp(row.played, 0.0f, 1.0f);
            p.g.saveState();
            p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), int(std::lround(to)), int(list_row_h)));
            p.fill(x, y, w, list_row_h, row.accent.withAlpha(0.2f * std::clamp(row.played_wake, 0.0f, 1.0f)), 4);
            p.g.restoreState();
            if (row.played_head > 0)
                p.fill(std::clamp(x + to - 1, x, x + w - 1.5f), y, 1.5f, list_row_h, colour::text.withAlpha(0.8f * std::min(1.0f, row.played_head)));
        }
        p.g.saveState();
        p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), 3, int(list_row_h)));
        p.fill(x, y, 8, list_row_h, row.accent, 4);
        p.g.restoreState();
    }
    auto number = plain(colour::dim, type::small, Weight::Medium);
    float nx = x + 9, nw = row.number.isEmpty() ? 0 : std::max(16.0f, p.width(row.number, number));
    p.text(row.number, nx, y, nw, list_row_h, number);
    float right = x + w - 7;
    if (row.starred) { star(p, right - 5.5f, y + list_row_h / 2, 5.5f, row.accent); right -= 19; }
    if (row.tag_lead.isNotEmpty()) { // "1.2 / 3.1 S": the lead in the accent, the rest in mid
        auto lead = caps_text(row.accent), rest = caps_text(colour::mid);
        float lw = p.width(row.tag_lead, lead), tw = p.width(row.tag, rest);
        p.text(row.tag, right - tw, y, tw + 1, list_row_h, rest);
        p.text(row.tag_lead, right - tw - lw, y, lw + 1, list_row_h, lead);
        right -= lw + tw + 8;
    } else if (row.tag.isNotEmpty()) {
        auto style = caps_text(row.accent);
        float tw = p.width(row.tag, style);
        p.text(row.tag, right - tw, y, tw + 1, list_row_h, style);
        right -= tw + 8;
    }
    float name_x = nx + (nw > 0 ? nw + 6 : 0);
    auto name = plain(row.current ? colour::text : row.dim ? colour::dim : colour::sec, type::list, Weight::Medium);
    p.text(row.name, name_x, y, right - name_x, list_row_h, p.fit(row.name, name, right - name_x, type::floor));
}

void gauge(Paint& p, float x, float y, float w, float fill, juce::Colour c, float thickness) {
    p.fill(x, y, w, thickness, colour::line, thickness / 2);
    fill = std::clamp(fill, 0.0f, 1.0f);
    if (fill > 0) p.fill(x, y, w * fill, thickness, c, thickness / 2);
}

void sheet(Paint& p, const Sheet& s) {
    // A card 260 wide, centred over the picture, the body wrapping to three lines.
    constexpr float w = 260, x = (zone::width - w) / 2, pad = 12;
    auto body = plain(colour::sec, type::body);
    auto title = plain(colour::text, type::title, Weight::SemiBold);
    float line = body.size * 1.25f, text_w = w - 2 * pad;
    // The body wraps at the card's width, never cut: past three lines the
    // audit hears of it.
    auto body_font = font(body.size, body.weight);
    juce::GlyphArrangement wrapped;
    wrapped.addJustifiedText(body_font, s.body, 0, body_font.getAscent(), text_w, juce::Justification::topLeft,
                             line - body_font.getHeight());
    float body_h = wrapped.getNumGlyphs() > 0 ? wrapped.getBoundingBox(0, -1, true).getBottom() : 0;
    int lines = int(std::ceil((body_h - 1) / line));
    if (lines > 3) report_text_issue({s.body, body.size, body_h, 3 * line});
    float h = pad + (s.power_icon ? 22 : 0) + 24 + lines * line + pad;
    float y = std::max(float(zone::header_y), zone::picture_y + (zone::picture_h - h) / 2);
    for (int n = 1; n <= 4; ++n) p.fill(x - n, y + n * 2, w + 2 * n, h, juce::Colours::black.withAlpha(0.06f), 6 + n); // a soft shadow
    p.fill(x, y, w, h, colour::surf, 6);
    p.ring(x, y, w, h, colour::line, 6);
    float at = y + pad;
    if (s.power_icon) {
        juce::Path icon;
        float cx = x + pad + 8.5f, cy = at + 8.5f;
        icon.addCentredArc(cx, cy, 6.25f, 6.25f, 0, juce::MathConstants<float>::pi * 0.25f,
                           juce::MathConstants<float>::pi * 1.75f, true);
        icon.startNewSubPath(cx, cy - 7.5f); icon.lineTo(cx, cy - 1.5f);
        p.g.setColour(colour::text);
        p.g.strokePath(icon, juce::PathStrokeType(1.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        at += 22;
    }
    p.text(s.title, x + pad, at, text_w, 22, p.fit(s.title, title, text_w, type::body));
    at += 24;
    p.g.setColour(body.colour);
    wrapped.draw(p.g, juce::AffineTransform::translation(x + pad, at));
}

void menu_sheet(Paint& p, const MenuSheet& m) {
    // A card 240 wide over the picture, rows of 23 with 1 between; tall
    // menus rise into the header so every row shows.
    constexpr float w = 240, x = (zone::width - w) / 2, row_h = 23;
    float h = 6 + 18 + m.count * (row_h + 1) + 5;
    float y = std::min(float(zone::picture_y + 4), zone::height - 4 - h);
    for (int n = 1; n <= 4; ++n) p.fill(x - n, y + n * 2, w + 2 * n, h, juce::Colours::black.withAlpha(0.06f), 6 + n);
    p.fill(x, y, w, h, colour::surf, 6);
    p.ring(x, y, w, h, colour::line, 6);
    auto title = caps_text(m.accent), detail = caps_text(colour::mid);
    float dw = m.detail.isEmpty() ? 0 : p.width(m.detail, detail);
    p.text(m.title, x + 13, y + 6, w - 26 - (dw > 0 ? dw + 8 : 0), 14, title);
    if (dw > 0) p.text(m.detail, x + w - 13 - dw, y + 6, dw + 1, 14, detail);
    for (unsigned n = 0; n < m.count; ++n) {
        const auto& r = m.rows[n];
        float ry = y + 24 + n * (row_h + 1), rx = x + 6, rw = w - 12;
        if (n == m.current) {
            p.fill(rx, ry, rw, row_h, colour::raised, 4);
            p.g.saveState();
            p.g.reduceClipRegion(juce::Rectangle<int>(int(rx), int(ry), 3, int(row_h)));
            p.fill(rx, ry, 8, row_h, m.accent, 4);
            p.g.restoreState();
        }
        auto value = plain(r.on ? m.accent : colour::mid, type::body, Weight::Medium);
        float vw = r.value.isEmpty() ? 0 : p.width(r.value, value);
        if (vw > 0) p.text(r.value, rx + rw - 7 - vw, ry, vw + 1, row_h, value);
        float nw = rw - 14 - (vw > 0 ? vw + 8 : 0);
        auto name = plain(r.off ? colour::dim : r.danger ? colour::rec : colour::text, type::list, Weight::Medium);
        p.text(r.name, rx + 7, ry, nw, row_h, p.fit(r.name, name, nw, type::floor));
    }
}

void wide_sheet(Paint& p, const MenuSheet& m, const juce::String& chip_text, juce::Colour chip_colour) {
    constexpr float x = zone::pad, y = zone::header_y + 2, w = zone::width - 2 * zone::pad, h = zone::knobs_y - 2 - y;
    for (int n = 1; n <= 4; ++n) p.fill(x - n, y + n * 2, w + 2 * n, h, juce::Colours::black.withAlpha(0.06f), 6 + n);
    p.fill(x, y, w, h, colour::surf, 6);
    p.ring(x, y, w, h, colour::line, 6);
    auto chip = caps_text(colour::bg);
    float cw = p.width(chip_text, chip) + 10;
    p.fill(x + 10, y + 8, cw, 15, chip_colour, 3);
    p.text(chip_text, x + 10, y + 8, cw, 15, chip, juce::Justification::centred);
    auto title = plain(colour::text, type::body, Weight::SemiBold), detail = caps_text(m.detail_colour);
    float tx = x + 10 + cw + 6, dw = m.detail.isEmpty() ? 0 : p.width(m.detail, detail);
    float tw = p.text(m.title, tx, y + 7, x + w - 10 - tx - (dw > 0 ? dw + 6 : 0), 17, title);
    if (dw > 0) p.text(m.detail, tx + tw + 6, y + 7, dw + 1, 17, detail);
    float column = (w - 14) / 2, row_h = 30;
    for (unsigned n = 0; n < m.count; ++n) {
        const auto& r = m.rows[n];
        float rx = x + 7 + (n / 4) * column, ry = y + 29 + (n % 4) * row_h;
        bool current = n == m.current;
        if (current) {
            p.fill(rx, ry, column - 6, row_h - 3, colour::raised, 4);
            p.fill(rx + 6, ry + 7, 2, row_h - 17, m.accent, 1);
        }
        auto colour = r.off ? colour::dim : r.danger ? colour::rec : current ? colour::text : colour::sec;
        auto name = plain(colour, type::body, Weight::Medium), value = plain(colour::mid, type::body);
        float vw = r.value.isEmpty() ? 0 : p.width(r.value, value);
        float nw = column - 26 - (vw > 0 ? vw + 6 : 0);
        p.text(r.name, rx + 14, ry, nw, row_h - 3, p.fit(r.name, name, nw, type::floor));
        if (vw > 0) p.text(r.value, rx + column - 12 - vw, ry, vw + 1, row_h - 3, value);
    }
}

void star(Paint& p, float cx, float cy, float r, juce::Colour c) {
    juce::Path path;
    path.addStar({cx, cy}, 5, r * 0.45f, r, 0);
    p.g.setColour(c); p.g.fillPath(path);
}

} // namespace melodee::ui
