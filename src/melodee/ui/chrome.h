// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/ui/glyphs.h"
#include "melodee/ui/paint.h"
#include <array>
#include <utility>
#include <vector>

// The two rows at the top of every screen: the soft-key row naming B1-B3,
// and the header with transport, tempo and what the page is about (pills,
// chips, plain text and the track tag).
namespace melodee::ui {

enum class KeyIcon : uint8_t { None, Back, Check }; // drawn before a label: back out, or do it
struct SoftKeys {
    SoftKeys(std::array<juce::String, 3> labels, int active = -1, juce::Colour accent = colour::text, std::array<KeyIcon, 3> icons = {})
        : labels(std::move(labels)), active(active), accent(accent), icons(icons) {}
    std::array<juce::String, 3> labels;
    int active = -1;                    // the page's own key gets a short bar at the top edge
    uint8_t lit = 0;                    // toggles side by side: each key that is on, a bit each (B1 1, B2 2, B3 4)
    juce::Colour accent = colour::text; // the bar's colour, usually the track's
    std::array<KeyIcon, 3> icons {};
    // Stage: a button with a cycle of pages shows a dot for each under its label, the page in view lit.
    std::array<uint8_t, 3> dots {}, dot_at {};
    // Any glyph before a label (a key's icon wins), and toggles: a key in `toggles` (a bit each, as `lit`)
    // names a thing that is on or off and carries an LED after its label, lit while its `lit` bit is set,
    // instead of ON / OFF words. A toggle gets no bar at the top.
    std::array<Icon, 3> glyphs {};
    std::array<Icon, 3> after {};       // a glyph after a label: the chain next along, "SENDS >"
    uint8_t toggles = 0;
    std::array<juce::Colour, 3> leds {colour::play, colour::play, colour::play};
    // A key whose label and glyph are drawn in this colour (a destructive verb in REC); transparent: the usual.
    std::array<juce::Colour, 3> tints {};
};
void soft_keys(Paint& p, const SoftKeys& keys);

// Small drawn signs in place of words: an LED lit (on) or a ring (bypassed), the
// sidechain's pump (a quick dip and a slow climb, as the Compressor's graph draws
// it), and the wheel's push (a ring with a dot).
enum class Glyph : uint8_t { Led, LedOff, Duck, Push };
float glyph_width(Glyph g);
void draw_glyph(Paint& p, Glyph g, float x, float cy, juce::Colour c); // x its left edge, cy its middle

struct HeaderItem {
    enum class Kind { Pill, Chip, Text, Tag, Sign } kind = Kind::Pill;
    juce::String text;                 // pill, chip, text; the tag's sound name
    juce::Colour colour = colour::mid; // pill text, chip fill, text colour; the tag's track colour
    int track = 0;                     // Tag: the track number shown, 1-16
    juce::String engine;               // Tag: the engine name in capitals
    Glyph glyph = Glyph::Led;          // Sign: which, in `colour`
    Icon icon = Icon::None;            // Pill, Chip: a glyph before the text (or alone)
};
inline HeaderItem pill(juce::String text, juce::Colour c = colour::mid) { return {HeaderItem::Kind::Pill, text, c, 0, {}}; }
inline HeaderItem chip(juce::String text, juce::Colour c) { return {HeaderItem::Kind::Chip, text, c, 0, {}}; }
inline HeaderItem label(juce::String text, juce::Colour c = colour::sec) { return {HeaderItem::Kind::Text, text, c, 0, {}}; }
inline HeaderItem sign(Glyph g, juce::Colour c) { HeaderItem it {HeaderItem::Kind::Sign, {}, c, 0, {}}; it.glyph = g; return it; }
inline HeaderItem pill(Icon i, juce::String text, juce::Colour c = colour::mid) { auto it = pill(std::move(text), c); it.icon = i; return it; }
inline HeaderItem chip(Icon i, juce::String text, juce::Colour c) { auto it = chip(std::move(text), c); it.icon = i; return it; }
inline HeaderItem track_tag(int track, juce::String engine, juce::String name, juce::Colour c) {
    return {HeaderItem::Kind::Tag, name, c, track, engine};
}

// The two rows over every page (frame: two chips at most): transport (a play
// triangle, a stop square, the red dot while recording, a red ring while
// armed), tempo, at most two state chips, and on the right the track, which
// gives up its engine name, then its sound name, but never its number.
struct Header {
    bool playing = false, recording = false, armed = false;
    juce::String bpm = "120";
    std::vector<HeaderItem> left, right;
};
void header(Paint& p, const Header& h);

} // namespace melodee::ui
