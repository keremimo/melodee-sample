// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SENDS (the MIX hub's B2): four buses, A-D, each with its own chain and
// return. Every track sends into each of the four (Stage's SENDS sets one
// track's four); here one bus at a time, its sends drawn as strips, eight
// tracks at a time (the half with the selected track). K1 the selected
// track's send into the bus in view, K2 which bus, K3 that bus's return.
// The wheel (or Pad Bank + pad) picks the track, Minus / Plus the bus.
// On a kit B2 again shows its pads instead (shell/mix_hub.h): the bank in hand as the pads sit, each with its own
// send into the bus in view; a pad hit or the wheel picks one, K1 its send.
#include "melodee/engine/effect_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/mix_hub.h"

namespace melodee {
namespace {
using namespace ui;

// The kit's pads as they sit, pad 1 bottom left, from `top` down to the picture's foot: name, its send into the bus
// in view as a number and a bar along the foot; the last pad hit raised, its name on a chip in the track's colour.
void pad_grid(Paint& p, const Snapshot& s, unsigned bus, float top) {
    const auto& t = s.track();
    auto c = colour_of(s);
    float gap = 3, bottom = zone::picture_y + zone::picture_h - 2;
    float cw = (zone::width - 2 * zone::pad - 3 * gap) / 4, ch = (bottom - top - 3 * gap) / 4;
    unsigned first = t.bank % sampler_banks * 16;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned kit = first + n, row = 3 - n / 4, col = n % 4;
        float x = zone::pad + col * (cw + gap), y = top + row * (ch + gap);
        const auto& pad = t.pads[kit];
        bool on = kit == t.last_pad, used = pad.used();
        unsigned send = used ? pad.sends[bus] : 0;
        p.fill(x, y, cw, ch, on ? colour::raised : colour::quiet, 3);
        juce::String name = used ? name_case(pad_name(s, kit, true)) : juce::String();
        if (name.isEmpty()) { char label[4]; pad_label(kit, label); name = label; }
        juce::String amount = send ? juce::String(send) : juce::String();
        auto amount_style = caps_text(colour::mid);
        float aw = amount.isEmpty() ? 0 : p.width(amount, amount_style) + 3;
        float nw = cw - 8 - aw;
        auto name_style = plain(on ? colour::bg : used ? colour::text : colour::dim, type::body, on ? Weight::SemiBold : Weight::Medium);
        auto fitted = p.fit(name, name_style, nw - (on ? 6 : 0), type::floor);
        if (on) p.fill(x + 2, y + 2, std::min(nw, p.width(name, fitted) + 6), 16, c, 3);
        p.text(name, x + (on ? 5 : 4), y + 2, nw - (on ? 6 : 0), 16, fitted);
        if (aw > 0) p.text(amount, x + cw - 4 - aw, y + 2, aw, 16, amount_style, juce::Justification::centredRight);
        p.fill(x + 4, y + ch - 5, cw - 8, 2, colour::line, 1);
        if (send) p.fill(x + 4, y + ch - 5, (cw - 8) * send / 100.0f, 2, c, 1);
    }
}

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    unsigned bus = s.ui.sends_bus % bus_count;
    Header h = header_of(s);
    unsigned half = (s.ui.track % track_count) / 8;
    bool pads = mix_sends_pads(s.ui, s.track().engine == EngineKind::Sampler);
    if (pads) h.left.push_back(pill(juce::String("BANK ") + char('A' + s.track().bank % sampler_banks)));
    else h.left.push_back(pill(half ? "TRACKS 9-16" : "TRACKS 1-8"));
    h.right.push_back(tag_of(s, s.ui.track));
    h.left.push_back(pill(juce::String("BUS ") + char('A' + bus), c)); // the bus in view (was B3's name)
    frame(p, s, mix_keys(s), h);

    // The four buses: letter, first effect, return.
    float y = zone::picture_y + 4, gap = 4, bw = (zone::width - 2 * zone::pad - 3 * gap) / 4, bh = 22;
    for (unsigned b = 0; b < bus_count; ++b) {
        float x = zone::pad + b * (bw + gap);
        const auto& chain = s.project.buses[b].fx;
        bool on = b == bus, empty = !chain.count();
        p.fill(x, y, bw, bh, on ? colour::raised : colour::quiet, 4);
        if (on) p.fill(x + 4, y + 4, 14, 14, colour::text, 3);
        auto letter = juce::String::charToString(juce::juce_wchar('A' + b));
        p.text(letter, x + 4, y + 4, 14, 14, plain(on ? colour::bg : empty ? colour::dim : colour::sec, type::small, Weight::SemiBold), juce::Justification::centred);
        juce::String level = empty ? juce::String() : juce::String(s.project.buses[b].level);
        auto level_style = caps_text(colour::mid);
        float lw = level.isEmpty() ? 0 : p.width(level, level_style) + 2;
        if (lw > 0) p.text(level, x, y, bw - 4, bh, level_style, juce::Justification::centredRight);
        juce::String name = empty ? "Empty" : effect_info(EffectKind(chain.slots[0].kind)).name;
        float nw = bw - 21 - 2 - lw;
        p.text(name, x + 21, y, nw, bh, p.fit(name, plain(empty ? colour::dim : colour::text, type::small, Weight::Medium), nw, type::floor));
    }
    if (pads) { pad_grid(p, s, bus, y + bh + 4); return; }
    // Eight strips: each track's send into the bus in view.
    float top = y + bh + 4, sh = zone::picture_y + zone::picture_h - 2 - top, sg = 4, sw = (zone::width - 2 * zone::pad - 7 * sg) / 8;
    unsigned first = half * 8;
    for (unsigned n = 0; n < 8; ++n) {
        unsigned t = first + n;
        const auto& track = s.project.tracks[t];
        float x = zone::pad + n * (sw + sg), cx = x + sw / 2;
        bool selected = t == s.ui.track, used = track.used();
        auto tc = track_colour(t);
        p.fill(x, top, sw, sh, selected ? colour::raised : colour::quiet, 3);
        if (selected) p.fill(cx - 12, top + 3, 24, 18, tc, 3);
        p.text(juce::String(t + 1), x, top + 3, sw, 18, plain(selected ? colour::bg : used ? tc : colour::dim, 15, Weight::Bold),
               juce::Justification::centred);
        float fy = top + 26, fh = sh - 32;
        p.fill(cx - 3, fy, 6, fh, colour::line, 3);
        float send = used ? track.mix.sends[bus] / 100.0f : 0;
        if (send > 0) p.fill(cx - 3, fy + fh * (1 - send), 6, fh * send, tc, 3);
    }
}

void wheel(Context& c, int delta) {
    auto& t = c.track();
    if (mix_sends_pads(c.ui, t.engine == EngineKind::Sampler)) { // the pads: the next one in the bank in hand
        unsigned first = t.bank % sampler_banks * 16, at = t.last_pad >= first && t.last_pad < first + 16 ? t.last_pad - first : 0;
        t.last_pad = uint8_t(first + unsigned(int(at) + delta % 16 + 16) % 16);
        return;
    }
    c.select_track(unsigned(int(c.ui.track) + delta + 16) % 16);
}

void bus(Context& c, int way) { c.ui.sends_bus = uint8_t((c.ui.sends_bus + bus_count + way) % bus_count); }
} // namespace

const PageSpec sends_page {draw, play_pad, wheel, no_push, mix_soft, bus}; // Minus / Plus: the bus

} // namespace melodee
