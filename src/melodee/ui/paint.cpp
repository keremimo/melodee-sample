// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/paint.h"
#include "melodee/ui/rubik_data.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace melodee::ui {
namespace {
// One typeface per weight, cloned from the variable font's wght axis.
const std::array<juce::Typeface::Ptr, 4>& faces() {
    static const std::array<juce::Typeface::Ptr, 4> loaded = [] {
        auto size = size_t(melodee_rubik_end - melodee_rubik_start);
        auto base = juce::Typeface::createSystemTypefaceFor(melodee_rubik_start, size);
        if (!base) throw std::runtime_error("Cannot load the embedded Rubik font");
        std::array<juce::Typeface::Ptr, 4> out;
        const float weights[] {400, 500, 600, 700};
        for (size_t n = 0; n < out.size(); ++n) {
            juce::FontVariableSetting setting {"wght", weights[n]};
            auto face = base->cloneWithVariableSettings(juce::Span<const juce::FontVariableSetting>(&setting, 1));
            out[n] = face ? face : base;
        }
        return out;
    }();
    return loaded;
}

juce::Font make_font(float size, Weight weight, bool caps) {
    // JUCE's kerning factor is a share of the font height (ascent + descent,
    // about 1.185 em for Rubik); capitals get type::caps_tracking.
    auto options = juce::FontOptions(faces()[size_t(weight)]).withPointHeight(size);
    if (caps) options = options.withKerningFactor(type::caps_tracking / 1.185f);
    return juce::Font(options);
}

// Text costs most of a frame: JUCE shapes each string with HarfBuzz, and its
// own layout cache compares whole Fonts (names, features, variations) on every
// lookup. These caches key on plain values instead. They hold fonts, string
// widths and justified layouts, are used from one thread at a time (the screen
// thread, or a host tool) and are dropped whole when they grow past `most`,
// since a page reuses the same few hundred strings frame after frame.
struct Style {
    int size_hundredths; Weight weight; bool caps;
    bool operator==(const Style& o) const { return size_hundredths == o.size_hundredths && weight == o.weight && caps == o.caps; }
    size_t hash() const { return size_t(size_hundredths) * 31u + size_t(weight) * 7u + size_t(caps); }
};
Style style_of(float size, Weight weight, bool caps) { return {int(std::lround(size * 100)), weight, caps}; }

struct FontHash { size_t operator()(const Style& s) const { return s.hash(); } };

struct WidthKey {
    juce::String text; Style style;
    bool operator==(const WidthKey& o) const { return style == o.style && text == o.text; }
};
struct WidthHash { size_t operator()(const WidthKey& k) const { return size_t(k.text.hashCode64()) ^ (k.style.hash() << 1); } };

struct LayoutKey {
    juce::String text; Style style; float w, h; int justify;
    bool operator==(const LayoutKey& o) const {
        return style == o.style && w == o.w && h == o.h && justify == o.justify && text == o.text;
    }
};
struct LayoutHash {
    size_t operator()(const LayoutKey& k) const {
        return size_t(k.text.hashCode64()) ^ (k.style.hash() << 1) ^ (std::hash<float>()(k.w) << 3)
             ^ (std::hash<float>()(k.h) << 5) ^ (size_t(k.justify) << 7);
    }
};

std::vector<TextIssue>* audit = nullptr;

template <typename Map> void bound(Map& map, size_t most) { if (map.size() >= most) map.clear(); }

const juce::Font& cached_font(float size, Weight weight, bool caps) {
    static std::unordered_map<Style, juce::Font, FontHash> fonts;
    auto key = style_of(size, weight, caps);
    auto found = fonts.find(key);
    if (found != fonts.end()) return found->second;
    bound(fonts, 256);
    return fonts.emplace(key, make_font(size, weight, caps)).first->second;
}

float cached_width(const juce::String& value, float size, Weight weight, bool caps) {
    static std::unordered_map<WidthKey, float, WidthHash> widths;
    WidthKey key {value, style_of(size, weight, caps)};
    auto found = widths.find(key);
    if (found != widths.end()) return found->second;
    bound(widths, 4096);
    float w = juce::GlyphArrangement::getStringWidth(cached_font(size, weight, caps), value);
    widths.emplace(std::move(key), w);
    return w;
}

// The layout juce::Graphics::drawText() builds, laid out once per string,
// style and box size.
const juce::GlyphArrangement& cached_layout(const juce::String& value, float size, Weight weight, bool caps,
                                            float w, float h, juce::Justification justify) {
    static std::unordered_map<LayoutKey, juce::GlyphArrangement, LayoutHash> layouts;
    LayoutKey key {value, style_of(size, weight, caps), w, h, justify.getFlags()};
    auto found = layouts.find(key);
    if (found != layouts.end()) return found->second;
    bound(layouts, 2048);
    juce::GlyphArrangement arrangement;
    arrangement.addCurtailedLineOfText(cached_font(size, weight, caps), value, 0, 0, w, false);
    arrangement.justifyGlyphs(0, arrangement.getNumGlyphs(), 0, 0, w, h, justify);
    return layouts.emplace(std::move(key), std::move(arrangement)).first->second;
}
} // namespace

juce::Font font(float size, Weight weight, bool caps) { return cached_font(std::max(size, type::floor), weight, caps); }

void audit_text(std::vector<TextIssue>* sink) { audit = sink; }
void report_text_issue(const TextIssue& issue) { if (audit) audit->push_back(issue); }

float Paint::width(const juce::String& value, const Text& style) const {
    return cached_width(value, std::max(style.size, type::floor), style.weight, style.caps);
}

Paint::Text Paint::fit(const juce::String& value, Text style, float w, float smallest) const {
    smallest = std::max(smallest, type::floor);
    style.size = std::max(style.size, type::floor);
    while (style.size > smallest && width(value, style) > w) style.size = std::max(smallest, style.size - 0.5f);
    return style;
}

juce::StringArray Paint::wrap(const juce::String& value, Text& style, float w, int most) const {
    juce::StringArray words;
    words.addTokens(value, " ", "");
    words.removeEmptyStrings();
    style.size = std::max(style.size, type::floor);
    for (;;) {
        juce::StringArray lines;
        for (const auto& word : words) {
            if (lines.isEmpty()) { lines.add(word); continue; }
            auto joined = lines[lines.size() - 1] + " " + word;
            if (width(joined, style) <= w || lines.size() >= most) lines.set(lines.size() - 1, joined);
            else lines.add(word);
        }
        bool fits = true;
        for (const auto& line : lines) fits = fits && width(line, style) <= w;
        if (fits || style.size <= type::floor) return lines;
        style.size = std::max(type::floor, style.size - 0.5f);
    }
}

float Paint::text(const juce::String& value, float x, float y, float w, float h, const Text& asked,
                  juce::Justification justify) {
    Text style = asked;
    style.size = std::max(style.size, type::floor);
    if (audit && value.isNotEmpty()) {
        float needed = width(value, style);
        if (asked.size < type::floor || needed > w + 0.5f) audit->push_back({value, asked.size, needed, w});
    }
    // Colour and font stay set on g, as drawText() callers expect.
    g.setColour(style.colour);
    g.setFont(cached_font(style.size, style.weight, style.caps));
    if (value.isEmpty()) return 0;
    juce::Rectangle<float> area(x, y, w, h);
    if (g.clipRegionIntersects(area.getSmallestIntegerContainer()))
        cached_layout(value, style.size, style.weight, style.caps, w, h, justify).draw(g, juce::AffineTransform::translation(x, y));
    return width(value, style);
}

void Paint::fill(float x, float y, float w, float h, juce::Colour c, float radius) {
    g.setColour(c);
    if (radius > 0) g.fillRoundedRectangle(x, y, w, h, radius);
    else g.fillRect(juce::Rectangle<float>(x, y, w, h));
}

void Paint::ring(float x, float y, float w, float h, juce::Colour c, float radius, float thickness) {
    g.setColour(c);
    float half = thickness / 2;
    g.drawRoundedRectangle(x + half, y + half, w - thickness, h - thickness, radius, thickness);
}

void Paint::circle(float cx, float cy, float r, juce::Colour c) {
    g.setColour(c);
    g.fillEllipse(cx - r, cy - r, 2 * r, 2 * r);
}

void Paint::dim_zone(int y, int h, float amount) { fill(0, float(y), float(zone::width), float(h), colour::bg.withAlpha(amount)); }

} // namespace melodee::ui
