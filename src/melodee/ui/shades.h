// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <array>
#include <cstdint>

// The colours Melodee gives its tracks and loops, as 0xRRGGBB: the screen's
// (ui/tokens.h) and the pads' (shell/pad_light.h). No JUCE here, so the
// pad lights build in the host tests too.
namespace melodee::ui {

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
// The sixteen by name, as Stage's COLOUR offers them to a kit pad (shell/pad_colour.h).
constexpr std::array<const char*, 16> track_colour_names {
    "Coral", "Amber", "Teal", "Lavender", "Sky", "Pink", "Lime", "Violet", "Cyan", "Yellow", "Rose", "Green", "Blue", "Orange", "Magenta", "Grey",
};
// The Looper's tracks L1-L4 (docs/LOOPER.md): colours of their own, apart from the sixteen, shaded the same way.
constexpr std::array<std::array<uint32_t, 3>, 4> loop_shades {{
    {0x6FD3FF, 0x6FA6FF, 0x6FFFE5}, {0xFFA36F, 0xFF7A6F, 0xFFD06F}, {0xC09CFF, 0x9C9EFF, 0xE69CFF}, {0x7DEBA6, 0x7DEBD9, 0xA4EB7D},
}};

} // namespace melodee::ui
