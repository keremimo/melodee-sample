// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A plug-in's own screen (sdk/melodee-screen/melodee_screen.h, docs/PLUGIN_SCREENS_PLAN.md) as the shell and the
// pages see it: the picture it drew last, what the page in view tells it, and the inputs handed to it. The rack's
// screen thread does the drawing (plugin/plugin_rack.h); the snapshot carries the newest picture to the pages, shared,
// never in UiState (plain values only). No JUCE here.
#pragma once
#include "melodee/model/limits.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace melodee {

constexpr unsigned picture_width = 320, picture_page_rows = 218, picture_strip_rows = 144;

struct ScreenPicture {
    uint8_t entry = no_plugin;     // whose (Project::plugins)
    uint8_t area = 0;              // 0 the page, 1 a strip (MELODEE_SCREEN_PAGE, _STRIP)
    uint32_t serial = 0;           // a new number each new picture
    uint16_t width = picture_width, height = picture_page_rows;
    std::vector<uint32_t> pixels;  // width x height, 0xffRRGGBB
};
using ScreenPicturePtr = std::shared_ptr<const ScreenPicture>;

// The page in view, as the shell tells the rack each tick: which entry's screen it shows (no_plugin none) and what the
// plug-in is told about it.
struct ScreenView {
    uint8_t entry = no_plugin;
    uint8_t area = 0;
    bool shift = false, playing = false;
    uint32_t colour = 0x808080;                 // the track's colour, 0xRRGGBB
    std::array<int16_t, 3> pots {-1, -1, -1};   // K1-K3: where each pot sits, 0..127, -1 not known
    std::array<uint8_t, 3> live {};             // and whether it drives its value yet
    bool operator==(const ScreenView& o) const {
        return entry == o.entry && area == o.area && shift == o.shift && playing == o.playing && colour == o.colour
            && pots == o.pots && live == o.live;
    }
    bool operator!=(const ScreenView& o) const { return !(*this == o); }
};

// An input on the page, numbered as MELODEE_SCREEN_WHEEL, _PUSH, _STEP, _MUTE and _ERASE are.
enum class ScreenKey : uint8_t { Wheel = 0, Push = 1, Step = 2, Mute = 3, Erase = 4 };
// The printed buttons a plug-in's page takes (its keys(), as MELODEE_SCREEN_KEY_MUTE and _ERASE are).
constexpr uint8_t screen_key_mute = 1, screen_key_erase = 2;
struct ScreenInput {
    ScreenKey key = ScreenKey::Wheel;
    int32_t value = 0;
    bool shift = false;
};

} // namespace melodee
