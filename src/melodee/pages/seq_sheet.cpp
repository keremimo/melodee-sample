// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The Sequence sheet (Seq held a moment, shell/seq_layer.h): over any page,
// the selected track's patterns of the bank in view (Pad Bank steps it, the
// header names it) laid out as the pads are (pad 1 bottom left), the one playing filled, the one waiting for the bar
// blinking, those with notes tinted; beside them the chain (round 2,
// seq-sheet2 B): what plays now, over what comes next, over what follows
// (a chain of more than three shows its first three and a count; each
// chained pad carries its place), and the tempo the wheel turns under them.
// B1-B3 name the editors they open, the knob strip the pattern's length, Q
// and swing.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"
#include <cmath>

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
    soft_keys(p, {{"PATTERNS", "ROLL", "MOTION"}, -1, c});
    Header h = header_of(s);
    h.left.push_back(chip("SEQUENCE", c));
    const bool printing = s.ui.seq_print; // freeze plan F2c: Seq held + Sample Record, the pads freeze or thaw
    const auto& frozen = s.freeze[t];
    h.left.push_back(printing ? chip(Icon::Snow, "PRINT", colour::ice) : bank_pill(s));
    h.right.push_back(tag_of(s, t));
    header(p, h);

    // The patterns as the pads sit: pads 13-16 along the top, 1-4 along the bottom.
    constexpr float cell = 31, gap = 3, grid = 4 * cell + 3 * gap;
    float gx = zone::pad, gy = zone::picture_y + (zone::picture_h - grid) / 2;
    for (unsigned k = 0; k < pattern_pads; ++k) {
        unsigned n = s.ui.pad_pattern(k), row = 3 - k / 4, column = k % 4;
        float x = gx + column * (cell + gap), y = gy + row * (cell + gap);
        bool notes = s.sketches[t][n].notes, playing = n == live, waiting = int(n) == queued;
        bool copying = s.ui.copying == UiState::Copying::Patterns && s.ui.copy_track == t && s.ui.copy_from == int(n);
        juce::Colour text = colour::dim;
        bool ice = notes && (printing ? s.ui.seq_frozen_at(n) : frozen.frozen() && frozen.plays(n)); // frozen, or to be
        if (ice && !playing) { p.fill(x, y, cell, cell, colour::ice.withAlpha(0.2f), 5); text = colour::ice; }
        else if (playing) { p.fill(x, y, cell, cell, c, 5); text = colour::bg; }
        else if (waiting) { p.fill(x, y, cell, cell, c.withAlpha(blink ? 0.5f : 0.15f), 5); p.ring(x, y, cell, cell, c, 5, 1.5f); text = colour::text; }
        else if (notes) { p.fill(x, y, cell, cell, c.withAlpha(0.22f), 5); text = c; }
        else p.fill(x, y, cell, cell, colour::quiet, 5);
        if (copying) p.ring(x - 1, y - 1, cell + 2, cell + 2, colour::text, 6, 1.5f);
        p.text(juce::String(n + 1), x, y, cell, cell, plain(text, type::body, Weight::SemiBold), juce::Justification::centred);
        if (ice && frozen.state == FreezeState::Deep && !printing) { // freeze plan F2d
            if (playing) deep_snow(p, x + cell - 12.5f, y + 2.5f, 10, colour::bg, c, 1.3f);
            else icon(p, Icon::DeepSnow, x + cell - 12.5f, y + 2.5f, 10, colour::ice, 1.3f);
        } else if (ice) icon(p, Icon::Snow, x + cell - 11.5f, y + 2.5f, 9, playing ? colour::bg : colour::ice, 1.4f);
        if (track.chained()) // its place in the chain, small in the corner
            for (unsigned k = 0; k < track.chain_count; ++k)
                if (track.chain[k] == n) {
                    p.text(juce::String(k + 1), x + 2, y + 1, 12, 12, plain(playing ? colour::bg : colour::amber, type::small, Weight::Bold));
                    break;
                }
    }

    float x = gx + grid + 14, w = zone::width - zone::pad - x, y = gy + 2;
    if (printing) { // beside them: how many are frozen, what that saves, the tempo
        unsigned with = 0, ice = 0;
        for (unsigned n = 0; n < pattern_count; ++n) { with += s.sketches[t][n].notes; ice += s.sketches[t][n].notes && s.ui.seq_frozen_at(n); }
        block(p, x, y, w, "FROZEN", juce::String(ice), "of " + juce::String(with), colour::ice);
        float load = s.machine.track_load[t];
        float saved = frozen.frozen() ? (s.live_load[t] > 0 ? std::max(0.0f, s.live_load[t] - load) : -1) : ice ? load : -1; // live: what freezing saves
        block(p, x, y + 44, w, "CPU", saved < 0 ? juce::String::fromUTF8("\xe2\x80\x94") : juce::String::fromUTF8("\xe2\x88\x92") + juce::String(int(std::lround(saved * 100))),
              saved < 0 ? juce::String() : juce::String("% of a block"), colour::ice);
        juce::String tempo = juce::String(s.tempo / 10) + "." + juce::String(s.tempo % 10);
        block(p, x, y + 88, w, "TEMPO", tempo, "BPM", colour::amber);
        knob_strip(p, cards_of(s));
        return;
    }
    // Beside them, the chain: what plays now, what comes next, what follows; the tempo under them.
    block(p, x, y, w, "NOW", "P" + juce::String(live + 1), bars_text(s, s.sketches[t][live].length), c);
    auto dash = juce::String::fromUTF8("\xe2\x80\x94");
    int next = queued, after = -1;
    if (track.chained()) { next = track.chain_ahead(1); after = track.chain_ahead(2); }
    auto row = [&](float ry, const char* caps, int pattern, const juce::String& sub) {
        p.text(caps, x, ry, 48, 18, caps_text(colour::mid));
        auto value = pattern >= 0 ? "P" + juce::String(pattern + 1) : dash;
        auto style = plain(pattern >= 0 ? c : colour::dim, type::list, Weight::SemiBold);
        float vw = p.text(value, x + 48, ry, 40, 18, style);
        if (sub.isNotEmpty() && pattern >= 0) p.text(sub, x + 52 + vw, ry + 2, w - 56 - vw, 14, p.fit(sub, plain(colour::sec, type::small), w - 56 - vw, type::floor));
    };
    row(y + 42, "NEXT", next, track.chained() ? juce::String("at its end") : juce::String("next bar"));
    row(y + 64, "AFTER", after, track.chain_count > 3 ? "+" + juce::String(track.chain_count - 3) : juce::String());
    juce::String tempo = juce::String(s.tempo / 10) + "." + juce::String(s.tempo % 10);
    block(p, x, y + 90, w, s.project.seq_tempo ? "P" + juce::String(s.ui.lead_pattern % pattern_count + 1) + " TEMPO" : juce::String("TEMPO"),
          tempo, s.ui.bpm_tenths ? "BPM, by 0.1" : "BPM", colour::amber);

    knob_strip(p, cards_of(s)); // K1-K3: the pattern's (shell/bindings.cpp)
}

} // namespace melodee
