// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
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
    juce::Colour accent = colour::text; // the bar's colour, usually the track's
    std::array<KeyIcon, 3> icons {};
};
void soft_keys(Paint& p, const SoftKeys& keys);

struct HeaderItem {
    enum class Kind { Pill, Chip, Text, Tag } kind = Kind::Pill;
    juce::String text;                 // pill, chip, text; the tag's sound name
    juce::Colour colour = colour::mid; // pill text, chip fill, text colour; the tag's track colour
    int track = 0;                     // Tag: the track number shown, 1-16
    juce::String engine;               // Tag: the engine name in capitals
};
inline HeaderItem pill(juce::String text, juce::Colour c = colour::mid) { return {HeaderItem::Kind::Pill, text, c, 0, {}}; }
inline HeaderItem chip(juce::String text, juce::Colour c) { return {HeaderItem::Kind::Chip, text, c, 0, {}}; }
inline HeaderItem label(juce::String text, juce::Colour c = colour::sec) { return {HeaderItem::Kind::Text, text, c, 0, {}}; }
inline HeaderItem track_tag(int track, juce::String engine, juce::String name, juce::Colour c) {
    return {HeaderItem::Kind::Tag, name, c, track, engine};
}

struct Header {
    bool playing = false, recording = false;
    juce::String bpm = "120";
    std::vector<HeaderItem> left, right;
};
void header(Paint& p, const Header& h);

} // namespace melodee::ui
