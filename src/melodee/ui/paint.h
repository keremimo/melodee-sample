// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/ui/tokens.h"
#include <juce_graphics/juce_graphics.h>

// The drawing primitives every component uses: Rubik text at a weight, with
// the capitals' letter spacing, and filled rounded shapes. Screen thread (and
// the host preview tool) only.
namespace melodee::ui {

enum class Weight { Regular, Medium, SemiBold, Bold };

// Rubik, embedded in the binary, at its four weights (one variable font).
// Loaded on first use; throws when the embedded data cannot be read.
juce::Font font(float size, Weight weight = Weight::Regular, bool caps = false);

class Paint {
public:
    explicit Paint(juce::Graphics& graphics) : g(graphics) {}
    juce::Graphics& g;

    struct Text {
        float size = type::body;
        Weight weight = Weight::Regular;
        juce::Colour colour = colour::text;
        bool caps = false; // capitals: semibold weight is the caller's, tracking is added here
    };
    // Draws `value` in the box; returns the drawn width.
    float text(const juce::String& value, float x, float y, float w, float h, const Text& style,
               juce::Justification justify = juce::Justification::centredLeft);
    float width(const juce::String& value, const Text& style) const;
    // The style shrunk until `value` fits `w`, down to `smallest`: text is
    // never cut or ellipsised (the boards' rule).
    Text fit(const juce::String& value, Text style, float w, float smallest) const;

    void fill(float x, float y, float w, float h, juce::Colour c, float radius = 0);
    void ring(float x, float y, float w, float h, juce::Colour c, float radius, float thickness = 1);
    void circle(float cx, float cy, float r, juce::Colour c);
    // Lays BG over a zone, as if what is there had CSS opacity 1 - amount.
    void dim_zone(int y, int h, float amount = behind_overlay);
};

// Style shorthands that match the boards.
inline Paint::Text caps_text(juce::Colour c, float size = type::caps) { return {size, Weight::SemiBold, c, true}; }
inline Paint::Text plain(juce::Colour c, float size = type::body, Weight weight = Weight::Regular) { return {size, weight, c, false}; }

} // namespace melodee::ui
