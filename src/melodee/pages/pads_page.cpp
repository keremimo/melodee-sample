// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PADS (B2 on a sampler track): one sound a pad, drawn as the pads sit,
// four banks of sixteen; Pad Bank (or Minus / Plus) steps through them and
// bank lights A-D show which. The pots follow the last pad hit: pitch,
// level, pan. Push opens Trim for that pad.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/waveform.h"
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;

// A pad's name: its sample's, or for pads sharing one sample (a chop), the
// last word of it and which slice: "Break 5" in full, "Brk 5" on a pad.
juce::String pad_name(const Snapshot& s, unsigned kit, bool short_form) {
    const auto& pads = s.track().pads;
    const auto& pad = pads[kit];
    if (!pad.used()) return {};
    juce::String name = s.project.samples[pad.sample % sample_slots].name;
    unsigned index = 0, sharing = 0;
    for (unsigned n = 0; n < sampler_pads; ++n)
        if (pads[n].sample == pad.sample) { ++sharing; if (n <= kit) ++index; }
    if (sharing < 2) return name;
    // The last word with letters in it ("LOOP 1 001" gives Loop).
    juce::StringArray words;
    words.addTokens(name, " ", "");
    juce::String word = name;
    for (int k = words.size() - 1; k >= 0; --k)
        if (words[k].containsAnyOf("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")) { word = words[k]; break; }
    word = word.toLowerCase();
    word = word.substring(0, 1).toUpperCase() + word.substring(1);
    if (short_form) { // the first letter and the next two consonants
        juce::String brief = word.substring(0, 1);
        for (int k = 1; k < word.length() && brief.length() < 3; ++k)
            if (!juce::String("aeiou").containsChar(word[k])) brief += word[k];
        word = brief;
    }
    return word + " " + juce::String(index);
}

void draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    auto c = colour_of(s);
    unsigned bank = track.bank % sampler_banks;
    Header h = header_of(s);
    h.left.push_back(pill(juce::String("BANK ") + char('A' + bank)));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, play_keys(s, 1), h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f, grid_w = 150;
    constexpr float w = (grid_w - 3 * gap) / 4, ch = (143.5f - 1.5f - 3 * gap) / 4;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned kit = bank * 16 + n, row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool last = kit == track.last_pad, held = s.ui.held_pads & (1u << n);
        p.fill(x, y, w, ch, last ? c : held ? c.withAlpha(0.35f) : colour::surf, 4);
        char label[4]; pad_label(kit, label);
        p.text(label, x + 4, y + 3, w - 8, 8, plain(last ? colour::bg : colour::dim, 6, Weight::SemiBold));
        auto name = pad_name(s, kit, true);
        if (name.isNotEmpty())
            p.text(name, x + 4, y + ch - 15, w - 8, 12, p.fit(name, plain(last ? colour::bg : colour::text, 9, Weight::Medium), w - 8, 6));
    }
    // The pad in hand.
    const auto& pad = track.pads[track.last_pad % sampler_pads];
    float x = zone::pad + grid_w + 10, y = zone::picture_y + 4.5f, iw = zone::width - zone::pad - x;
    char label[4]; pad_label(track.last_pad, label);
    p.text(juce::String("PAD ") + juce::String(label), x, y, iw, 10, caps_text(colour::mid));
    auto name = pad.used() ? pad_name(s, track.last_pad % sampler_pads, false) : juce::String("Empty");
    p.text(name, x, y + 11, iw, 29, p.fit(name, plain(pad.used() ? colour::text : colour::dim, 26, Weight::SemiBold), iw, 12));
    if (pad.used() && s.sample.frames) {
        uint32_t end = pad.end ? pad.end : s.sample.frames;
        char text[48];
        std::snprintf(text, sizeof(text), "%.3f s", (end - std::min(end, pad.start)) / 44100.0);
        p.text(juce::String(text) + juce::String::fromUTF8(" \xc2\xb7 ") + (pad.play == PadPlay::Gate ? "gate" : "one shot"),
               x, y + 41, iw, 12, plain(colour::sec, 10));
        float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
        waveform(p, x, y + 60, iw, 14, s.sample.overview, from, to, from, to, c, 1);
    } else if (pad.used()) {
        p.text(s.sample.error.empty() ? "Loading" : juce::String(s.sample.error), x, y + 41, iw, 12, plain(colour::dim, 10));
    } else {
        p.text("Sample Select puts a sound here", x, y + 41, iw, 12, plain(colour::dim, 9));
    }
    p.text("BANK", x, y + 92, 30, 12, caps_text(colour::mid, 7));
    for (unsigned b = 0; b < sampler_banks; ++b) {
        float bx = x + 30 + b * 16;
        if (b == bank) p.fill(bx, y + 92, 12, 12, c, 3);
        p.text(juce::String::charToString(juce::juce_wchar('A' + b)), bx, y + 92, 12, 12,
               plain(b == bank ? colour::bg : colour::dim, 8, Weight::SemiBold), juce::Justification::centred);
    }
}

void bank(Context& c, int way) { c.track().bank = uint8_t((c.track().bank + sampler_banks + way) % sampler_banks); }
void push(Context& c) { if (c.pad().used()) c.open(PageId::Trim); }
} // namespace

const PageSpec pads_page {draw, play_pad, no_wheel, push, play_soft, bank};

} // namespace melodee
