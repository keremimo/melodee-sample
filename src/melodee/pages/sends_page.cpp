// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SENDS (B3 from the track): four buses, A-D, each with its own chain and
// return. One send per track into the bus it picks, drawn as strips, eight
// at a time (the half with the selected track).
// K1 the selected track's send, K2 which bus, K3 that bus's return. The
// wheel (or Pad Bank + pad) picks the track.
#include "melodee/engine/effect_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/pages/signal_flow.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    unsigned bus = s.track().mix.bus % bus_count;
    Header h = header_of(s);
    h.left.push_back(track_half_pill(s));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{juce::String::fromUTF8("\xe2\x80\xb9 TRACK"), "SENDS",
                  juce::String("BUS ") + char('A' + bus) + juce::String::fromUTF8(" \xe2\x80\xba")}, 1, c}, h);
    signal_flow(p, Stop::Sends, c, s.track().engine == EngineKind::Sampler);

    // The four buses: letter, first effect, return.
    float y = zone::picture_y + 20, gap = 4, bw = (zone::width - 2 * zone::pad - 3 * gap) / 4, bh = 22;
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
    // Eight strips: each track's send, dim when it feeds another bus.
    float top = y + bh + 4, sh = zone::picture_y + zone::picture_h - 2 - top, sg = 4, sw = (zone::width - 2 * zone::pad - 7 * sg) / 8;
    unsigned first = track_half(s) * 8;
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
        float send = used ? track.mix.send / 100.0f : 0;
        if (send > 0) p.fill(cx - 3, fy + fh * (1 - send), 6, fh * send, track.mix.bus % bus_count == bus ? tc : tc.withAlpha(0.35f), 3);
    }
}

void wheel(Context& c, int delta) { c.select_track(unsigned(int(c.ui.track) + delta + 16) % 16); }

void soft(Context& c, int key) {
    if (key == 0) c.open(PageId::Rack);
    if (key == 2) c.open(PageId::Bus);
}
} // namespace

const PageSpec sends_page {draw, play_pad, wheel, no_push, soft, wheel}; // Minus / Plus: the track, as the wheel

} // namespace melodee
