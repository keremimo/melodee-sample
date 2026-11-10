// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// KEYS (B2): what the pads play, drawn as they sit with the roots filled.
// K1 root, K2 scale, K3 layout; the wheel turns to row 2, where K1
// transposes the pads by semitones. Minus / Plus the octave.
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
    constexpr float w = (grid_w - 3 * gap) / 4, ch = (143.5f - 1.5f - 3 * gap) / 4;
    int lowest = 127, highest = 0;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned row = 3 - pad / 4, column = pad % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        if (track.engine == EngineKind::Kit909) { // the pads are the drums
            bool held = s.ui.held_pads & (1u << pad), drum = pad < kit909::Drums;
            p.fill(x, y, w, ch, held ? c : drum ? colour::surf : colour::quiet, 4);
            p.text(juce::String(pad + 1), x + 4, y + 3, w - 8, 8, plain(held ? colour::bg : colour::dim, 6, Weight::SemiBold));
            if (drum) p.text(kit909::drum_name(pad), x + 4, y + ch - 16, w - 8, 13, p.fit(kit909::drum_name(pad), plain(held ? colour::bg : colour::text, 9, Weight::Medium), w - 8, 6));
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
        p.text(juce::String(pad + 1), x + 4, y + 3, w - 8, 8, plain(note.root ? colour::bg : colour::dim, 6, Weight::SemiBold));
        p.text(note_name(note.key), x + 4, y + ch - 16, w - 8, 13, plain(fg, 11, Weight::Medium));
    }

    float x = zone::pad + grid_w + 10, y = zone::picture_y + 4.5f, iw = zone::width - zone::pad - x;
    page_dots(p, zone::width - zone::pad, y + 5, 2, s.ui.keys_row, c, true); // row 1 / 2 (wheel)
    if (track.engine == EngineKind::Kit909) {
        p.text("PADS PLAY", x, y, iw, 10, caps_text(colour::mid));
        p.text("Drums", x, y + 11, iw, 29, plain(colour::text, 26, Weight::SemiBold));
        p.text("eleven on pads 1 to 11", x, y + 41, iw, 12, plain(colour::sec, 10));
        return;
    }
    p.text("PADS PLAY", x, y, iw, 10, caps_text(colour::mid));
    juce::String title = track.layout == PadLayout::Chromatic ? juce::String(note_letter(s.project.root)) + " Chromatic"
        : track.layout == PadLayout::Levels ? juce::String(note_name(track.last_note)) + " Levels"
        : juce::String(note_letter(s.project.root)) + " " + scales()[s.project.scale % scales().size()].name;
    float size = p.width(title, plain(colour::text, 26, Weight::SemiBold)) > iw ? 18 : 26;
    p.text(title, x, y + 11, iw, 29, plain(colour::text, size, Weight::SemiBold));
    juce::String range = track.layout == PadLayout::Levels ? juce::String("soft to loud")
        : juce::String(note_name(lowest)) + " to " + juce::String(note_name(highest)) + juce::String::fromUTF8(" \xc2\xb7 roots filled");
    p.text(range, x, y + 41, iw, 12, plain(colour::sec, 10));
    float ly = y + 64;
    for (unsigned n = 0; n < unsigned(PadLayout::Count); ++n) {
        bool on = unsigned(track.layout) == n;
        if (on) p.circle(x + 2, ly + 6, 2, c);
        p.text(layout_name(PadLayout(n)), x + 9, ly, iw - 9, 12, plain(on ? colour::text : colour::dim, 10, Weight::Medium));
        ly += 15.5f;
    }
}
void rows(Context& c, int delta) { c.ui.keys_row = uint8_t(std::clamp(c.ui.keys_row + delta, 0, 1)); }
} // namespace

const PageSpec keys_page {draw, play_pad, rows, no_push, play_soft, octave};

} // namespace melodee
