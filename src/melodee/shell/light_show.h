// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "demos/light_show/light_show.h"
#include "surface/led_colours.h"
#include "surface/leds.h"
#include <array>
#include <cmath>
#include <cstdint>

// Light show (Sample + Mute; Sample + Minus / Plus picks the style), for
// filming. The pads play as ever; only the lights change. Every style keeps
// time with the tempo (stopped: at the tempo, half as bright), changes colour
// every four bars, and flashes a pressed pad white into the style's colour.
//   Pulse   the whole panel on the beat, harder on beat 1; buttons red on beat 1
//   Sweep   a bar of light across the panel, left to right and back, a beat each way
//   Chase   one pad a sixteenth, 1 to 16 through the bar; the buttons round the panel in eighths
//   Ripple  rings from each pad pressed (and from the middle on beat 1) over the whole panel
//   Levels  the pads, buttons and bank lights as a level meter of the mix
// Bank lights A-H step through the bar in eighths (Levels: the level); the
// meters show the mix. Pure but for its rings and the mix's held level: the
// light pass (shell/lights.cpp) and the tests drive it with times in ms.
namespace melodee {

enum class ShowStyle : uint8_t { Pulse, Sweep, Chase, Ripple, Levels, Count };
constexpr const char* show_style_names[] {"Pulse", "Sweep", "Chase", "Ripple", "Levels"};

struct ShowFrame {
    LightFrame lights;                               // pads, fader, bank lights, buttons
    std::array<uint8_t, surface_meter_lights> meters {};
};

struct ShowClock {
    uint32_t now = 0;        // ms
    double beats = 0;        // beats since Play (stopped: since any time at the tempo)
    unsigned bar_beats = 4;  // beats a bar
    bool playing = false;
    float level = 0;         // the mix, linear (1 full scale)
    std::array<uint8_t, surface_meter_lights> meters {}; // the mix's meter lights (meter_lights.h)
};

namespace show_light {

struct Point { float x = 0, y = 0; };

// Where each light sits on the panel, in pad pitches: pad 1 at 0,0 and pad 16 at 3,3; the screen above the pads,
// the left buttons two columns left of them, the right buttons two columns right.
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
constexpr float panel_left = -2.6f, panel_right = 5.6f;

// The lit buttons round the panel, clockwise from Erase: up the left, across the top, down the right.
constexpr std::array<SurfaceButton, 17> round_panel {{
    SurfaceButton::Erase, SurfaceButton::NoteRepeat, SurfaceButton::PadBank, SurfaceButton::PadFx, SurfaceButton::Sample,
    SurfaceButton::Seq, SurfaceButton::KnobFx, SurfaceButton::Chop, SurfaceButton::Mute, SurfaceButton::SixteenLevels,
    SurfaceButton::Loop, SurfaceButton::SampleSelect, SurfaceButton::TapTempo, SurfaceButton::SeqRecord,
    SurfaceButton::SampleRecord, SurfaceButton::Play, SurfaceButton::Stop,
}};

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
inline ButtonLight button_level(float peak, float dim = 0.2f, float bright = 0.6f) {
    return peak >= bright ? ButtonLight::Bright : peak >= dim ? ButtonLight::Dim : ButtonLight::Off;
}

constexpr unsigned ring_ms = 1200;   // a ring's life
constexpr float ring_speed = 0.009f; // pad pitches a ms: the far side of the panel in about 0.9 s
constexpr float ring_width = 1.0f;   // pad pitches
constexpr unsigned flash_ms = 300;   // a pressed pad's flash
constexpr unsigned intro_ms = 600;   // turned on: the lights come up
constexpr unsigned colour_bars = 4;  // the colour changes every four bars

struct Ring {
    Point from;
    uint32_t at = 0;      // ms + 1; 0 none
    float strength = 0;   // 0..1
};

} // namespace show_light

class LightShow {
public:
    static constexpr unsigned rings = 12;

    void start(uint32_t now) {
        rings_ = {}; flashes_ = {};
        started_ = now + 1;
        held_level_ = 0;
    }
    // A pad pressed by hand, `strength` 0..1: its flash; in Ripple a ring from it too.
    void tap(unsigned pad, float strength, uint32_t now, ShowStyle style) {
        pad %= surface_pad_lights;
        flashes_[pad] = now + 1; flash_strength_[pad] = led_colour::clamp01(strength);
        if (style == ShowStyle::Ripple) ring(show_light::pad_at(pad), now, strength);
    }
    // A sound starting on a pad (the sequence, Note Repeat): its flash alone, softer.
    void play(unsigned pad, float strength, uint32_t now) {
        pad %= surface_pad_lights;
        flashes_[pad] = now + 1; flash_strength_[pad] = 0.6f * led_colour::clamp01(strength);
    }
    // The newest ring's start, ms + 1 (0 none), for the tests.
    uint32_t newest() const { return rings_[(next_ + rings - 1) % rings].at; }

    ShowFrame frame(const ShowClock& clock, ShowStyle style) {
        using namespace show_light;
        ShowFrame out;
        auto& f = out.lights;
        const uint32_t now = clock.now;
        const unsigned bar_beats = clock.bar_beats ? clock.bar_beats : 4;
        const double bars = clock.beats / bar_beats;
        const unsigned beat = unsigned(std::floor(clock.beats));
        const float beat_in = float(clock.beats - std::floor(clock.beats)); // 0..1 through the beat
        const float bar_in = float(bars - std::floor(bars));                // 0..1 through the bar
        const bool downbeat = beat % bar_beats == 0;
        const float strength = clock.playing ? 1.0f : 0.5f;
        const float fade_in = started_ ? std::fmin(1.0f, float(now + 1 - started_) / intro_ms) : 1.0f;
        const float scale = strength * fade_in;
        // The colour: a sixth of the way round the wheel every four bars, glided over the first beat of them.
        {
            double blocks = bars / colour_bars;
            float step = float(std::floor(blocks)), in = float((blocks - step) * colour_bars * bar_beats);
            float glide = std::fmin(1.0f, in);
            hue_ = 0.55f + (step - 1 + glide * glide * (3 - 2 * glide)) / 6.0f;
        }
        const Glow main = hue(hue_), accent = hue(hue_ + 0.5f), white_hot = hue(hue_, 0.6f);
        const float pulse = (downbeat ? 1.0f : 0.55f) * std::exp(-beat_in * 6.0f); // each beat's hit, falling
        // The mix's level held a moment, falling slowly (Levels).
        {
            float level = led_colour::clamp01(std::sqrt(led_colour::clamp01(clock.level * 2.5f)));
            held_level_ = level > held_level_ ? level : held_level_ * 0.93f + level * 0.07f;
        }

        std::array<Glow, surface_pad_lights> pads {};
        Glow fader;
        std::array<ButtonLight, size_t(SurfaceButton::Count)> buttons {};
        std::array<bool, surface_bank_lights> banks {};
        // Bank lights: the bar in eighths, while playing.
        unsigned eighth = unsigned(bar_in * surface_bank_lights) % surface_bank_lights;
        if (clock.playing) banks[eighth] = true;

        switch (style) {
        case ShowStyle::Pulse:
        case ShowStyle::Count: {
            for (auto& p : pads) { p.add(main, 0.2f); p.add(downbeat ? white_hot : main, 0.8f * pulse); }
            fader = pads[0];
            for (auto b : panel_lit_buttons) buttons[size_t(b)] = button_level(0.15f + pulse);
            if (clock.playing && downbeat && beat_in < 0.15f)
                for (auto b : panel_lit_buttons)
                    if (surface_button_lights[size_t(b)] == ButtonLightKind::WhiteRed) buttons[size_t(b)] = ButtonLight::RedBright;
            break;
        }
        case ShowStyle::Sweep: { // left to right on even beats, back on odd ones, eased at the ends
            float t = beat_in * beat_in * (3 - 2 * beat_in);
            float x = beat % 2 ? panel_right - t * (panel_right - panel_left) : panel_left + t * (panel_right - panel_left);
            const Glow& c = downbeat ? accent : main;
            auto band = [&](Point p) { return std::fmax(0.0f, 1 - std::fabs(p.x - x) / 1.3f); };
            for (unsigned n = 0; n < surface_pad_lights; ++n) { pads[n].add(main, 0.22f); pads[n].add(c, band(pad_at(n))); }
            fader.add(main, 0.22f); fader.add(c, band(fader_at()));
            for (auto b : panel_lit_buttons) buttons[size_t(b)] = button_level(band(button_at(b)), 0.15f, 0.55f);
            for (unsigned n = 0; n < surface_bank_lights; ++n) banks[n] = banks[n] || band(bank_at(n)) > 0.5f;
            break;
        }
        case ShowStyle::Chase: { // a pad a sixteenth with a short tail; the buttons round the panel in eighths
            unsigned steps = bar_beats * 4, at = unsigned(bar_in * steps) % steps;
            float in = float(bar_in * steps - std::floor(bar_in * steps));
            for (auto& p : pads) p.add(main, 0.25f);
            for (unsigned tail = 0; tail < 3; ++tail) {
                unsigned step = (at + steps - tail) % steps;
                if (step >= surface_pad_lights) continue; // a bar longer than four beats: the pads rest
                float level = tail == 0 ? 1.0f : (tail == 1 ? 0.45f : 0.2f) * (1 - in);
                pads[step].add(step % 4 == 0 ? accent : main, level);
            }
            fader.add(main, 0.2f + 0.6f * pulse);
            unsigned lead = unsigned(clock.beats * 2) % round_panel.size();
            for (auto b : panel_lit_buttons) buttons[size_t(b)] = ButtonLight::Off;
            buttons[size_t(round_panel[lead])] = ButtonLight::Bright;
            buttons[size_t(round_panel[(lead + round_panel.size() - 1) % round_panel.size()])] = ButtonLight::Dim;
            break;
        }
        case ShowStyle::Ripple: { // a calm base breathing over the bar; rings from the pads pressed and on beat 1
            if (clock.playing && downbeat && beat != ripple_beat_) { ripple_beat_ = beat; ring({1.5f, 1.5f}, now, 0.7f); }
            float base = 0.15f + 0.1f * std::sin(6.2831853f * bar_in);
            auto light = [&](Point p) {
                Glow g;
                g.add(main, base);
                for (const auto& r : rings_) {
                    if (!r.at || now + 1 < r.at || now + 1 - r.at >= ring_ms) continue;
                    unsigned age = now + 1 - r.at;
                    float dx = p.x - r.from.x, dy = p.y - r.from.y;
                    float off = std::fabs(std::sqrt(dx * dx + dy * dy) - ring_speed * float(age));
                    if (off < ring_width) g.add(accent, r.strength * (1 - off / ring_width) * (1 - float(age) / ring_ms) * 1.3f);
                }
                return g;
            };
            for (unsigned n = 0; n < surface_pad_lights; ++n) pads[n] = light(pad_at(n));
            fader = light(fader_at());
            for (auto b : panel_lit_buttons) buttons[size_t(b)] = button_level(light(button_at(b)).peak(), 0.3f, 0.7f);
            for (unsigned n = 0; n < surface_bank_lights; ++n) banks[n] = banks[n] || light(bank_at(n)).peak() > 0.5f;
            break;
        }
        case ShowStyle::Levels: { // rows fill with the mix (outer columns a little lower); top row the accent
            for (unsigned n = 0; n < surface_pad_lights; ++n) {
                unsigned col = n % 4, row = n / 4;
                float height = held_level_ * (col == 0 || col == 3 ? 3.4f : 4.0f);
                float fill = led_colour::clamp01(height - float(row));
                pads[n].add(main, 0.08f);
                pads[n].add(row == 3 ? accent : main, fill);
            }
            fader.add(main, held_level_);
            for (auto b : panel_lit_buttons) { // up both sides from the bottom
                float y = button_at(b).y / 4.6f;
                buttons[size_t(b)] = held_level_ > y + 0.05f ? ButtonLight::Bright : held_level_ > y - 0.15f ? ButtonLight::Dim : ButtonLight::Off;
            }
            for (unsigned n = 0; n < surface_bank_lights; ++n) banks[n] = held_level_ * surface_bank_lights > float(n) + 0.5f;
            break;
        }
        }
        // Every style: Play lit while playing, Tap Tempo on the beat.
        if (clock.playing) buttons[size_t(SurfaceButton::Play)] = ButtonLight::Bright;
        if (style != ShowStyle::Chase) buttons[size_t(SurfaceButton::TapTempo)] = beat_in < 0.2f ? ButtonLight::Bright : ButtonLight::Dim;
        // A pressed pad: white, into the colour, over flash_ms.
        for (unsigned n = 0; n < surface_pad_lights; ++n)
            if (flashes_[n] && now + 1 >= flashes_[n] && now + 1 - flashes_[n] < flash_ms) {
                float t = float(now + 1 - flashes_[n]) / flash_ms;
                pads[n].add(hue(hue_ + 0.5f, 1 - t), flash_strength_[n] * (1 - t) * 1.6f / std::fmax(scale, 0.01f));
            }

        for (unsigned n = 0; n < surface_pad_lights; ++n) {
            Glow g = pads[n]; g.r *= scale; g.g *= scale; g.b *= scale;
            f.pads[n] = g.rgb();
        }
        fader.r *= scale; fader.g *= scale; fader.b *= scale;
        f.fader = fader.rgb();
        for (auto b : panel_lit_buttons) {
            auto l = buttons[size_t(b)];
            if (fade_in < 1 || (!clock.playing && l == ButtonLight::Bright && b != SurfaceButton::TapTempo)) l = l == ButtonLight::Off ? l : ButtonLight::Dim;
            if (!clock.playing && (l == ButtonLight::RedBright || l == ButtonLight::RedDim)) l = ButtonLight::Dim;
            f.buttons[size_t(b)] = l;
        }
        for (unsigned n = 0; n < surface_bank_lights; ++n) f.banks[n] = fade_in >= 1 && banks[n];
        out.meters = clock.meters;
        return out;
    }

private:
    void ring(show_light::Point from, uint32_t now, float strength) {
        rings_[next_] = {from, now + 1, led_colour::clamp01(strength)};
        next_ = (next_ + 1) % rings;
    }
    std::array<show_light::Ring, rings> rings_ {};
    std::array<uint32_t, surface_pad_lights> flashes_ {};
    std::array<float, surface_pad_lights> flash_strength_ {};
    unsigned next_ = 0, ripple_beat_ = ~0u;
    uint32_t started_ = 0;
    float hue_ = 0, held_level_ = 0;
};

// Sends a show frame (SurfaceLights sends only what changed).
inline void show_frame(SurfaceLights& lights, const ShowFrame& frame) {
    show_light_frame(lights, frame.lights);
    for (unsigned side = 0; side < surface_meter_lights; ++side) lights.meter(side, frame.meters[side]);
}

} // namespace melodee
