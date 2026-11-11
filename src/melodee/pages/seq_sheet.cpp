// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The Sequence sheet (Seq held a moment, shell/seq_layer.h): over any page,
// the selected track's sixteen patterns laid out as the pads are (pad 1
// bottom left), the one playing filled, the one waiting for the bar
// blinking, those with notes tinted; beside them what plays, what comes
// next and the tempo the wheel turns. B1-B3 name the editors they open, the
// knob strip the pattern's length, Q and swing.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"

namespace melodee {
using namespace ui;

namespace {
juce::String bars_text(const Snapshot& s, uint32_t length) {
    unsigned bar = std::max(1u, s.project.bar_ticks()), bars = std::max(1u, unsigned((length + bar - 1) / bar));
    return juce::String(bars) + (bars == 1 ? " bar" : " bars");
}

// A label in capitals over a value with a unit beside it: one of the column's three.
void block(Paint& p, float x, float y, float w, const juce::String& caps, const juce::String& value, const juce::String& unit,
           juce::Colour value_colour) {
    p.text(caps, x, y, w, 12, caps_text(colour::mid));
    float vw = p.text(value, x, y + 13, w, 24, plain(value_colour, type::value, Weight::SemiBold));
    if (unit.isNotEmpty()) p.text(unit, x + vw + 5, y + 21, w - vw - 5, 14, plain(colour::sec, type::body));
}
} // namespace

void draw_seq_sheet(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    unsigned t = s.ui.track % track_count;
    const auto& track = s.project.tracks[t];
    unsigned live = s.playing ? s.playing_patterns[t] % pattern_count : track.pattern % pattern_count;
    int queued = s.playing && track.pattern % pattern_count != live ? int(track.pattern % pattern_count) : -1;
    bool blink = (s.now / 150) % 2;

    p.fill(0, zone::soft_y, zone::width, zone::soft_h + zone::header_h + zone::picture_h, colour::bg);
    soft_keys(p, {{"STEPS", "ROLL", "MOTION"}, -1, c});
    Header h = header_of(s);
    h.left.push_back(chip("SEQUENCE", c));
    h.right.push_back(tag_of(s, t));
    header(p, h);

    // The patterns as the pads sit: pads 13-16 along the top, 1-4 along the bottom.
    constexpr float cell = 31, gap = 3, grid = 4 * cell + 3 * gap;
    float gx = zone::pad, gy = zone::picture_y + (zone::picture_h - grid) / 2;
    for (unsigned n = 0; n < pattern_count; ++n) {
        unsigned row = 3 - n / 4, column = n % 4;
        float x = gx + column * (cell + gap), y = gy + row * (cell + gap);
        bool notes = s.sketches[t][n].notes, playing = n == live, waiting = int(n) == queued;
        bool copying = s.ui.copying == UiState::Copying::Patterns && s.ui.copy_track == t && s.ui.copy_from == int(n);
        juce::Colour text = colour::dim;
        if (playing) { p.fill(x, y, cell, cell, c, 5); text = colour::bg; }
        else if (waiting) { p.fill(x, y, cell, cell, c.withAlpha(blink ? 0.5f : 0.15f), 5); p.ring(x, y, cell, cell, c, 5, 1.5f); text = colour::text; }
        else if (notes) { p.fill(x, y, cell, cell, c.withAlpha(0.22f), 5); text = c; }
        else p.fill(x, y, cell, cell, colour::quiet, 5);
        if (copying) p.ring(x - 1, y - 1, cell + 2, cell + 2, colour::text, 6, 1.5f);
        p.text(juce::String(n + 1), x, y, cell, cell, plain(text, type::body, Weight::SemiBold), juce::Justification::centred);
    }

    // Beside them: what plays, what comes next, the tempo.
    float x = gx + grid + 14, w = zone::width - zone::pad - x, y = gy + 2;
    block(p, x, y, w, "PLAYING", "P" + juce::String(live + 1), bars_text(s, s.sketches[t][live].length), c);
    if (queued >= 0) block(p, x, y + 44, w, "NEXT", "P" + juce::String(queued + 1), "next bar", c);
    else block(p, x, y + 44, w, "NEXT", juce::String::fromUTF8("\xe2\x80\x94"), "", colour::dim);
    juce::String tempo = juce::String(s.tempo / 10) + "." + juce::String(s.tempo % 10);
    block(p, x, y + 88, w, s.project.seq_tempo ? "P" + juce::String(s.ui.lead_pattern % pattern_count + 1) + " TEMPO" : juce::String("TEMPO"),
          tempo, s.ui.bpm_tenths ? "BPM, by 0.1" : "BPM", colour::amber);

    knob_strip(p, cards_of(s)); // K1-K3: the pattern's (shell/bindings.cpp)
}

} // namespace melodee
