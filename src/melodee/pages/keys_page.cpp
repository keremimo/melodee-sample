// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// KEYS (B2): what the pads play, drawn as they sit with the roots filled.
// K1 root, K2 scale, K3 layout; Shift + K3 transposes the pads by
// semitones (the card's Shift mark says so). Minus / Plus the octave.
// Push opens Edit, the engine's every page.
#include "melodee/engine/kit909.h"
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(pill("OCT " + juce::String(track.octave)));
    if (track.transpose) h.left.push_back(pill((track.transpose > 0 ? "+" : juce::String::fromUTF8("\xe2\x88\x92")) + juce::String(std::abs(track.transpose)) + " ST"));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, play_keys(s, 1), h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f, grid_w = 150;
    constexpr float w = (grid_w - 3 * gap) / 4, ch = (zone::picture_h - 2.0f - 3 * gap) / 4;
    int lowest = 127, highest = 0;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned row = 3 - pad / 4, column = pad % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        if (track.engine == EngineKind::Kit909) { // the pads are the drums
            bool held = s.ui.held_pads & (1u << pad), drum = pad < kit909::Drums;
            p.fill(x, y, w, ch, held ? c : drum ? colour::surf : colour::quiet, 4);
            p.text(juce::String(pad + 1), x + 4, y + 3, w - 8, 8, plain(held ? colour::bg : colour::dim, type::small, Weight::SemiBold));
            if (drum) p.text(kit909::drum_name(pad), x + 4, y + ch - 16, w - 8, 13, p.fit(kit909::drum_name(pad), plain(held ? colour::bg : colour::text, type::small, Weight::Medium), w - 8, type::floor));
            continue;
        }
        auto note = pad_note(s.project, track, pad);
        lowest = std::min(lowest, note.key); highest = std::max(highest, note.key);
        bool held = s.ui.held_pads & (1u << pad);
        auto fill = note.root ? c : held ? c.withAlpha(0x55 / 255.0f) : colour::surf;
        if (held && note.root) fill = colour::text;
        p.fill(x, y, w, ch, colour::bg, 4);
        p.fill(x, y, w, ch, fill, 4);
        auto fg = note.root || (held && note.root) ? colour::bg : colour::text;
        p.text(juce::String(pad + 1), x + 4, y + 3, w - 8, 8, plain(note.root ? colour::bg : colour::dim, type::small, Weight::SemiBold));
        p.text(note_name(note.key), x + 4, y + ch - 16, w - 8, 13, plain(fg, type::small, Weight::Medium));
    }

    float x = zone::pad + grid_w + 12, y = zone::picture_y + 4, iw = zone::width - zone::pad - x;
    if (track.engine == EngineKind::Kit909) { // the drums on pads 1-11
        p.text("Drums", x, y, iw, 29, plain(colour::text, 26, Weight::SemiBold));
        icon(p, Icon::Pads, x, y + 36, 14, colour::sec, 1.5f);
        p.text(juce::String(int(kit909::Drums)), x + 20, y + 35, iw - 20, 16, plain(colour::sec, type::body, Weight::SemiBold));
        return;
    }
    // What the pads play: the root in the track colour, then the scale (or Chromatic, or the note of 16 Levels).
    juce::String root = track.layout == PadLayout::Levels ? juce::String(note_name(track.last_note)) : juce::String(note_letter(s.project.root));
    juce::String rest = track.layout == PadLayout::Chromatic ? juce::String("Chromatic") : track.layout == PadLayout::Levels ? juce::String("Levels")
                      : juce::String(scales()[s.project.scale % scales().size()].name);
    auto big = plain(colour::text, 22, Weight::SemiBold);
    if (p.width(root + " " + rest, big) > iw) big.size = 17;
    float rw = p.text(root, x, y, iw, 28, plain(c, big.size, Weight::SemiBold));
    p.text(rest, x + rw + 6, y, iw - rw - 6, 28, big);

    // The keyboard from the lowest pad's octave to the highest's: the keys the pads play lit, the roots in the track colour.
    if (track.layout != PadLayout::Levels && highest >= lowest) {
        std::array<bool, 128> played {};
        for (unsigned pad = 0; pad < 16; ++pad) played[size_t(std::clamp(pad_note(s.project, track, pad).key, 0, 127))] = true;
        auto is_white = [](int k) { int n = ((k % 12) + 12) % 12; return n != 1 && n != 3 && n != 6 && n != 8 && n != 10; };
        int from = lowest - ((lowest % 12) + 12) % 12, to = highest + (11 - ((highest % 12) + 12) % 12);
        int whites = 0;
        for (int k = from; k <= to; ++k) whites += is_white(k);
        float kw = iw / std::max(1, whites), ky = y + 38, kh = 28;
        int at = 0;
        for (int k = from; k <= to; ++k) {
            if (!is_white(k)) continue;
            bool on = k >= 0 && k < 128 && played[size_t(k)], root_key = on && ((k % 12) + 12) % 12 == int(s.project.root % 12);
            p.fill(x + at * kw, ky, kw - 1, kh, root_key ? c : on ? colour::sec : colour::dim.withAlpha(0.5f), 1);
            ++at;
        }
        at = 0;
        for (int k = from; k <= to; ++k) {
            if (is_white(k)) { ++at; continue; }
            bool on = k >= 0 && k < 128 && played[size_t(k)], root_key = on && ((k % 12) + 12) % 12 == int(s.project.root % 12);
            p.fill(x + at * kw - kw * 0.3f - 0.5f, ky, kw * 0.6f, kh * 0.6f, root_key ? c : on ? colour::mid : colour::bg, 1);
        }
        auto ends = plain(colour::sec, type::small, Weight::SemiBold);
        p.text(note_name(lowest), x, ky + kh + 2, iw / 2, 14, ends);
        p.text(note_name(highest), x + iw / 2, ky + kh + 2, iw / 2, 14, ends, juce::Justification::centredRight);
    }

    // The layouts K3 picks, as glyphs, the one in use lit.
    const Icon layouts[] {Icon::Scale, Icon::Chromatic, Icon::Chord, Icon::Velocity};
    float tw = std::min(32.0f, (iw - 3 * 4) / 4), ty = zone::picture_y + zone::picture_h - tw - 4;
    for (unsigned n = 0; n < unsigned(PadLayout::Count) && n < 4; ++n) {
        bool on = unsigned(track.layout) == n;
        float tx = x + n * (tw + 4);
        p.fill(tx, ty, tw, tw, on ? c.withAlpha(0.16f) : colour::surf, 4);
        if (on) p.ring(tx, ty, tw, tw, c, 4, 1.2f);
        icon(p, layouts[n], tx + tw / 2 - 9, ty + tw / 2 - 9, 18, on ? c : colour::mid, 1.6f);
    }
}
} // namespace

void push(Context& c) { if (c.track().used()) c.open(PageId::Edit); } // the engine's every page (Stage holds the quick ones)
const PageSpec keys_page {draw, play_pad, no_wheel, push, play_soft, octave};

} // namespace melodee
