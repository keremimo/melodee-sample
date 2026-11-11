// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/ui/shades.h"
#include "surface/led_colours.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

// The pads' one grammar (docs/MELODEE.md, Pad lights). Every pad colour goes
// through here, as the screen's colours go through ui/tokens.h:
//   colour      whose pad it is: the track's (or a loop's) own; white the
//               panel's, or the one in your hand; red a loss or a recording
//   brightness  how much, on five rungs: Off, Faint, Dim, Mid, Full
//   motion      when: a blink waits for a boundary, a flare is a sound
//               fading, a pulse the beat
// led_colours.h squares a level before the 0-127 drive, so under about 0.2 a
// colour falls apart (0.07 drove tracks 1 and 2 as pure red, 0.04 drove
// nothing): Faint, 0.25, is the lowest level at which all sixteen track
// colours keep their hue.
namespace melodee::pad_light {

constexpr float off = 0, faint = 0.25f, dim = 0.4f, mid = 0.65f, full = 1.0f;
constexpr uint32_t blink_ms = 150; // a change waiting for the bar: a pattern, a track's next pattern
constexpr uint32_t flash_ms = 250; // waiting on you: a take's pad, the pad a question is about
constexpr uint32_t hand_ms = 250;  // a pad let go: how fast its flare fades

enum class Hue : uint8_t { None, Track, Loop, White, Red, Orange };

// A pad's light: the drive the LEDs get, and what it says (`status`, the tests).
struct Look {
    Rgb rgb {};
    uint32_t colour = 0; // 0xRRGGBB at full
    float level = 0;     // 0..1, a flare's included
    Hue hue = Hue::None;
    bool lit() const { return rgb.r || rgb.g || rgb.b; }
};

inline Look of(uint32_t colour, Hue hue, float level) {
    level = led_colour::clamp01(level);
    if (level <= 0) return {};
    Look out;
    out.rgb = led_colour::rgb(((colour >> 16) & 255) / 255.0f, ((colour >> 8) & 255) / 255.0f, (colour & 255) / 255.0f, level);
    out.colour = colour; out.level = level; out.hue = hue;
    return out;
}
inline Look track(unsigned t, float level) { return of(ui::track_shades[t % 16][0], Hue::Track, level); }
inline Look loop(unsigned l, float level) { return of(ui::loop_shades[l % 4][0], Hue::Loop, level); }
inline Look white(float level) { return of(0xFFFFFF, Hue::White, level); }
inline Look red(float level) { return of(0xFF0000, Hue::Red, level); }
inline Look orange(float level) { return of(0xFF7300, Hue::Orange, level); } // the Looper's overdub

inline float between(float a, float b, float t) { return a + (b - a) * led_colour::clamp01(t); }
// `a` moved `t` 0..1 of the way to `b` in drive values; it says what the nearer says.
inline Look blend(const Look& a, const Look& b, float t) {
    t = led_colour::clamp01(t);
    if (t <= 0) return a;
    if (t >= 1) return b;
    Look out = t < 0.5f ? a : b;
    out.rgb = mix_light(a.rgb, b.rgb, t);
    out.level = between(a.level, b.level, t);
    return out;
}
// A sound on the pad: `amount` 0..1 (its envelope) of the way to its colour at full, a little whiter.
inline Look flare(const Look& base, float amount) {
    if (amount <= 0.002f || base.hue == Hue::None) return base;
    Look peak = of(base.colour, base.hue, full);
    peak.rgb = mix_light(peak.rgb, led_colour::white(1), 0.45f);
    Look out = blend(base, peak, amount);
    out.hue = base.hue;
    return out;
}
// A recording on the pad: `amount` 0..1 of the way to red.
inline Look red_flare(const Look& base, float amount) { return amount <= 0.002f ? base : blend(base, red(full), amount); }
// The brighter of two looks, channel by channel; it says what the brighter says.
inline Look brighter(const Look& a, const Look& b) {
    Look out = b.level > a.level ? b : a;
    out.rgb = {std::max(a.rgb.r, b.rgb.r), std::max(a.rgb.g, b.rgb.g), std::max(a.rgb.b, b.rgb.b)};
    return out;
}
// On for `ms`, then off for `ms`.
inline bool blink(uint32_t now, uint32_t ms) { return (now / ms) % 2 == 0; }
// The rung a level is nearest: 0 off, 1 faint, 2 dim, 3 mid, 4 full.
inline unsigned rung(float level) {
    if (level <= 0) return 0;
    const float rungs[] {faint, dim, mid, full};
    unsigned best = 0;
    for (unsigned n = 1; n < 4; ++n)
        if (std::fabs(level - rungs[n]) < std::fabs(level - rungs[best])) best = n;
    return best + 1;
}
// One letter a hue, for `status`: - none, t track, l loop, w white, r red, o orange.
inline char hue_letter(Hue hue) { return "-tlwro"[unsigned(hue) % 6]; }

} // namespace melodee::pad_light
