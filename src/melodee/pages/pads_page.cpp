// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PADS (B2 on a sampler track): one sound a pad, drawn as the pads sit,
// eight banks of sixteen; Pad Bank (or Minus / Plus) steps through them and
// bank lights A-H show which. Muted pads (mute mode) are drawn dim. The pots follow the last pad hit: pitch,
// level, pan. Push opens the map of the sampler's pages for that pad.
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
    auto c = colour_of(s);
    unsigned bank = track.bank % sampler_banks;
    Header h = header_of(s);
    h.left.push_back(pill(juce::String("BANK ") + char('A' + bank)));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, play_keys(s, 2), h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f, grid_w = 196;
    constexpr float w = (grid_w - 3 * gap) / 4, ch = (zone::picture_h - 2.0f - 3 * gap) / 4;
    auto lines = bank_lines(s.heads, bank); // what sounds: a line along each pad's foot (mock 4)
    for (unsigned n = 0; n < 16; ++n) {
        unsigned kit = bank * 16 + n, row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool last = kit == track.last_pad, held = s.ui.held_pads & (1u << n);
        p.fill(x, y, w, ch, last ? c : held ? c.withAlpha(0.35f) : colour::surf, 4);
        char label[4]; pad_label(kit, label);
        bool muted = track.pad_muted(kit) && track.pads[kit].used();
        juce::String tag = muted ? juce::String("MUTE") : juce::String(label); // mute mode muted it
        auto tag_style = plain(last ? colour::bg : muted ? colour::sec : colour::dim, type::small, Weight::SemiBold);
        p.text(tag, x + 4, y + 2, w - 8, 14, tag_style);
        auto name = pad_name(s, kit, true);
        if (name.isNotEmpty())
            p.text(name, x + 4, y + ch - 16, w - 8, 14, p.fit(name, plain(last ? colour::bg : colour::text, type::small, Weight::Medium), w - 8, type::floor));
        if (lines[n].strength > 0 && lines[n].through > 0) // as far as it has played, as strong as its envelope
            p.fill(x + 4, y + ch - 3.5f, std::max(2.0f, (w - 8) * lines[n].through), 2,
                   (last ? colour::bg : c).withAlpha(lines[n].strength), 1);
        if (muted) p.fill(x, y, w, ch, colour::bg.withAlpha(0.45f), 4);
    }
    // The pad in hand.
    const auto& pad = track.pads[track.last_pad % sampler_pads];
    float x = zone::pad + grid_w + 8, y = zone::picture_y + 3, iw = zone::width - zone::pad - x;
    char label[4]; pad_label(track.last_pad, label);
    p.text(juce::String("PAD ") + juce::String(label), x, y, iw, 14, caps_text(colour::mid));
    auto name = pad.used() ? pad_name(s, track.last_pad % sampler_pads, false) : juce::String("Empty");
    auto name_style = p.fit(name, plain(pad.used() ? colour::text : colour::dim, 22, Weight::SemiBold), iw, type::floor);
    auto name_lines = p.wrap(name, name_style, iw, 2);
    for (int l = 0; l < name_lines.size(); ++l) p.text(name_lines[l], x, y + 15 + l * 24, iw, 26, name_style);
    y += (name_lines.size() > 1 ? 24 : 0);
    if (pad.used() && s.sample.frames) {
        uint32_t end = pad.end ? pad.end : s.sample.frames;
        char text[48];
        std::snprintf(text, sizeof(text), "%.3f s", (end - std::min(end, pad.start)) / 44100.0);
        p.text(juce::String(text) + juce::String::fromUTF8(" \xc2\xb7 ") + (pad.play == PadPlay::Gate ? "gate" : "one shot"),
               x, y + 41, iw, 14, plain(colour::sec, type::small));
        float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
        ui::WaveHead head; // the pad as it is heard: a head and its wake
        bool heard = pad_head(s, track.last_pad % sampler_pads, pad, s.sample.frames, head);
        waveform(p, x, y + 58, iw, 14, s.sample.overview, from, to, from, to, c, &head, heard ? 1 : 0, 1);
        if (heard) draw_heads(p, x, y + 56, iw, 18, from, to, &head, 1, false);
    } else if (pad.used() && s.sample.instrument) {
        juce::String what = juce::String(s.sample.zones) + " zones";
        if (s.sample.parts_ready + s.sample.parts_failed < s.sample.parts) what = "Reading " + juce::String(s.sample.parts_ready) + " / " + juce::String(s.sample.parts);
        else if (!s.sample.error.empty()) what = s.sample.error;
        p.text(what, x, y + 41, iw, 14, plain(colour::sec, type::small));
    } else if (pad.used()) {
        p.text(s.sample.error.empty() ? "Loading" : juce::String(s.sample.error), x, y + 41, iw, 14, plain(colour::dim, type::small));
    } else {
        auto hint = plain(colour::dim, type::small);
        auto hint_lines = p.wrap("Sample Select puts a sound here", hint, iw, 3);
        for (int l = 0; l < hint_lines.size(); ++l) p.text(hint_lines[l], x, y + 41 + l * 13, iw, 14, hint);
    }
    float by = zone::picture_y + zone::picture_h - 34;
    p.text("BANK", x, by, iw, 14, caps_text(colour::mid));
    for (unsigned b = 0; b < sampler_banks; ++b) { // A-H under the label
        float bx = x + b * iw / sampler_banks, bw = iw / sampler_banks - 1;
        if (b == bank) p.fill(bx, by + 15, bw, 15, c, 3);
        p.text(juce::String::charToString(juce::juce_wchar('A' + b)), bx, by + 15, bw, 15,
               plain(b == bank ? colour::bg : colour::dim, type::small, Weight::SemiBold), juce::Justification::centred);
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
