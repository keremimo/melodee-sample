// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <cstdint>

namespace melodee::ui {

// The boot animation, screen thread only. From black, the logo fades in
// from left to right; once it is whole, the backdrop fades in behind it;
// then the whole frame fades out over the app. Timed from the first draw.
// A touch (skip()) cuts it short: the frame as it stands fades out.
class BootAnimation {
public:
    static constexpr uint32_t black_ms = 200, wipe_ms = 1200, logo_hold_ms = 300, backdrop_ms = 900,
                              hold_ms = 900, out_ms = 350;
    static constexpr uint32_t out_at = black_ms + wipe_ms + logo_hold_ms + backdrop_ms + hold_ms;

    // `now` is any millisecond clock; the first call starts the animation.
    // True while the animation covers the whole screen, so the app need not
    // draw underneath; false once it fades out over the app, or has ended.
    bool covers(uint32_t now);
    bool finished(uint32_t now);
    void skip(uint32_t now);
    // Draws the frame for `now` over whatever the screen holds.
    void draw(juce::Graphics& g, uint32_t now);

private:
    bool started = false, loaded = false, broken = false;
    uint32_t start = 0, fade_from = out_at; // the elapsed time the fade out starts at
    juce::Image logo, backdrop, canvas;
    int logo_left = 0, logo_right = 0; // the columns the logo's pixels span
    int64_t composed = -1;             // the elapsed time `canvas` shows

    uint32_t elapsed(uint32_t now);
    void load();
    void compose(uint32_t at);
};

} // namespace melodee::ui
