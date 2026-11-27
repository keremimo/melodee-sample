// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The mixer (docs/MIXER_PLAN.md; Pad FX + Knob FX). It opens on the strips: every channel of the row side by side from
// the left, as wide as the row lets them be, the selected one lit (number, stereo meter, level, M and S). Plus held:
// the card view, the selected one a wide card in the middle, its neighbours narrow strips either side (number, stereo
// meter, level); Minus held: the strips again (shell/mixer_channels.h MixerViewHold). The card: name, mute and solo, a slim meter
// and its fader, and one block, the one B1-B3 light: SENDS (four sends and what each bus holds), EQ (nine bands as a
// curve, the three in hand lit; B2 again steps LOW, MID, HIGH) or COMP (its curve and what it took off of late).
// K1-K3 turn the lit block (Shift: its second row), the fader the level. A pad or the wheel pick the channel; Minus /
// Plus move through the lit block (a row, a group of bands, an effect); Push turns the EQ or compressor on or off.
// B1 again: FX, the channel's chain in the card: Push edits the lit effect, Mute switches it, Erase takes it out. Pad Bank steps between the tracks and the second
// row: submixes, buses, the input and the master (shell/mixer_channels.h).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/erase_copy.h"
#include "melodee/shell/features.h"
#include "melodee/shell/mixer_channels.h"
#include "melodee/shell/new_track.h"
#include <cmath>
#include <complex>

namespace melodee {
namespace {
using namespace ui;

// The row: the card in the middle, strips either side, the outer ones cut by the screen's edge.
constexpr float top = zone::picture_y + 3, bottom = zone::picture_y + zone::picture_h - 6;
constexpr float card_x = 75, card_w = 170, strip_w = 25, gap = 3;
constexpr float head_h = 18; // a strip's and the card's top
constexpr float block_x = card_x + 32, block_y = top + head_h + 5, block_w = card_x + card_w - 5 - block_x, block_h = bottom - block_y;

float meter_fraction(float linear) { return linear > 1e-5f ? std::clamp((20 * std::log10(linear) + 48) / 48, 0.0f, 1.0f) : 0.0f; }

// A meter of short segments, bottom up: lit to `fill`, the rest on the groove.
void segments(Paint& p, float x, float y, float w, float h, float fill, juce::Colour c) {
    constexpr float seg = 2, space = 1;
    const unsigned count = unsigned((h + space) / (seg + space));
    const unsigned lit = unsigned(std::lround(fill * float(count)));
    for (unsigned n = 0; n < count; ++n) {
        const float sy = y + h - float(n + 1) * (seg + space) + space;
        p.fill(x, sy, w, seg, n < lit ? c : colour::line);
    }
}

// A level's fader: the groove and the cap where the level sits (0..1 of its travel).
void fader(Paint& p, float x, float y, float h, float level, juce::Colour c) {
    p.fill(x, y, 2, h, colour::line, 1);
    const float at = y + h * (1 - std::clamp(level, 0.0f, 1.0f));
    p.fill(x, at, 2, y + h - at, c, 1);
    p.fill(x - 3, at - 2, 8, 4, colour::text, 1);
}

juce::String track_title(const Track& track) {
    if (track.name[0]) return name_case(track.name);
    return track.used() ? engine_word(track.engine) : juce::String("Empty");
}

// A channel as the row and the card show it (shell/mixer_channels.h): its strip number, kind, short number and name,
// colour, where its fader sits and its mute, solo and pan.
struct Channel {
    unsigned n = 0;
    ChannelKind kind = ChannelKind::Track;
    juce::String number, name;
    juce::Colour colour;
    bool used = true;     // a track with something on it; every other channel is always there
    float level = 0;      // the fader's place, 0..1
    bool mute = false, solo = false;
    int pan = 0;
};

Channel channel_of(const Snapshot& s, unsigned n) {
    Channel ch;
    ch.n = n; ch.kind = channel_kind(n);
    switch (ch.kind) {
    case ChannelKind::Track: {
        const auto& t = s.project.tracks[n];
        ch.number = juce::String(n + 1); ch.name = track_title(t); ch.colour = track_colour(n); ch.used = t.used();
        ch.level = t.mix.level / 127.0f; ch.mute = t.mix.mute; ch.solo = t.mix.solo; ch.pan = t.mix.pan;
        break;
    }
    case ChannelKind::Submix: {
        const unsigned sub = channel_sub(n);
        const auto& x = s.project.subs[sub];
        ch.number = "S" + juce::String(sub + 1); ch.name = x.name[0] ? name_case(x.name) : juce::String("Submix");
        ch.colour = sub_colour(sub);
        ch.level = x.mix.level / 127.0f; ch.mute = x.mix.mute; ch.solo = x.mix.solo; ch.pan = x.mix.pan;
        break;
    }
    case ChannelKind::Bus: { // named after what it holds first, as the SENDS block names it
        const unsigned b = channel_bus(n);
        const auto& x = s.project.buses[b];
        const auto& first = x.fx.slots[0];
        ch.number = juce::String::charToString(juce::juce_wchar('A' + b));
        ch.name = first.used() ? name_case(effect_info(EffectKind(first.kind)).name) : juce::String("Return");
        ch.colour = loop_colour(b); ch.level = x.level / 100.0f; ch.mute = x.mute;
        break;
    }
    case ChannelKind::Input: // its fader is Input Config's gain, -12..24 dB
        ch.number = "IN"; ch.name = "Input"; ch.colour = colour::ice;
        ch.level = float(s.ui.settings.input.gain + 12) / 36.0f;
        break;
    case ChannelKind::Master: // no level of its own: its fader is its strip's trim
        ch.number = "MST"; ch.name = "Master"; ch.colour = colour::text;
        ch.level = float(s.project.master_strip.trim + trim_range) / float(2 * trim_range);
        break;
    }
    return ch;
}

// The knobs' shades on the channel's colour (pages/common.h channel_shade).
juce::Colour shade(const Channel& ch, unsigned knob) { return channel_shade(ch.n, knob); }

// A neighbour: its number on its colour, its stereo meter and where its level sits.
void strip(Paint& p, const Snapshot& s, const Channel& ch, float x) {
    p.fill(x, top, strip_w, bottom - top, colour::panel, 3);
    p.fill(x, top, strip_w, 2, ch.used ? ch.colour : colour::line, 1);
    auto style = plain(ch.used ? ch.colour : colour::dim, type::body, Weight::Bold);
    p.text(ch.number, x, top + 2, strip_w, head_h - 2, p.fit(ch.number, style, strip_w - 2, type::floor), juce::Justification::centred);
    if (!ch.used) return;
    const auto& m = s.strips[ch.n];
    const float my = top + head_h + 4, mh = bottom - my - 4;
    segments(p, x + 5, my, 4, mh, ch.mute ? 0 : meter_fraction(m.left), ch.colour);
    segments(p, x + 10, my, 4, mh, ch.mute ? 0 : meter_fraction(m.right), ch.colour);
    const float at = my + mh * (1 - std::clamp(ch.level, 0.0f, 1.0f));
    p.fill(x + 17, my, 1, mh, colour::line);
    p.fill(x + 15, at - 1.5f, 6, 3, colour::text, 1);
}

// The strips view's channel: its number (on its colour when it is the one in hand, the strip raised), its stereo
// meter, where its level sits, and M (over the meter a mute leaves dark) and S under it when on. `w` 18 px for the
// sixteen tracks, 20 for the second row. `bare`: its ground alone (under the card fading in).
void lane(Paint& p, const Snapshot& s, const Channel& ch, float x, float w, bool here, bool bare = false) {
    p.fill(x, top, w, bottom - top + 5, here ? colour::raised : colour::panel, 3);
    if (here) {
        p.fill(x, top, w, head_h, ch.used ? ch.colour : colour::line, 3);
        p.fill(x, top + head_h - 4, w, 4, ch.used ? ch.colour : colour::line);
    } else {
        p.fill(x, top, w, 2, ch.used ? ch.colour : colour::line, 1);
    }
    if (bare) return;
    auto style = plain(here ? (ch.used ? colour::bg : colour::text) : ch.used ? ch.colour : colour::dim, type::body, Weight::Bold);
    p.text(ch.number, x, top + (here ? 0.0f : 2.0f), w, head_h - (here ? 0.0f : 2.0f), p.fit(ch.number, style, w - 2, type::floor), juce::Justification::centred);
    if (!ch.used) return;
    const auto& m = s.strips[ch.n];
    const float my = top + head_h + 4, mh = bottom - my - 16, mw = w >= 20 ? 4.0f : 3.0f;
    segments(p, x + 3, my, mw, mh, ch.mute ? 0 : meter_fraction(m.left), ch.colour);
    segments(p, x + 4 + mw, my, mw, mh, ch.mute ? 0 : meter_fraction(m.right), ch.colour);
    // The fader at the right edge, or where the card has it on a wider strip (the second row in Basic, a slide).
    const float fx = x + std::min(w - 4, 19.0f), at = my + mh * (1 - std::clamp(ch.level, 0.0f, 1.0f));
    p.fill(fx, my, 1, mh, colour::line);
    p.fill(fx - 2.5f, at - 1.5f, 6, 3, colour::text, 1);
    auto pill = [&](const char* letter, float y, juce::Colour c) {
        const float pw = std::min(w - 4, 14.0f);
        p.fill(x + (w - pw) / 2, y, pw, 12, c, 2);
        p.text(letter, x + (w - pw) / 2, y, pw, 12, plain(colour::bg, type::small, Weight::Bold), juce::Justification::centred);
    };
    if (ch.mute && channel_mutes(ch.kind)) pill("M", my + mh - 13, colour::rec);
    if (ch.solo && channel_solos(ch.kind)) pill("S", bottom - 12, colour::amber);
}

// A channel's place across the screen: its left edge and width.
struct Span {
    float x, w;
};
// In the strips view: the row in view from the left edge, at most 40 px apart (16 tracks 20, the second row 22); the
// master, last, takes what is left of the screen's width, up to 14 px more, so MST fits.
Span strips_span(unsigned count, unsigned k, ChannelKind kind) {
    const float pitch = std::min(40.0f, std::floor(float(zone::width) / float(std::max(count, 1u)))), x = float(k) * pitch;
    return {x, kind == ChannelKind::Master ? std::min(float(zone::width) - x, pitch + 14) : pitch - 2};
}
// In the card view: the one in hand the card, the rest narrow strips either side of it (off the screen past the third).
Span card_span(unsigned at, unsigned k) {
    if (k == at) return {card_x, card_w};
    const int d = int(k) - int(at);
    return d < 0 ? Span {card_x + float(d) * (strip_w + gap), strip_w} : Span {card_x + card_w + gap + float(d - 1) * (strip_w + gap), strip_w};
}

void strips_view(Paint& p, const Snapshot& s) {
    const unsigned count = mixer_row_count(s.ui), at = mixer_row_at(s.ui);
    for (unsigned k = 0; k < count; ++k) {
        const Channel ch = channel_of(s, mixer_row_strip(s.ui, k));
        const Span sp = strips_span(count, k, ch.kind);
        lane(p, s, ch, sp.x, sp.w, k == at);
    }
}

// The response of a strip's nine bands at `hz`, dB (model/strip.h; the engine's peaks, engine/rbj.h, at 48 kHz).
float eq_db(const Strip& strip, float hz) {
    if (!strip.eq_on) return 0;
    constexpr double fs = 48000, q = 1.4142;
    const std::complex<double> z1 = std::polar(1.0, -2 * juce::MathConstants<double>::pi * hz / fs), z2 = z1 * z1;
    std::complex<double> h = 1;
    for (unsigned n = 0; n < eq_bands; ++n) {
        if (!strip.eq[n]) continue;
        const double A = std::pow(10.0, strip.eq[n] / 10.0 / 40.0);
        const double w = 2 * juce::MathConstants<double>::pi * std::min(double(eq_band_hz[n]), 0.45 * fs) / fs, alpha = std::sin(w) / (2 * q), cw = std::cos(w);
        const double a0 = 1 + alpha / A;
        const double b0 = (1 + alpha * A) / a0, b1 = -2 * cw / a0, b2 = (1 - alpha * A) / a0, a1 = -2 * cw / a0, a2 = (1 - alpha / A) / a0;
        h *= (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    }
    return float(20 * std::log10(std::max(std::abs(h), 1e-9)));
}

// SENDS: four columns, A-D: how much goes into each bus, and what the bus holds first. A track's or a submix's.
void sends_block(Paint& p, const Snapshot& s, const Channel& ch) {
    const auto& sends = ch.kind == ChannelKind::Submix ? s.project.subs[channel_sub(ch.n)].mix.sends : s.project.tracks[ch.n % track_count].mix.sends;
    const bool basic = !full_features(s.ui);
    const unsigned shown = basic ? 1 : bus_count;
    const float col = block_w / float(bus_count), bar_y = block_y + 22, bar_h = block_h - 56;
    for (unsigned b = 0; b < shown; ++b) {
        const float cx = block_x + col * (float(b) + 0.5f);
        const auto sc = b < 3 ? shade(ch, b) : colour::mid;
        const int amount = sends[b];
        p.text(juce::String(amount), cx - col / 2, block_y + 4, col, 16, plain(colour::text, type::body, Weight::SemiBold), juce::Justification::centred);
        p.fill(cx - 4, bar_y, 8, bar_h, colour::line, 4);
        const float fill = bar_h * amount / 100.0f;
        if (amount > 0) p.fill(cx - 4, bar_y + bar_h - fill, 8, fill, sc, 4);
        p.text(juce::String::charToString(juce::juce_wchar('A' + b)), cx - col / 2, bar_y + bar_h + 2, col, 15, plain(sc, type::body, Weight::Bold), juce::Justification::centred);
        // What the bus holds first: its name where it fits the column, else its family's glyph (text is never cut).
        const auto& first = s.project.buses[b].fx.slots[0];
        const float ny = bar_y + bar_h + 17;
        auto style = plain(colour::mid, type::small, Weight::Medium);
        if (!first.used()) {
            p.text(juce::String::fromUTF8("\xe2\x80\x93"), cx - col / 2, ny, col, 13, style, juce::Justification::centred);
            continue;
        }
        const auto& info = effect_info(EffectKind(first.kind));
        const juce::String held = name_case(info.name);
        if (p.width(held, style) <= col - 2) p.text(held, cx - col / 2 + 1, ny, col - 2, 13, style, juce::Justification::centred);
        else icon(p, family_icon(info.family), cx - 6, ny, 12, colour::mid, 1.4f);
    }
}

// EQ: the nine bands as a curve over 40 Hz - 20 kHz, the group in hand shaded and its three dots in the knobs' shades.
void eq_block(Paint& p, const Snapshot& s, const Channel& ch) {
    const auto& strip = s.project.strip(ch.n);
    const float x = block_x + 3, y = block_y + 4, w = block_w - 6, h = block_h - 20;
    const float lo = std::log10(40.0f), hi = std::log10(20000.0f);
    auto x_of = [&](float hz) { return x + w * (std::log10(hz) - lo) / (hi - lo); };
    auto y_of = [&](float db) { return y + h / 2 - std::clamp(db, -15.0f, 15.0f) / 15.0f * (h / 2); };
    const unsigned g = s.ui.mixer_eq_group % 3;
    const float gx0 = x_of(eq_band_hz[g * 3] / 1.414f), gx1 = x_of(eq_band_hz[g * 3 + 2] * 1.414f);
    p.fill(gx0, y, gx1 - gx0, h, shade(ch, 0).withAlpha(0.14f), 3);
    // What the channel plays (shell/spectrum_view.h), behind the curve: -72 dB at the floor of the picture, 0 dB at its top.
    {
        juce::Path heard;
        auto y_level = [&](float db) { return y + h - std::clamp((db + 72) / 72, 0.0f, 1.0f) * h; };
        heard.startNewSubPath(x_of(spectrum_low_hz), y + h);
        bool any = false;
        for (unsigned b = 0; b < spectrum_bands; ++b) {
            const float db = s.spectrum[b];
            any |= db > -72;
            heard.lineTo(x_of(spectrum_band_hz(float(b) + 0.5f)), y_level(db));
        }
        heard.lineTo(x_of(spectrum_high_hz), y + h);
        heard.closeSubPath();
        if (any) { p.g.setColour(colour::mid.withAlpha(0.16f)); p.g.fillPath(heard); }
    }
    p.fill(x, y + h / 2, w, 1, colour::line);
    juce::Path curve;
    for (unsigned n = 0; n <= 96; ++n) {
        const float hz = std::pow(10.0f, lo + (hi - lo) * float(n) / 96);
        const float px = x_of(hz), py = y_of(eq_db(strip, hz));
        if (n == 0) curve.startNewSubPath(px, py); else curve.lineTo(px, py);
    }
    p.g.setColour(strip.eq_on ? colour::text : colour::dim);
    p.g.strokePath(curve, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    for (unsigned n = 0; n < eq_bands; ++n) {
        const float px = x_of(eq_band_hz[n]), py = y_of(eq_db(strip, eq_band_hz[n]));
        const bool hand = n / 3 == g;
        if (hand) {
            const auto c = shade(ch, n % 3);
            p.fill(px - 0.5f, std::min(py, y + h / 2), 1, std::fabs(py - (y + h / 2)), c);
            p.circle(px, py, 3.2f, c);
        } else {
            p.circle(px, py, 2.4f, colour::text);
            p.circle(px, py, 1.4f, colour::panel);
        }
    }
    auto label = plain(colour::mid, type::small, Weight::Medium);
    p.text("63", x, y + h + 2, 30, 13, label);
    p.text("1k", x_of(1000) - 15, y + h + 2, 30, 13, label, juce::Justification::centred);
    p.text("16k", x + w - 30, y + h + 2, 30, 13, label, juce::Justification::centredRight);
    if (!strip.eq_on) p.text("OFF", x + w - 40, y + 2, 38, 13, caps_text(colour::dim), juce::Justification::centredRight);
}

// COMP: its curve (in against out, the knee where AMOUNT sets it) and what it took off of late, bars hanging down.
void comp_block(Paint& p, const Snapshot& s, const Channel& ch) {
    const auto& strip = s.project.strip(ch.n);
    const auto& trail = s.strip_trail;
    const float reduce = s.strips[ch.n].reduction;
    const bool on = strip.comp_on && strip.comp_amount;
    p.text("GR", block_x + 4, block_y + 2, 30, 14, caps_text(colour::mid));
    const juce::String value = (reduce >= 0.05f ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(reduce, 1);
    p.text(value + " dB", block_x + 30, block_y + 1, block_w - 34, 16, plain(on ? colour::text : colour::dim, type::body, Weight::SemiBold), juce::Justification::centredRight);
    // The curve: -48..0 dB in and out, the threshold at -AMOUNT, the ratio above it.
    const float cx = block_x + 4, cy = block_y + 20, cs = std::min(block_h - 26, 58.0f);
    p.fill(cx, cy, cs, cs, colour::panel, 2);
    const float threshold = -float(strip.comp_amount), slope = 1 - 10.0f / float(std::max<unsigned>(strip.comp_ratio, comp_ratio_min));
    auto out_of = [&](float in) {
        const float over = in - threshold;
        const float red = over <= -3 ? 0 : over >= 3 ? over * slope : slope * (over + 3) * (over + 3) / 12;
        return on ? in - red : in;
    };
    juce::Path curve;
    for (unsigned n = 0; n <= 48; ++n) {
        const float in = -48 + float(n), out = out_of(in);
        const float px = cx + cs * (in + 48) / 48, py = cy + cs * (1 - (out + 48) / 48);
        if (n == 0) curve.startNewSubPath(px, py); else curve.lineTo(px, py);
    }
    p.g.setColour(on ? shade(ch, 0) : colour::dim);
    p.g.strokePath(curve, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const float heard = std::clamp(trail.back().level, -48.0f, 0.0f);
    if (on && trail.back().level > -90) p.circle(cx + cs * (heard + 48) / 48, cy + cs * (1 - (out_of(heard) + 48) / 48), 2.6f, colour::text);
    // Its reduction of late, newest on the right: a bar a column, 12 dB the bars' full depth.
    const float hx = cx + cs + 6, hw = block_x + block_w - 4 - hx, hh = cs;
    const unsigned shown = std::min<unsigned>(comp_columns, unsigned(hw / 2));
    for (unsigned n = 0; n < shown; ++n) {
        const float r = trail[comp_columns - shown + n].reduce;
        if (r < 0.05f) continue;
        p.fill(hx + float(n) * 2, cy, 1.4f, hh * std::min(r / 12.0f, 1.0f), colour::amber);
    }
    p.text(juce::String::fromUTF8("\xe2\x88\x92") + "12", hx, cy + hh + 1, 30, 13, plain(colour::mid, type::small, Weight::Medium));
    if (!on) p.text(strip.comp_on ? "AMOUNT 0" : "OFF", hx, cy + hh / 2 - 7, hw, 14, caps_text(colour::dim), juce::Justification::centred);
}

// FX: the channel's chain, an effect a row (its family's glyph and name; one switched off dim), then the plus while
// there is room. The lit row is the effect K1-K3 turn (its FX page's cursor: − / + move it); a long chain scrolls.
void fx_block(Paint& p, const Snapshot& s, const Channel& ch) {
    const int n = channel_chain(ch.n);
    if (n < 0) return;
    const auto& chain = s.project.chain(unsigned(n));
    const unsigned count = chain.count(), rows = count + (count < chain.size ? 1u : 0u);
    const unsigned lit = std::min<unsigned>(s.ui.fx_cursor[UiState::cursor_of(unsigned(n))], rows - 1);
    constexpr float row_h = 18;
    const unsigned fits = unsigned((block_h - 4) / row_h), first = lit >= fits ? lit - fits + 1 : 0;
    for (unsigned r = first; r < rows && r < first + fits; ++r) {
        const float y = block_y + 3 + float(r - first) * row_h, x = block_x + 3, w = block_w - 6;
        const bool here = r == lit;
        if (here) p.fill(x, y, w, row_h - 2, shade(ch, 0).withAlpha(0.22f), 3);
        if (r == count) { icon(p, Icon::Plus, x + 6, y + 3, 10, here ? colour::text : colour::mid, 1.6f); continue; } // the plus
        const auto& slot = chain.slots[r];
        const auto& info = effect_info(EffectKind(slot.kind));
        const bool off = slot.bypass;
        const juce::String name = slot.kind == plugin_effect_kind ? plugin_name(s, unsigned(slot.params[0])) : name_case(info.name);
        icon(p, family_icon(info.family), x + 5, y + 3, 10, off ? colour::dim : here ? shade(ch, 0) : colour::mid, 1.4f);
        auto style = plain(off ? colour::dim : here ? colour::text : colour::sec, type::body, Weight::Medium);
        p.text(name, x + 22, y, w - 26, row_h - 2, p.fit(name, style, w - 26, type::floor));
    }
}

// INPUT: what the input plays after its strip, a meter a side across the block, its peak in dB over them; the
// cards set its trim, Input Config's monitor and whether takes record it dry or wet.
void input_block(Paint& p, const Snapshot& s, const Channel& ch) {
    const auto& m = s.strips[strip_input];
    const float peak = std::max(m.left, m.right);
    p.text("PEAK", block_x + 4, block_y + 4, 40, 14, caps_text(colour::mid));
    const juce::String minus = juce::String::fromUTF8("\xe2\x88\x92");
    const float db = peak > 1e-5f ? 20 * std::log10(peak) : -90.0f;
    const juce::String value = db <= -89 ? minus + juce::String::fromUTF8("\xe2\x88\x9e") : (db < -0.05f ? minus : juce::String()) + juce::String(std::fabs(db), 1);
    p.text(value + " dB", block_x + 40, block_y + 2, block_w - 44, 16, plain(colour::text, type::body, Weight::SemiBold), juce::Justification::centredRight);
    const float x = block_x + 4, w = block_w - 8, mh = 14;
    auto row = [&](float y, float level) { // short segments left to right, 2 px with a gap of 1
        const unsigned count = unsigned((w + 1) / 3), lit = unsigned(std::lround(meter_fraction(level) * float(count)));
        for (unsigned n = 0; n < count; ++n) {
            const float f = float(n) / float(count);
            const auto c = n >= lit ? colour::line : f > 0.94f ? colour::rec : f > 0.82f ? colour::amber : ch.colour;
            p.fill(x + float(n) * 3, y, 2, mh, c);
        }
    };
    row(block_y + 26, m.left);
    row(block_y + 26 + mh + 4, m.right);
    auto label = plain(colour::mid, type::small, Weight::Medium);
    const float ly = block_y + 26 + 2 * mh + 8;
    p.text(minus + "48", x, ly, 30, 13, label);
    p.text(minus + "12", x + w * 0.75f - 15, ly, 30, 13, label, juce::Justification::centred);
    p.text("0", x + w - 20, ly, 20, 13, label, juce::Justification::centredRight);
}

// The master's COMP: the printed Compressor (Shift + Pad 5) as its page has it. What it takes off now, in dB, and
// what it took off of late, a bar a column hanging down (12 dB the full depth); OFF while it is off.
void master_comp_block(Paint& p, const Snapshot& s) {
    const auto& comp = s.project.compressor;
    const auto& trail = s.comp_trail;
    const float reduce = trail.back().reduce;
    p.text("GR", block_x + 4, block_y + 2, 30, 14, caps_text(colour::mid));
    const juce::String value = (comp.on && reduce >= 0.05f ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(comp.on ? reduce : 0.0f, 1);
    p.text(value + " dB", block_x + 30, block_y + 1, block_w - 34, 16, plain(comp.on ? colour::text : colour::dim, type::body, Weight::SemiBold), juce::Justification::centredRight);
    const float hx = block_x + 4, hy = block_y + 22, hw = block_w - 8, hh = block_h - 40;
    p.fill(hx, hy, hw, 1, colour::line);
    const unsigned shown = std::min<unsigned>(comp_columns, unsigned(hw / 2));
    if (comp.on)
        for (unsigned n = 0; n < shown; ++n) {
            const float r = trail[comp_columns - shown + n].reduce;
            if (r >= 0.05f) p.fill(hx + float(n) * 2, hy + 1, 1.4f, hh * std::min(r / 12.0f, 1.0f), colour::amber);
        }
    else p.text("OFF", hx, hy + hh / 2 - 7, hw, 14, caps_text(colour::dim), juce::Justification::centred);
    p.text(juce::String::fromUTF8("\xe2\x88\x92") + "12", hx, hy + hh + 2, 30, 13, plain(colour::mid, type::small, Weight::Medium));
}

// The channel in hand's card: its name on its colour with M and S (those it has) and, for a track played into a
// submix, that submix's number in its colour; its meter and fader, its pan, the lit block.
void card(Paint& p, const Snapshot& s, const Channel& ch) {
    p.fill(card_x, top, card_w, bottom - top + 5, colour::panel, 4);
    p.fill(card_x, top, card_w, head_h, ch.used ? ch.colour : colour::raised, 4);
    p.fill(card_x, top + head_h - 4, card_w, 4, ch.used ? ch.colour : colour::raised);
    const bool numbered = ch.kind != ChannelKind::Input && ch.kind != ChannelKind::Master;
    const juce::String title = numbered ? ch.number + "  " + ch.name : ch.name;
    auto head = plain(ch.used ? colour::bg : colour::dim, type::body, Weight::Bold);
    const bool mutes = ch.used && channel_mutes(ch.kind), solos = ch.used && channel_solos(ch.kind);
    const unsigned to = ch.kind == ChannelKind::Track && ch.used ? s.project.tracks[ch.n].mix.out : out_master;
    const float right = (mutes ? 16.0f : 0) + (solos ? 16.0f : 0) + (to != out_master ? 22.0f : 0) + (mutes || solos || to != out_master ? 2.0f : 0);
    p.text(title, card_x + 6, top, card_w - 12 - right, head_h, p.fit(title, head, card_w - 12 - right, type::floor));
    if (!ch.used) {
        p.text("Empty", card_x, top + head_h, card_w, bottom - top - head_h, plain(colour::dim, type::body, Weight::Medium), juce::Justification::centred);
        return;
    }
    // M and S: lit red and amber when on, as the MUTE key and Shift + pad set them.
    float px = card_x + card_w - 18;
    auto pill_at = [&](const char* letter, bool lit, juce::Colour on) {
        p.fill(px, top + 3, 14, 12, lit ? on : colour::bg.withAlpha(0.25f), 2);
        p.text(letter, px, top + 3, 14, 12, plain(lit ? colour::bg : colour::bg.withAlpha(0.8f), type::small, Weight::Bold), juce::Justification::centred);
        px -= 16;
    };
    if (solos) pill_at("S", ch.solo, colour::amber);
    if (mutes) pill_at("M", ch.mute, colour::rec);
    if (to != out_master) { // the submix it plays into, on that submix's colour
        const unsigned sub = (to - 1u) % sub_count;
        px -= 6;
        p.fill(px, top + 3, 20, 12, sub_colour(sub), 2);
        p.text("S" + juce::String(sub + 1), px, top + 3, 20, 12, plain(colour::bg, type::small, Weight::Bold), juce::Justification::centred);
    }
    const auto& m = s.strips[ch.n];
    const float my = top + head_h + 5, mh = bottom - my - 10;
    segments(p, card_x + 4, my, 4, mh, ch.mute ? 0 : meter_fraction(m.left), ch.colour);
    segments(p, card_x + 9, my, 4, mh, ch.mute ? 0 : meter_fraction(m.right), ch.colour);
    fader(p, card_x + 19, my, mh, ch.level, ch.colour);
    if (channel_sends(ch.kind)) { // pan: a short line under them, the dot where it points
        const float py = bottom - 3, px0 = card_x + 4, pw = 22;
        p.fill(px0, py - 0.5f, pw, 1, colour::line);
        p.circle(px0 + pw * (ch.pan + 50) / 100.0f, py, 2.2f, colour::text);
    }
    const bool full = full_features(s.ui);
    if (!full && !channel_sends(ch.kind)) return; // Basic: SENDS only, and this channel has none
    p.fill(block_x, block_y, block_w, block_h, colour::surf, 3);
    switch (mixer_block_of(s.ui, ch.kind)) {
    case block_eq: eq_block(p, s, ch); break;
    case block_comp: if (ch.kind == ChannelKind::Master) master_comp_block(p, s); else comp_block(p, s, ch); break;
    case block_fx: fx_block(p, s, ch); break;
    case block_input: input_block(p, s, ch); break;
    default: sends_block(p, s, ch); break;
    }
}

// Between the strips and the card (Plus or Minus held, mixer_view_ms): each channel slides and grows from its place in
// one view to its place in the other, the one in hand into the card's, whose contents fade in over the last quarter.
// `f` 0 the strips, 1 the card.
void slide(Paint& p, const Snapshot& s, const Channel& ch, float f) {
    const unsigned count = mixer_row_count(s.ui), at = mixer_row_at(s.ui);
    auto between = [&](unsigned k, ChannelKind kind) {
        const Span a = strips_span(count, k, kind), b = card_span(at, k);
        return Span {a.x + (b.x - a.x) * f, a.w + (b.w - a.w) * f};
    };
    for (unsigned k = 0; k < count; ++k) {
        if (k == at) continue;
        const Channel other = channel_of(s, mixer_row_strip(s.ui, k));
        const Span sp = between(k, other.kind);
        if (sp.x + sp.w > 0 && sp.x < float(zone::width)) lane(p, s, other, sp.x, sp.w, false);
    }
    const Span sp = between(at, ch.kind);
    const bool fading = f > 0.75f;
    lane(p, s, ch, sp.x, sp.w, true, fading);
    if (!fading) return;
    p.g.saveState(); // the card squeezed to where its strip is now, over its bare ground
    p.g.addTransform(juce::AffineTransform::translation(-card_x, 0).scaled(sp.w / card_w, 1).translated(sp.x, 0));
    p.g.beginTransparencyLayer((f - 0.75f) / 0.25f);
    card(p, s, ch);
    p.g.endTransparencyLayer();
    p.g.restoreState();
}

void draw(Paint& p, const Snapshot& s) {
    const Channel ch = channel_of(s, mixer_strip(s.ui));
    const bool basic = !full_features(s.ui);
    Header h = header_of(s);
    const juce::Colour own = s.ui.mixer_bank == 0 ? colour_of(s) : ch.colour; // the second row: the channel's colour
    h.left.push_back(chip(Icon::Pads, "SELECT", own));
    h.right.push_back(label(s.ui.mixer_bank == 0 ? "TRACKS" : basic ? "BUSES" : "SUBMIXES, BUSES"));
    static const char* const groups[] {"EQ LOW", "EQ MID", "EQ HIGH"};
    const unsigned block = mixer_block_of(s.ui, ch.kind);
    const bool sends = channel_sends(ch.kind), fx = !basic && channel_fx(ch.kind);
    // B1: SENDS, again FX (a bus or the master FX alone); B2 EQ, again the next bands; B3 COMP.
    const char* b1 = block == block_fx || (!sends && fx) ? "FX" : sends ? "SENDS" : basic ? "" : "INPUT";
    SoftKeys keys {{b1, basic ? "" : block == block_eq ? groups[s.ui.mixer_eq_group % 3] : "EQ", basic || !channel_comp(ch.kind) ? "" : "COMP"},
                   basic && !sends ? -1 : block == block_fx || block == block_input ? 0 : int(block), own};
    if (!basic) { keys.dots[1] = 3; keys.dot_at[1] = uint8_t(s.ui.mixer_eq_group % 3); }
    if (sends && fx) { keys.dots[0] = 2; keys.dot_at[0] = uint8_t(block == block_fx ? 1 : 0); }
    frame(p, s, keys, h);
    const float f = mixer_card_shown(s.ui, s.now);
    if (f <= 0) { strips_view(p, s); return; } // as it opens, and Minus held: every channel a strip
    if (f < 1) { slide(p, s, ch, f); return; }   // on the way
    // The neighbours in the row in view, three a side, nearest the card first; then the card over them.
    const int at = int(mixer_row_at(s.ui)), count = int(mixer_row_count(s.ui));
    for (int k = 1; k <= 3; ++k) {
        if (at - k >= 0) strip(p, s, channel_of(s, mixer_row_strip(s.ui, unsigned(at - k))), card_x - float(k) * (strip_w + gap));
        if (at + k < count) strip(p, s, channel_of(s, mixer_row_strip(s.ui, unsigned(at + k))), card_x + card_w + gap + float(k - 1) * (strip_w + gap));
    }
    card(p, s, ch);
}

// A pad: its track, or on the second row its submix, bus, the input or the master (pads 13 and 14 hold none).
void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (!down) return;
    if (c.ui.mixer_bank == 1) {
        if (const int n = pad_other(c.ui, pad); n >= 0) pick_other(c.ui, unsigned(n));
        return;
    }
    c.select_track(pad % track_count);
    if (c.ui.erase) ask_clear_track(c); // Erase + pad: that track cleared, once B3 says so
    else offer_new_track(c);      // an empty one: its doors, then back here
}

// The wheel and Minus / Plus: the next channel of the row in view comes to the middle.
void wheel(Context& c, int delta) {
    const int at = std::clamp(int(mixer_row_at(c.ui)) + delta, 0, int(mixer_row_count(c.ui)) - 1);
    if (c.ui.mixer_bank == 0) c.select_track(unsigned(at));
    else c.ui.mixer_other = uint8_t(other_first(c.ui) + unsigned(at));
}

// Push: EQ's and the compressor's on and off; on FX the lit effect to edit (its FX page, every row and a plug-in's
// own screen; B1 MIXER back), on its plus the picker (effects and plug-ins), back here once one is in. SENDS has
// none. On FX, Mute switches the lit effect off or on and Erase takes it out (shell/mute_mode.h, shell/erase_copy.h).
void push(Context& c) {
    const unsigned n = mixer_strip(c.ui);
    const auto kind = channel_kind(n);
    if (kind == ChannelKind::Track && !c.track().used()) { offer_new_track(c); return; }
    if (c.ui.shift && kind == ChannelKind::Track) { c.ui.shift_used = true; c.name_track(c.ui.track); return; } // Shift + Push: its name
    const unsigned block = mixer_block_of(c.ui, kind);
    if (block == block_sends || block == block_input) return;
    if (block == block_comp && kind == ChannelKind::Master) { c.set_compressor(0, !c.project.compressor.on); return; } // the printed Compressor on or off
    if (block == block_fx) {
        const int chain = channel_chain(n);
        if (chain < 0) return;
        const auto& fx = c.project.chain(unsigned(chain));
        const unsigned slot = c.ui.fx_cursor[UiState::cursor_of(unsigned(chain))];
        c.open_mixer_chain(slot >= fx.count()); // an effect: its page to edit; the plus: the picker
        return;
    }
    const auto part = block == 1 ? StripPart::EqOn : StripPart::CompOn;
    const Strip& strip = c.project.strip(n);
    c.apply(strip_card(n, part, strip), strip_get(strip, part) ? 0 : 1);
}

// B1 SENDS (again: FX, the chain; a bus or the master FX alone), B2 EQ (again: the next three bands), B3 COMP; a key
// the channel in hand has no block for does nothing. A key pressed puts SENDS and COMP back on their first row.
void soft(Context& c, int key) {
    if (!full_features(c.ui)) { c.ui.mixer_block = 0; return; }
    const unsigned k = unsigned(key) % 3;
    const auto kind = channel_kind(mixer_strip(c.ui));
    if (k == 2 && !channel_comp(kind)) return;
    const unsigned now = mixer_block_of(c.ui, kind);
    if (k == 0 && channel_sends(kind) && (now == block_sends || now == block_fx)) c.ui.mixer_b1 = c.ui.mixer_b1 ? 0 : 1;
    if (k == 1 && now == block_eq) c.ui.mixer_eq_group = uint8_t((c.ui.mixer_eq_group + 1) % 3);
    c.ui.mixer_block = uint8_t(k);
    c.ui.mixer_row = 0;
}

// Minus / Plus, a tap (as it is let go; held, the strips or the card): through what the lit block holds. SENDS and COMP
// their two rows (−: the first, +: the second, as Shift shows it), EQ its three groups, FX the chain's effects and its
// plus. The wheel and the pads pick the channel.
void nudge(Context& c, int way) {
    const unsigned n = mixer_strip(c.ui);
    switch (mixer_block_of(c.ui, channel_kind(n))) {
    case block_eq: c.ui.mixer_eq_group = uint8_t(std::clamp(int(c.ui.mixer_eq_group % 3) + way, 0, 2)); break;
    case block_fx: {
        const int chain = channel_chain(n);
        if (chain < 0) break;
        const auto& fx = c.project.chain(unsigned(chain));
        const unsigned at = UiState::cursor_of(unsigned(chain)), last = std::min<unsigned>(fx.count(), fx.size - 1);
        c.ui.fx_cursor[at] = uint8_t(std::clamp(int(std::min<unsigned>(c.ui.fx_cursor[at], last)) + way, 0, int(last)));
        c.ui.fx_row[at] = 0;
        break;
    }
    default: c.ui.mixer_row = way > 0 ? 1 : 0; break;
    }
}
} // namespace

const PageSpec mixer_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
