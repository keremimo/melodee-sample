// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/chrome.h"
#include <algorithm>

namespace melodee::ui {
namespace {
constexpr float item_h = 15; // a pill or chip: capitals with 2 px above and below
constexpr float header_mid = zone::header_y + zone::header_h / 2.0f;

// Draws one header item with its left edge at x; returns its width.
float item(Paint& p, const HeaderItem& it, float x, bool measure_only) {
    using Kind = HeaderItem::Kind;
    float top = header_mid - item_h / 2;
    switch (it.kind) {
    case Kind::Pill:
    case Kind::Chip: {
        auto style = caps_text(it.kind == Kind::Pill ? it.colour : colour::bg);
        float w = p.width(it.text, style) + 8;
        if (!measure_only) {
            p.fill(x, top, w, item_h, it.kind == Kind::Pill ? colour::surf : it.colour, 3);
            p.text(it.text, x, top, w, item_h, style, juce::Justification::centred);
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
    }
    return 0;
}
} // namespace

void soft_keys(Paint& p, const SoftKeys& keys) {
    float column = zone::width / 3.0f;
    for (int n = 0; n < 3; ++n) {
        bool on = n == keys.active || ((keys.lit >> n) & 1);
        float x = n * column;
        if (on) {
            float cx = x + column / 2;
            p.fill(cx - 15, -2, 30, 4, keys.accent, 1.5f); // drawn from above the edge: only the lower corners show
        }
        const auto& text = keys.labels[size_t(n)];
        auto style = caps_text(on ? colour::text : colour::mid, type::soft);
        unsigned dots = keys.dots[size_t(n)];
        float label_y = dots > 1 ? zone::soft_y - 3.0f : float(zone::soft_y); // room under it for the dots
        if (text.isNotEmpty()) p.text(text, x, label_y, column, zone::soft_h, style, juce::Justification::centred);
        if (dots > 1) { // 3 px dots, 3 px apart, the page in view in the accent
            float dw = dots * 3.0f + (dots - 1) * 3.0f, dx = x + column / 2 - dw / 2;
            for (unsigned d = 0; d < dots; ++d) {
                auto c = on && d == keys.dot_at[size_t(n)] ? keys.accent : on ? colour::mid : colour::dim;
                p.fill(dx + d * 6.0f, zone::soft_h - 6.0f, 3, 3, c, 1.5f);
            }
        }
        auto icon = keys.icons[size_t(n)];
        if (icon == KeyIcon::None) continue;
        float left = x + column / 2 - p.width(text, style) / 2, mid = zone::soft_h / 2.0f;
        juce::Path path;
        if (icon == KeyIcon::Back) {
            float bx = left - 9;
            path.startNewSubPath(bx + 5, mid + 3); path.lineTo(bx + 5, mid - 3); path.lineTo(bx + 1, mid - 3);
            path.startNewSubPath(bx, mid - 3); path.lineTo(bx + 2, mid - 5);
            path.startNewSubPath(bx, mid - 3); path.lineTo(bx + 2, mid - 1);
        } else {
            float kx = left - 10;
            path.startNewSubPath(kx, mid); path.lineTo(kx + 2.5f, mid + 3); path.lineTo(kx + 7, mid - 4);
        }
        p.g.setColour(style.colour);
        p.g.strokePath(path, juce::PathStrokeType(1.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void header(Paint& p, const Header& h) {
    float x = zone::pad;
    // Transport: a play triangle while playing, a stop square otherwise.
    if (h.playing) {
        juce::Path tri;
        tri.addTriangle(x + 1, header_mid - 3.75f, x + 1, header_mid + 3.75f, x + 7.5f, header_mid);
        p.g.setColour(colour::text); p.g.fillPath(tri);
    } else {
        p.fill(x + 1, header_mid - 3, 6, 6, colour::mid, 1);
    }
    x += 13;
    x += p.text(h.bpm, x, zone::header_y, 50, zone::header_h, plain(colour::text, type::bpm, Weight::Medium)) + 5;
    if (h.recording) {
        p.circle(x + 3, header_mid, 3, colour::rec);
        x += 9;
        x += p.text("REC", x, zone::header_y, 34, zone::header_h, caps_text(colour::rec)) + 5;
    }
    float left_end = x;
    for (const auto& it : h.left) left_end += item(p, it, 0, true) + 4;
    auto right_items = h.right;
    auto right_width = [&] {
        float total = 0;
        for (const auto& it : right_items) total += item(p, it, 0, true) + 4;
        return total > 0 ? total - 4 : 0;
    };
    float right = zone::width - zone::pad, total = right_width();
    // When both sides do not fit, the track tag gives up its engine name
    // first, then its sound name, then goes (the page under it says which
    // track it is).
    if (right - total < left_end + 2)
        for (auto& it : right_items) if (it.kind == HeaderItem::Kind::Tag) it.engine = {};
    total = right_width();
    if (right - total < left_end + 2)
        for (auto& it : right_items) if (it.kind == HeaderItem::Kind::Tag) it.text = {};
    total = right_width();
    if (right - total < left_end + 2) // last, the tag itself: the page shows the track
        right_items.erase(std::remove_if(right_items.begin(), right_items.end(),
                                         [](const HeaderItem& it) { return it.kind == HeaderItem::Kind::Tag; }), right_items.end());
    total = right_width();
    if (right - total < left_end + 2) report_text_issue({"header", type::floor, left_end + total, right - zone::pad});
    for (const auto& it : h.left) x += item(p, it, x, false) + 4;
    float at = right - total;
    for (const auto& it : right_items) at += item(p, it, at, false) + 4;
}

} // namespace melodee::ui
