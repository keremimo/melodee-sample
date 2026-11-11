// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <juce_graphics/juce_graphics.h>
#include "melodee/ui/shades.h"
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
    dim = hex(0x5A5468), rec = hex(0xFF4848), amber = hex(0xFFC145),
    play = hex(0x5FD97A); // the transport playing (the place bar), apart from any track's colour
}

// Track colours 1-16 with their knob shades, and the Looper's L1-L4: ui/shades.h (the pads use them too).
inline juce::Colour track_colour(unsigned track) { return hex(track_shades[track % 16][0]); }
inline juce::Colour knob_shade(unsigned track, unsigned knob) { return hex(track_shades[track % 16][knob % 3]); }
inline juce::Colour loop_colour(unsigned loop) { return hex(loop_shades[loop % 4][0]); }
inline juce::Colour loop_shade(unsigned loop, unsigned knob) { return hex(loop_shades[loop % 4][knob % 3]); }

// Zones, top to bottom, at device size.
namespace zone {
constexpr int width = 320, height = 240, pad = 6;
constexpr int soft_y = 0, soft_h = 22;
constexpr int header_y = 22, header_h = 22;
constexpr int picture_y = 44, picture_h = 144;
constexpr int knobs_y = 188, knobs_h = 52;
}

// Type sizes in device pixels. Readable at arm's length on the 320 x 240
// panel: nothing is drawn under `floor`, and Paint::fit never shrinks below
// it. Where a page runs out of room it shows fewer things at a time instead.
namespace type {
constexpr float floor = 11;
constexpr float caps = 11, soft = 11, small = 11, body = 13, value = 22, title = 20, list = 15, bpm = 15;
constexpr float caps_tracking = 0.06f; // letter spacing of capitals, in em
}

// How strongly something sits behind an overlay: CSS opacity .35 over BG.
constexpr float behind_overlay = 0.65f;

} // namespace melodee::ui
