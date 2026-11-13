// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PADS (Sample held on a kit, docs/HUBS_PLAN.md H-b; round 2, pads B "wave pads"): one sound a
// pad, drawn as the pads sit, each pad its own waveform from start to end,
// so a kit is known by its shapes; a pad that sounds lights and a head runs
// through it. The grid takes the whole picture; the pad in hand and the bank
// are in the header. Eight banks of sixteen; Pad Bank (or Minus / Plus)
// steps through them and bank lights A-H show which. Muted pads (mute mode)
// are drawn dim. The pots follow the last pad hit: pitch, level, pan. Push
// opens the map of the sampler's pages for that pad.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/shell/sampler_map.h"
#include "melodee/ui/waveform.h"
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    unsigned bank = track.bank % sampler_banks;
    Header h = header_of(s);
    char hand[4]; pad_label(track.last_pad, hand);
    auto hand_name = track.pads[track.last_pad % sampler_pads].used() ? pad_name(s, track.last_pad % sampler_pads, true) : juce::String("Empty");
    h.left.push_back(chip(juce::String(hand) + " " + hand_name, pad_colour(s, track.last_pad)));
    auto bank_pill = pill(juce::String("BANK ") + char('A' + bank));
    bank_pill.spare = true; // a long sample name keeps its room: the bank lights say the bank too
    h.left.push_back(bank_pill);
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, sound_keys(s), h);

    constexpr float gap = 4, top = zone::picture_y + 4, grid_w = zone::width - 2 * zone::pad;
    constexpr float w = (grid_w - 3 * gap) / 4, ch = (zone::picture_h - 8 - 3 * gap) / 4;
    auto lines = bank_lines(s.heads, bank); // what sounds: how far through, how strong
    for (unsigned n = 0; n < 16; ++n) {
        unsigned kit = bank * 16 + n, row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool last = kit == track.last_pad, held = s.ui.held_pads & (1u << n);
        bool sounding = lines[n].strength > 0 && lines[n].through > 0;
        const auto pc = pad_colour(s, kit); // its own colour (Stage's COLOUR, an MPC kit's), else the track's
        p.fill(x, y, w, ch, last ? pc : colour::surf, 3);
        if (!last && (held || sounding)) p.fill(x, y, w, ch, pc.withAlpha(0.25f), 3); // it sounds: lit
        // Its waveform, start to end, bars from the middle.
        float wx = x + 5, wy = y + 4, ww = w - 10, wh = ch - 19;
        if (s.pad_waves_shown & (1u << n)) {
            const auto& wave = s.pad_waves[n];
            float bw = ww / float(wave.size());
            auto ink = last ? ink_on(pc).withAlpha(0.9f) : pc.withAlpha(0.75f);
            for (unsigned k = 0; k < wave.size(); ++k) {
                float bh = std::max(1.0f, wave[k] / 255.0f * wh);
                p.fill(wx + k * bw, wy + (wh - bh) / 2, std::max(1.0f, bw - 0.6f), bh, ink, 0.5f);
            }
            if (sounding) p.fill(wx + ww * std::clamp(lines[n].through, 0.0f, 1.0f), wy - 1, 1.5f, wh + 2, last ? ink_on(pc) : colour::text); // the head
        }
        // Its name (a chop's slice) or label; muted, MUTE.
        char label[4]; pad_label(kit, label);
        bool muted = track.pad_muted(kit) && track.pads[kit].used();
        auto name = muted ? juce::String("MUTE") : track.pads[kit].used() ? pad_name(s, kit, true) : juce::String(label);
        auto style = plain(last ? ink_on(pc) : track.pads[kit].used() ? colour::text : colour::dim, type::small, Weight::SemiBold);
        p.text(name, x + 5, y + ch - 15, w - 10, 13, p.fit(name, style, w - 10, type::floor));
        if (muted) p.fill(x, y, w, ch, colour::bg.withAlpha(0.45f), 3);
    }
}

void bank(Context& c, int way) { c.track().bank = uint8_t((c.track().bank + sampler_banks + way) % sampler_banks); }
// Push: the sampler's ten pages as a map (Stage holds them too, but not their second rows).
void push(Context& c) {
    c.open(PageId::PadMix);
    c.ui.map_open = true;
    c.ui.map_cursor = uint8_t(sampler_index(PageId::PadMix));
}
} // namespace

const PageSpec pads_page {draw, play_pad, no_wheel, push, play_soft, bank};

} // namespace melodee
