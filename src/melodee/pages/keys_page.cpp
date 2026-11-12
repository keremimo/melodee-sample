// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// KEYS (Sample's third stop on a synth): what the pads play, the sixteen
// pads filling the picture as they sit: each its note large and its degree
// in the scale small, the roots filled in the track's colour, the pad played
// white. The key and scale are the header's chip (Chromatic, or 16 Levels'
// note, when the layout is that). K1 root, K2 scale, K3 layout; Shift + K3
// transposes the pads by semitones. Minus / Plus the octave. Push opens
// Edit, the engine's every page.
//
// Drums (K3's last layout; a drum plug-in picks it itself): the pads as a
// kit's, notes 36 up whatever the key, each named by the plug-in's voice
// there (else its note, a pad without a voice dark), the drum hit last in the
// track's colour. The chip says DRUMS, the pill the bank; Minus / Plus (and
// Pad Bank) step the bank, A-D.
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
    bool drums = drum_pads(track);
    if (drums) {
        h.left.push_back(chip("DRUMS", c));
        h.left.push_back(pill(juce::String("BANK ") + juce::String::charToString(juce::juce_wchar('A' + drum_bank(track)))));
    } else { // the key the pads play in: the root and the scale (or Chromatic, or 16 Levels' note)
        juce::String key = track.layout == PadLayout::Levels ? juce::String(note_name(track.last_note)) + " LEVELS"
                         : juce::String(note_letter(s.project.root)) + " " + (track.layout == PadLayout::Chromatic ? juce::String("Chromatic")
                                                                                                                    : juce::String(scales()[s.project.scale % scales().size()].name));
        h.left.push_back(chip(key, c));
        juce::String octave = "OCT " + juce::String(track.octave);
        if (track.transpose) octave += juce::String::fromUTF8(" \xc2\xb7 ") + (track.transpose > 0 ? "+" : juce::String::fromUTF8("\xe2\x88\x92")) + juce::String(std::abs(track.transpose)) + " st";
        h.left.push_back(pill(octave));
    }
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, play_keys(s, 2), h);

    std::array<uint8_t, 12> degrees {};
    unsigned in_scale = scale_degrees(s.project.scale, degrees);
    auto degree_of = [&](int key) { // its place in the scale, 1-based; 0 outside it
        int offset = ((key - int(s.project.root)) % 12 + 12) % 12;
        for (unsigned d = 0; d < in_scale; ++d) if (degrees[d] == offset) return int(d) + 1;
        return 0;
    };
    constexpr float gap = 4, top = zone::picture_y + 2;
    constexpr float w = (zone::width - 2 * zone::pad - 3 * gap) / 4, ch = (zone::picture_h - 4 - 3 * gap) / 4;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned row = 3 - pad / 4, column = pad % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool held = s.ui.held_pads & (1u << pad);
        auto note = pad_note(s.project, track, pad);
        if (drums) { // the voice there by name, else its note; no voice at all: dark
            const char* voice = s.ui.drum_voices.at(note.key);
            bool last = note.key == int(track.last_note), silent = !voice && s.ui.drum_voices.any();
            p.fill(x, y, w, ch, held ? colour::text : last ? c : silent ? colour::quiet : colour::surf, 3);
            juce::String label = voice ? juce::String(voice) : juce::String(note_name(note.key));
            auto style = plain(held || last ? colour::bg : silent ? colour::dim : colour::text, type::body, Weight::SemiBold);
            p.text(label, x + 7, y, w - 14, ch, p.fit(label, style, w - 14, type::floor));
            continue;
        }
        p.fill(x, y, w, ch, held ? colour::text : note.root ? c : colour::surf, 3);
        auto fg = held || note.root ? colour::bg : colour::text;
        p.text(note_name(note.key), x + 7, y, w - 30, ch, plain(fg, type::list, Weight::SemiBold));
        int degree = track.layout == PadLayout::Chromatic ? 0 : degree_of(note.key);
        if (degree) p.text(juce::String(degree), x + w - 27, y + 3, 20, 14, plain(held || note.root ? colour::bg.withAlpha(0.7f) : colour::dim, type::small, Weight::SemiBold),
                           juce::Justification::centredRight);
    }
}
} // namespace

void push(Context& c) { if (c.track().used()) c.open(PageId::Edit); } // the engine's every page (Stage holds the quick ones)
const PageSpec keys_page {draw, play_pad, no_wheel, push, play_soft, octave};

} // namespace melodee
