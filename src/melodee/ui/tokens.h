// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <cstdint>

// Melodee's design tokens at device size (the boards are drawn at 2x: halve
// them). NIGHT palette, one colour per track, and the four fixed zones of
// every screen.
namespace melodee::ui {

inline juce::Colour hex(uint32_t rgb, float alpha = 1) { return juce::Colour(0xff000000u | rgb).withAlpha(alpha); }

namespace colour {
inline const juce::Colour bg = hex(0x17141F), panel = hex(0x1D1926), surf = hex(0x221E2C), raised = hex(0x262033),
    quiet = hex(0x1A1722), line = hex(0x2E2939), text = hex(0xF3EEF8), sec = hex(0xC9C2D6), mid = hex(0x9A93A8),
    dim = hex(0x5A5468), rec = hex(0xFF4848), amber = hex(0xFFC145);
}

// Track colours 1-16 with the three knob shades: knob 1 is the track's own
// colour, knobs 2 and 3 turn 28 degrees each way round the hue
// (.local/melodee-design/gen/lib16.py).
constexpr std::array<std::array<uint32_t, 3>, 16> track_shades {{
    {0xFF7A5C, 0xFF6F93, 0xFF9A3C}, {0xFFC145, 0xFF9F5A, 0xE2D94A}, {0x46D9B0, 0x4FC7E0, 0x8FDB6A},
    {0x9D8CFF, 0xC587F2, 0x6FA0FF}, {0x4FB8FF, 0x4FFFF4, 0x4F66FF}, {0xFF6FA8, 0xFF6FEB, 0xFF796F},
    {0xA6E04A, 0xE0D44A, 0x60E04A}, {0xD77BFF, 0x997BFF, 0xFF7BE9}, {0x2FD0D6, 0x2FD68E, 0x2F82D6},
    {0xE8D94A, 0xE88F4A, 0xADE84A}, {0xFF5C7A, 0xFF5CC6, 0xFF8A5C}, {0x5FD97A, 0x7DD95F, 0x5FD9B3},
    {0x6F8CFF, 0x6FCFFF, 0x956FFF}, {0xFF9A3C, 0xFF3F3C, 0xFFF53C}, {0xF06FD8, 0xCC6FF0, 0xF06F9C},
    {0xB8C2D6, 0xB8D0D6, 0xBCB8D6},
}};
inline juce::Colour track_colour(unsigned track) { return hex(track_shades[track % 16][0]); }
inline juce::Colour knob_shade(unsigned track, unsigned knob) { return hex(track_shades[track % 16][knob % 3]); }

// Zones, top to bottom, at device size (2x on the boards: 44, 40, 296, 100).
namespace zone {
constexpr int width = 320, height = 240, pad = 6;
constexpr int soft_y = 0, soft_h = 22;
constexpr int header_y = 22, header_h = 20;
constexpr int picture_y = 42, picture_h = 148;
constexpr int knobs_y = 190, knobs_h = 50;
}

// Type sizes in device pixels (CSS px on the boards, halved).
namespace type {
constexpr float caps = 8, soft = 9, small = 7, body = 10, value = 16, title = 18, list = 12, bpm = 11;
constexpr float caps_tracking = 0.08f; // letter spacing of capitals, in em
}

// How strongly something sits behind an overlay: CSS opacity .35 over BG.
constexpr float behind_overlay = 0.65f;

} // namespace melodee::ui
