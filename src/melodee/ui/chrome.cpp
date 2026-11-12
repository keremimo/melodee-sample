// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/chrome.h"
#include <algorithm>

namespace melodee::ui {
namespace {
constexpr float item_h = 15; // a pill or chip: capitals with 2 px above and below
constexpr float header_mid = zone::header_y + zone::header_h / 2.0f;

} // namespace

float glyph_width(Glyph g) { return g == Glyph::Duck ? 16 : g == Glyph::Push ? 10 : 8; }

void draw_glyph(Paint& p, Glyph g, float x, float cy, juce::Colour c) {
    switch (g) {
    case Glyph::Led: p.circle(x + 4, cy, 3.5f, c); break;
    case Glyph::LedOff: p.g.setColour(c); p.g.drawEllipse(x + 0.75f, cy - 3.25f, 6.5f, 6.5f, 1.3f); break;
    case Glyph::Duck: { // level at the top, a quick dip, a slow climb back
        juce::Path path;
        float top = cy - 4.5f, foot = cy + 4.5f;
        path.startNewSubPath(x, top); path.lineTo(x + 4, top); path.lineTo(x + 4.5f, foot);
        path.cubicTo(x + 7.5f, foot, x + 9, top + 1, x + 16, top); // the slow climb of a release
        p.g.setColour(c); p.g.strokePath(path, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        break;
    }
    case Glyph::Push: p.g.setColour(c); p.g.drawEllipse(x + 0.75f, cy - 4.25f, 8.5f, 8.5f, 1.3f); p.circle(x + 5, cy, 1.8f, c); break;
    }
}

namespace {
// Draws one header item with its left edge at x; returns its width.
float item(Paint& p, const HeaderItem& it, float x, bool measure_only) {
    using Kind = HeaderItem::Kind;
    float top = header_mid - item_h / 2;
    switch (it.kind) {
    case Kind::Pill:
    case Kind::Chip: {
        auto style = caps_text(it.kind == Kind::Pill ? it.colour : colour::bg);
        bool glyph = it.icon != Icon::None;
        float tw = it.text.isEmpty() ? 0 : p.width(it.text, style);
        float gw = glyph ? 10 + (tw > 0 ? 3 : 0) : 0;
        float w = gw + tw + 8;
        if (!measure_only) {
            p.fill(x, top, w, item_h, it.kind == Kind::Pill ? colour::surf : it.colour, 3);
            if (glyph && it.icon == Icon::DeepSnow && it.kind == Kind::Chip) deep_snow(p, x + 4, header_mid - 5, 10, colour::bg, it.colour, 1.5f); // on the chip's fill
            else if (glyph) icon(p, it.icon, x + 4, header_mid - 5, 10, style.colour, 1.5f);
            if (tw > 0) p.text(it.text, x + 4 + gw, top, tw + 1, item_h, style);
        }
        return w;
    }
    case Kind::Text: {
        auto style = plain(it.colour, type::body, Weight::Medium);
        float w = p.width(it.text, style);
        if (!measure_only) p.text(it.text, x, top, w + 1, item_h, style);
        return w;
    }
    case Kind::Tag: {
        auto number = juce::String(it.track);
        auto engine = caps_text(it.colour), name = plain(colour::text, type::body, Weight::Medium);
        float ew = it.engine.isEmpty() ? 0 : p.width(it.engine, engine), nw = p.width(it.text, name);
        auto digits = plain(colour::bg, type::small, Weight::Bold);
        float box = std::max(item_h, p.width(number, digits) + 6);
        float w = box + (ew > 0 ? 4 + ew : 0) + (nw > 0 ? 4 + nw : 0);
        if (!measure_only) {
            p.fill(x, top, box, item_h, it.colour, 3);
            p.text(number, x, top, box, item_h, digits, juce::Justification::centred);
            float at = x + box + 4;
            if (ew > 0) { p.text(it.engine, at, top, ew + 1, item_h, engine); at += ew + 4; }
            if (nw > 0) p.text(it.text, at, top, nw + 1, item_h, name);
        }
        return w;
    }
    case Kind::Sign:
        if (!measure_only) draw_glyph(p, it.glyph, x, header_mid, it.colour);
        return glyph_width(it.glyph);
    }
    return 0;
}
} // namespace

void soft_keys(Paint& p, const SoftKeys& keys) {
    float column = zone::width / 3.0f;
    // One baseline for the row: when any key has page dots, every label sits up to leave them room.
    bool dotted = false;
    for (auto d : keys.dots) dotted = dotted || d > 1;
    float label_y = dotted ? zone::soft_y - 3.0f : float(zone::soft_y), mid = label_y + zone::soft_h / 2.0f;
    for (int n = 0; n < 3; ++n) {
        size_t k = size_t(n);
        bool toggle = (keys.toggles >> n) & 1, lit = (keys.lit >> n) & 1;
        bool on = n == keys.active || lit;
        float x = n * column, cx = x + column / 2;
        if (on && !toggle) p.fill(cx - 15, -2, 30, 4, keys.accent, 1.5f); // drawn from above the edge: only the lower corners show
        const auto& text = keys.labels[k];
        auto tint = keys.tints[k];
        auto style = caps_text(!tint.isTransparent() ? tint : on ? colour::text : colour::mid, type::soft);
        // The glyph: Back and Check from the key's icon, else any glyph the page gives it.
        Icon glyph = keys.icons[k] == KeyIcon::Back ? Icon::Back : keys.icons[k] == KeyIcon::Check ? Icon::Check : keys.glyphs[k];
        bool has_glyph = glyph != Icon::None;
        float tw = text.isEmpty() ? 0 : p.width(text, style);
        bool has_after = keys.after[k] != Icon::None;
        float gw = has_glyph ? 10.5f + (tw > 0 ? 3.5f : 0) : 0, lw = toggle ? 4 + 6 : 0, aw = has_after ? 3.5f + 10.5f : 0;
        float left = cx - (gw + tw + lw + aw) / 2;
        if (has_glyph) icon(p, glyph, left, mid - 5.5f, 10.5f, style.colour, 1.5f);
        if (tw > 0) p.text(text, left + gw, label_y, tw + 1, zone::soft_h, style);
        if (has_after) icon(p, keys.after[k], left + gw + tw + 3.5f, mid - 5.5f, 10.5f, style.colour, 1.5f);
        if (toggle) { // the LED: lit in its colour while on, a ring while off
            float lx = left + gw + tw + 4 + 3;
            if (lit) p.circle(lx, mid, 3, keys.leds[k]);
            else { p.g.setColour(colour::dim); p.g.drawEllipse(lx - 2.6f, mid - 2.6f, 5.2f, 5.2f, 1.1f); }
        }
        unsigned dots = keys.dots[k];
        if (dots > 1) { // 3 px dots, 3 px apart, the page in view in the accent
            float dw = dots * 3.0f + (dots - 1) * 3.0f, dx = cx - dw / 2;
            for (unsigned d = 0; d < dots; ++d) {
                auto c = on && d == keys.dot_at[k] ? keys.accent : on ? colour::mid : colour::dim;
                p.fill(dx + d * 6.0f, zone::soft_h - 6.0f, 3, 3, c, 1.5f);
            }
        }
    }
}

void header(Paint& p, const Header& h) {
    float x = zone::pad;
    // Transport: the red dot while recording (no word), a red ring while armed, a play triangle, a stop square.
    if (h.recording) {
        p.circle(x + 4, header_mid, 3.6f, colour::rec);
    } else if (h.armed) {
        p.g.setColour(colour::rec);
        p.g.drawEllipse(x + 0.6f, header_mid - 3.4f, 6.8f, 6.8f, 1.5f);
    } else if (h.playing) {
        juce::Path tri;
        tri.addTriangle(x + 1, header_mid - 3.75f, x + 1, header_mid + 3.75f, x + 7.5f, header_mid);
        p.g.setColour(colour::text); p.g.fillPath(tri);
    } else {
        p.fill(x + 1, header_mid - 3, 6, 6, colour::mid, 1);
    }
    x += 13;
    x += p.text(h.bpm, x, zone::header_y, 50, zone::header_h, plain(colour::text, type::bpm, Weight::Medium)) + 5;
    // Two chips at most: the state chips come first (header_of), a page's own after them.
    std::vector<HeaderItem> left(h.left.begin(), h.left.begin() + std::min<size_t>(h.left.size(), 2));
    float left_end = x;
    for (const auto& it : left) left_end += item(p, it, 0, true) + 4;
    auto right_items = h.right;
    auto right_width = [&] {
        float total = 0;
        for (const auto& it : right_items) total += item(p, it, 0, true) + 4;
        return total > 0 ? total - 4 : 0;
    };
    float right = zone::width - zone::pad, total = right_width();
    // When both sides do not fit, the track tag gives up its engine name
    // first, then its sound name; its numbered box always stays.
    if (right - total < left_end + 2)
        for (auto& it : right_items) if (it.kind == HeaderItem::Kind::Tag) it.engine = {};
    total = right_width();
    if (right - total < left_end + 2)
        for (auto& it : right_items) if (it.kind == HeaderItem::Kind::Tag) it.text = {};
    total = right_width();
    // Still too wide (a long folder or sample name): an item its page marked spare is left out, last first,
    // rather than run under the tag; what is shown is never cut.
    for (size_t n = left.size(); n-- > 0 && right - total < left_end + 2;)
        if (left[n].spare) { left_end -= item(p, left[n], 0, true) + 4; left.erase(left.begin() + std::ptrdiff_t(n)); }
    if (right - total < left_end + 2) report_text_issue({"header", type::floor, left_end + total, right - zone::pad});
    for (const auto& it : left) x += item(p, it, x, false) + 4;
    float at = right - total;
    for (const auto& it : right_items) at += item(p, it, at, false) + 4;
}

} // namespace melodee::ui
