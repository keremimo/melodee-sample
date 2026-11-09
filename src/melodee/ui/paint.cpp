// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/paint.h"
#include "melodee/ui/rubik_data.h"
#include <algorithm>
#include <array>
#include <stdexcept>

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
} // namespace

juce::Font font(float size, Weight weight, bool caps) {
    // JUCE's kerning factor is a share of the font height (ascent + descent,
    // about 1.185 em for Rubik); the boards space capitals by 0.08 em.
    auto options = juce::FontOptions(faces()[size_t(weight)]).withPointHeight(size);
    if (caps) options = options.withKerningFactor(type::caps_tracking / 1.185f);
    return juce::Font(options);
}

float Paint::width(const juce::String& value, const Text& style) const {
    return juce::GlyphArrangement::getStringWidth(font(style.size, style.weight, style.caps), value);
}

Paint::Text Paint::fit(const juce::String& value, Text style, float w, float smallest) const {
    while (style.size > smallest && width(value, style) > w) style.size = std::max(smallest, style.size - 0.5f);
    return style;
}

float Paint::text(const juce::String& value, float x, float y, float w, float h, const Text& style,
                  juce::Justification justify) {
    g.setColour(style.colour);
    g.setFont(font(style.size, style.weight, style.caps));
    g.drawText(value, juce::Rectangle<float>(x, y, w, h), justify, false);
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
