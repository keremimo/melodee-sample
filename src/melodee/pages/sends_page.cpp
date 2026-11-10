// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SENDS (B3 from the track): four buses, A-D, each with its own chain and
// return. One send per track into the bus it picks, drawn as sixteen strips.
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
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{juce::String::fromUTF8("\xe2\x80\xb9 TRACK"), "SENDS",
                  juce::String("BUS ") + char('A' + bus) + juce::String::fromUTF8(" \xe2\x80\xba")}, 1, c}, h);
    signal_flow(p, Stop::Sends, c, s.track().engine == EngineKind::Sampler);

    // The four buses: letter, first effect, return.
    float y = zone::picture_y + 18, gap = 4, bw = (zone::width - 2 * zone::pad - 3 * gap) / 4, bh = 20;
    for (unsigned b = 0; b < bus_count; ++b) {
        float x = zone::pad + b * (bw + gap);
        const auto& chain = s.project.buses[b].fx;
        bool on = b == bus, empty = !chain.count();
        p.fill(x, y, bw, bh, on ? colour::raised : colour::quiet, 4);
        if (on) p.fill(x + 5, y + 4, 12, 12, colour::text, 3);
        auto letter = juce::String::charToString(juce::juce_wchar('A' + b));
        p.text(letter, x + 5, y + 4, 12, 12, plain(on ? colour::bg : empty ? colour::dim : colour::sec, 9, Weight::SemiBold), juce::Justification::centred);
        juce::String name = empty ? "Empty" : effect_info(EffectKind(chain.slots[0].kind)).name;
        p.text(name, x + 21, y, bw - 40, bh, p.fit(name, plain(empty ? colour::dim : colour::text, 10, Weight::Medium), bw - 44, 7));
        if (!empty) p.text(juce::String(s.project.buses[b].level), x, y, bw - 6, bh, caps_text(colour::mid, 7), juce::Justification::centredRight);
    }
    // Sixteen strips: each track's send, dim when it feeds another bus.
    float top = y + bh + 4, sh = zone::picture_y + 143.5f - top, sg = 2, sw = (zone::width - 2 * zone::pad - 15 * sg) / 16;
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& track = s.project.tracks[t];
        float x = zone::pad + t * (sw + sg), cx = x + sw / 2;
        bool selected = t == s.ui.track, used = track.used();
        auto tc = track_colour(t);
        p.fill(x, top, sw, sh, selected ? colour::raised : colour::quiet, 3);
        if (selected) p.fill(cx - 7, top + 3, 14, 13, tc, 3);
        p.text(juce::String(t + 1), cx - 10, top + 3, 20, 13, plain(selected ? colour::bg : used ? tc : colour::dim, 8.5f, Weight::Bold),
               juce::Justification::centred);
        float fy = top + 22, fh = sh - 30;
        p.fill(cx - 2, fy, 4, fh, colour::line, 2);
        float send = used ? track.mix.send / 100.0f : 0;
        if (send > 0) p.fill(cx - 2, fy + fh * (1 - send), 4, fh * send, track.mix.bus % bus_count == bus ? tc : tc.withAlpha(0.35f), 2);
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
