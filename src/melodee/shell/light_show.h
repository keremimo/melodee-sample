// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "demos/light_show/light_show.h"
#include "surface/led_colours.h"
#include "surface/leds.h"
#include <array>
#include <cmath>
#include <cstdint>

// Light show (Sample + Mute): every light on the panel follows the tempo and
// the pads, for filming. The pads play as ever; only the lights change.
//   beat    the pads and fader pulse on each beat, harder on the bar's first;
//           the colours move on each bar; stopped, the same at half strength
//   bar     playing, the bank lights A-H step through the bar in eighths
//   taps    a pad press (or a sound starting on a pad) sends a ring out from
//           that pad over the whole panel: pads, buttons, fader, bank lights
//           and meters, in panel order
//   sound   a loud hit in the mix (any track) sends a softer ring from the
//           middle of the pads; the meters still show the mix
// Pure but for its rings: the light pass (shell/lights.cpp) and the tests
// drive it with times in ms.
namespace melodee {

struct ShowFrame {
    LightFrame lights;                               // pads, fader, bank lights, buttons
    std::array<uint8_t, surface_meter_lights> meters {};
};

struct ShowClock {
    uint32_t now = 0;        // ms
    double beats = 0;        // beats since Play (stopped: since any time at the tempo)
    unsigned bar_beats = 4;  // beats a bar
    bool playing = false;
    std::array<uint8_t, surface_meter_lights> meters {}; // the mix's meter lights (meter_lights.h)
};

namespace show_light {

struct Point { float x = 0, y = 0; };

// Where each light sits on the panel, in pad pitches: pad 1 at 0,0 and pad 16 at 3,3; the screen above the pads,
// the left buttons two columns left of them, the right buttons two columns right (docs/MELODEE.md, Light show).
inline Point pad_at(unsigned pad) { return {float(pad % 4), float(pad / 4)}; }
inline Point fader_at() { return {-1.9f, 1.6f}; }
inline Point bank_at(unsigned bank) { return {-2.7f + 0.2f * float(bank), 5.6f}; } // beside Main Volume
inline Point meter_at(unsigned side, unsigned step) { return {3.45f + 0.25f * float(side), 4.5f + 0.3f * float(step)}; }
inline Point button_at(SurfaceButton b) {
    using B = SurfaceButton;
    constexpr float l1 = -2.4f, l2 = -1.4f, r1 = 4.4f, r2 = 5.4f;
    switch (b) {
    case B::Sample: return {l1, 4.6f};       case B::Seq: return {l2, 4.6f};
    case B::PadFx: return {l1, 3.8f};        case B::KnobFx: return {l2, 3.8f};
    case B::Shift: return {l1, 3.0f};        case B::PadBank: return {l2, 3.0f};
    case B::Erase: return {l1, 0.3f};        case B::NoteRepeat: return {l2, 0.3f};
    case B::Chop: return {r1, 4.6f};         case B::Mute: return {r2, 4.6f};
    case B::Loop: return {r1, 3.8f};         case B::SixteenLevels: return {r2, 3.8f};
    case B::SampleSelect: return {r1, 3.0f}; case B::TapTempo: return {r2, 3.0f};
    case B::Minus: return {r1, 1.5f};        case B::Plus: return {r2, 1.5f};
    case B::SampleRecord: return {r1, 0.8f}; case B::SeqRecord: return {r2, 0.8f};
    case B::Stop: return {r1, 0.1f};         case B::Play: return {r2, 0.1f};
    default: return {1.5f, 5.2f};            // above the screen
    }
}

// Light 0..1 a channel, before the LEDs' squaring (led_colours.h).
struct Glow {
    float r = 0, g = 0, b = 0;
    void add(const Glow& o, float amount) { r += o.r * amount; g += o.g * amount; b += o.b * amount; }
    float peak() const { return std::fmax(r, std::fmax(g, b)); }
    Rgb rgb() const {
        auto drive = [](float x) { x = led_colour::clamp01(x); return uint8_t(std::lround(x * x * 127)); };
        return {drive(r), drive(g), drive(b)};
    }
};
inline Glow hue(float h, float whiten = 0) {
    h -= std::floor(h);
    float x = h * 6, f = x - std::floor(x);
    Glow c;
    switch (int(x) % 6) {
    case 0: c = {1, f, 0}; break;     case 1: c = {1 - f, 1, 0}; break; case 2: c = {0, 1, f}; break;
    case 3: c = {0, 1 - f, 1}; break; case 4: c = {f, 0, 1}; break;     default: c = {1, 0, 1 - f}; break;
    }
    whiten = led_colour::clamp01(whiten);
    c.r += (1 - c.r) * whiten; c.g += (1 - c.g) * whiten; c.b += (1 - c.b) * whiten;
    return c;
}

constexpr unsigned ring_ms = 1400;   // a ring's life
constexpr float ring_speed = 0.011f; // pad pitches a ms: the far side of the panel in about 0.7 s
constexpr float ring_width = 1.1f;   // pad pitches
constexpr unsigned flash_ms = 260;   // the pressed pad's own flash
constexpr unsigned intro_ms = 600;   // turned on: the lights come up

struct Ring {
    Point from;
    uint32_t at = 0;      // ms + 1; 0 none
    float strength = 0;   // 0..1
    float hue = 0;
    int pad = -1;         // the pad it left from, for its flash; -1 none
};

} // namespace show_light

class LightShow {
public:
    static constexpr unsigned rings = 24;

    void start(uint32_t now) {
        rings_ = {};
        started_ = now + 1;
        ring({1.5f, 1.5f}, now, 1.0f, -1, 0.5f); // a first ring from the middle as it comes on
    }
    // A pad pressed (or a sound starting on it), `strength` 0..1.
    void tap(unsigned pad, float strength, uint32_t now) {
        pad %= surface_pad_lights;
        ring(show_light::pad_at(pad), now, strength, int(pad), hue_now_ + 0.33f + 0.3f * float(pad) / 16.0f);
    }
    // A loud hit in the mix, `strength` 0..1: a softer ring from the middle, and a flash over the panel.
    void hit(float strength, uint32_t now) {
        ring({1.5f, 1.5f}, now, 0.55f * strength, -1, hue_now_ + 0.5f);
        hit_at_ = now + 1; hit_strength_ = strength;
    }
    // The newest ring's start, ms + 1 (0 none), for the tests.
    uint32_t newest() const { return rings_[(next_ + rings - 1) % rings].at; }

    ShowFrame frame(const ShowClock& clock) {
        using namespace show_light;
        ShowFrame out;
        const uint32_t now = clock.now;
        const unsigned bar_beats = clock.bar_beats ? clock.bar_beats : 4;
        const double bars = clock.beats / bar_beats;
        const float beat_in = float(clock.beats - std::floor(clock.beats));  // 0..1 through the beat
        const float bar_in = float(bars - std::floor(bars));                 // 0..1 through the bar
        const bool downbeat = unsigned(std::floor(clock.beats)) % bar_beats == 0;
        const float strength = clock.playing ? 1.0f : 0.5f;
        // The bar's colour: a step round the wheel each bar, glided over the bar's first beat.
        const float bar = float(std::floor(bars));
        const float glide = std::fmin(1.0f, float(bar_in * bar_beats));
        hue_now_ = 0.13f * (bar - 1) + 0.13f * glide * glide * (3 - 2 * glide);
        const float pulse = strength * (downbeat ? 1.0f : 0.6f) * std::exp(-beat_in * 5.0f);
        float hit = 0;
        if (hit_at_ && now + 1 >= hit_at_ && now + 1 - hit_at_ < 300) hit = hit_strength_ * (1 - float(now + 1 - hit_at_) / 300.0f);
        const float fade_in = started_ ? std::fmin(1.0f, float(now + 1 - started_) / intro_ms) : 1.0f;

        // The light at a point: the bar's colour across the panel, the beat's pulse, the rings passing.
        auto light = [&](Point p, float base) {
            Glow g;
            float across = (p.x + p.y) / 14.0f;                         // the colour leans across the panel
            float breathe = 0.5f + 0.5f * std::sin(6.2831853f * (float(clock.beats) / 8 + across)); // slow, every 8 beats
            g.add(hue(hue_now_ + across), base * (0.55f + 0.45f * breathe));
            g.add(hue(hue_now_ + across, downbeat ? 0.35f : 0.1f), pulse * base * 1.6f);
            if (hit > 0) g.add(hue(hue_now_ + 0.5f, 0.3f), hit * 0.5f);
            for (const auto& r : rings_) {
                if (!r.at || now + 1 < r.at) continue;
                unsigned age = now + 1 - r.at;
                if (age >= ring_ms) continue;
                float radius = ring_speed * float(age), dx = p.x - r.from.x, dy = p.y - r.from.y;
                float off = std::fabs(std::sqrt(dx * dx + dy * dy) - radius);
                if (off >= ring_width) continue;
                float amount = r.strength * (1 - off / ring_width) * (1 - float(age) / ring_ms);
                g.add(hue(r.hue + 0.04f * radius, 0.25f * (1 - off / ring_width)), amount * 1.3f);
            }
            g.r *= fade_in; g.g *= fade_in; g.b *= fade_in;
            return g;
        };

        auto& f = out.lights;
        for (unsigned pad = 0; pad < surface_pad_lights; ++pad) {
            Glow g = light(pad_at(pad), 0.3f);
            for (const auto& r : rings_) // the pressed pad flashes white, then into its ring's colour
                if (r.at && r.pad == int(pad) && now + 1 >= r.at && now + 1 - r.at < flash_ms) {
                    float t = float(now + 1 - r.at) / flash_ms;
                    g.add(hue(r.hue, 1 - t), r.strength * (1 - t) * 1.5f);
                }
            f.pads[pad] = g.rgb();
        }
        f.fader = light(fader_at(), 0.45f).rgb();

        // Buttons: white or red buttons go red on the bar's first beat while playing; else as bright as their light.
        const bool red_beat = clock.playing && downbeat && beat_in < 0.12f;
        for (auto b : panel_lit_buttons) {
            float peak = light(button_at(b), 0.25f).peak();
            auto kind = surface_button_lights[size_t(b)];
            ButtonLight level = peak > 0.55f ? ButtonLight::Bright : peak > 0.2f ? ButtonLight::Dim : ButtonLight::Off;
            if (red_beat && kind == ButtonLightKind::WhiteRed) level = ButtonLight::RedBright;
            if (b == SurfaceButton::Play && clock.playing) level = ButtonLight::Bright;
            if (b == SurfaceButton::TapTempo && beat_in < 0.25f) level = clock.playing ? ButtonLight::Bright : ButtonLight::Dim;
            f.buttons[size_t(b)] = level;
        }
        // Bank lights: playing, the bar in eighths with a short tail; the rings light them as they pass.
        unsigned eighth = unsigned(bar_in * surface_bank_lights) % surface_bank_lights;
        for (unsigned n = 0; n < surface_bank_lights; ++n) {
            bool step = clock.playing && fade_in >= 1 && (n == eighth || (pulse > 0.3f && n == (eighth + surface_bank_lights - 1) % surface_bank_lights));
            f.banks[n] = step || light(bank_at(n), 0).peak() > 0.45f;
        }
        // Meters: the mix as ever, and the rings climbing them.
        for (unsigned side = 0; side < surface_meter_lights; ++side) {
            uint8_t lit = clock.meters[side];
            for (unsigned s = 0; s < surface_meter_segments; ++s)
                if (light(meter_at(side, s), 0).peak() > 0.4f) lit |= uint8_t(1u << s);
            out.meters[side] = lit & surface_meter_all;
        }
        return out;
    }

private:
    void ring(show_light::Point from, uint32_t now, float strength, int pad, float h) {
        auto& r = rings_[next_];
        next_ = (next_ + 1) % rings;
        r = {from, now + 1, led_colour::clamp01(strength), h, pad};
    }
    std::array<show_light::Ring, rings> rings_ {};
    unsigned next_ = 0;
    uint32_t started_ = 0, hit_at_ = 0;
    float hit_strength_ = 0, hue_now_ = 0;
};

// Sends a show frame (SurfaceLights sends only what changed).
inline void show_frame(SurfaceLights& lights, const ShowFrame& frame) {
    show_light_frame(lights, frame.lights);
    for (unsigned side = 0; side < surface_meter_lights; ++side) lights.meter(side, frame.meters[side]);
}

} // namespace melodee
