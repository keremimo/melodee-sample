// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PATTERNS (Seq, the MPC's Sequence mode): tracks down, the sixteen patterns
// of the bank in view across (A-H, Seq held + Pad Bank; the header names it),
// eight tracks in view following the selected one. The pads play
// the selected track, so it can be played and recorded while its patterns
// are in view; hold Seq for the pads to launch them (shell/seq_layer.h).
// K1 the selected pattern's length (1, 2, 4 .. 128 bars; Shift + K1 any
// bar count), K2 its Q (Shift + K2 the project's time signature), K3 swing
// (Shift + K3 the metronome's level). B1-B3 open Steps, Roll and Motion, as
// on every sequence page; Seq again opens the one last used on the track.
// The wheel moves the playhead by a beat (Shift: by Q); stopped, it sets
// where Shift + Play carries on from (the place bar shows it). The header
// shows Rec Quantize (Shift + Pad 10) and the track. Minus / Plus
// move the track. Tempo: Seq held + wheel; per pattern or global on the
// Tempo page (Push). Push opens SCENES (roadmap P5); a pattern with a follow action carries an arrow.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    unsigned first = s.ui.track / 8 * 8;
    Header h = header_of(s);
    // Rec Quantize's grid (Shift + Pad 10): the pattern's Q or its own; the position is the place bar's.
    h.left.push_back(pill(Icon::Magnet, s.ui.settings.rec_quantize ? juce::String(division_name(grid_division(s.ui.settings.rec_grid, s.pattern.division)))
                                                                 : juce::String("OFF"),
                          s.ui.settings.rec_quantize ? colour::mid : colour::dim));
    if (s.track().speed != speed_normal) // the track plays its patterns at a speed (Tracks, Shift + K1: roadmap P1)
        h.right.push_back(pill(juce::String::fromUTF8(speed_name(s.track().speed)), colour_of(s)));
    h.left.push_back(bank_pill(s));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"STEPS", "ROLL", "MOTION"}, -1, colour_of(s)}, h);

    constexpr float gap = 2, label_w = 16, head_h = 9, top = zone::picture_y + 1.5f;
    constexpr float cw = (zone::width - 2 * zone::pad - label_w - 16 * gap) / 16;
    constexpr float rh = (zone::picture_h - 2.0f - head_h - 8 * gap) / 8;
    unsigned shown = s.playing ? s.playing_patterns[s.ui.track] : s.track().pattern;
    // The columns' numbers; three digits (banks G-H) fit over two columns only: one a pair, the one playing if it is
    // in that pair, centred over both.
    bool wide = s.ui.pad_pattern(pattern_pads - 1) + 1 >= 100;
    for (unsigned k = 0; k < pattern_pads; ++k) {
        unsigned n = s.ui.pad_pattern(k);
        if (!wide) {
            p.text(juce::String(n + 1), zone::pad + label_w + gap + k * (cw + gap), top, cw, head_h,
                   plain(n == shown ? colour::text : colour::dim, type::small, Weight::SemiBold), juce::Justification::centred);
            continue;
        }
        if (k % 2) continue;
        unsigned label = n + 1 == shown ? n + 1 : n;
        p.text(juce::String(label + 1), zone::pad + label_w + gap + k * (cw + gap), top, 2 * cw + gap, head_h,
               plain(label == shown ? colour::text : colour::dim, type::small, Weight::SemiBold), juce::Justification::centred);
    }
    for (unsigned row = 0; row < 8; ++row) {
        unsigned t = first + row;
        const auto& track = s.project.tracks[t];
        auto c = track_colour(t);
        float y = top + head_h + gap + row * (rh + gap);
        bool selected = t == s.ui.track;
        if (selected) p.fill(zone::pad, y, label_w, rh, c, 3);
        p.text(juce::String(t + 1), zone::pad, y, label_w, rh, plain(selected ? colour::bg : c, type::small, Weight::Bold),
               juce::Justification::centred);
        unsigned live = s.playing ? s.playing_patterns[t] : track.pattern;
        for (unsigned k = 0; k < pattern_pads; ++k) {
            unsigned n = s.ui.pad_pattern(k);
            float x = zone::pad + label_w + gap + k * (cw + gap);
            const auto& pattern = s.sketches[t][n];
            bool notes = track.used() && pattern.notes;
            bool playing = track.used() && n == live && notes;
            bool queued = track.used() && s.playing && n == track.pattern && n != live;
            if (playing) {
                p.fill(x, y, cw, rh, c, 3);
                int position = s.positions[t];
                bool here = s.playing ? position >= 0 : selected; // the playhead, or where Shift + Play carries on from
                if (here) {
                    float done = float((s.playing ? uint32_t(position) : s.ui.locate) % std::max<uint32_t>(pattern.length, 1) + 1) / pattern.length;
                    p.g.saveState();
                    p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), int(cw + 1), int(rh)));
                    p.fill(x, y + rh - 2.5f, cw * done, 2.5f, colour::bg.withAlpha(0.5f));
                    p.g.restoreState();
                }
            } else if (queued) {
                p.fill(x, y, cw, rh, c.withAlpha(0.2f), 3);
                p.ring(x, y, cw, rh, c, 3);
            } else if (notes) {
                p.fill(x, y, cw, rh, c.withAlpha(0.2f), 3);
                const float widths[] {8, 5, 6.5f};
                for (unsigned k = 0; k < 3; ++k) p.fill(x + 3, y + rh / 2 - 4 + k * 3, std::min(widths[k], cw - 5), 1.5f, c, 0.75f);
            } else {
                p.fill(x, y, cw, rh, colour::quiet, 3);
            }
            if (notes && pattern.follows) // roadmap P5: it follows on by itself
                p.fill(x + cw - 4, y + 2, 2.5f, 2.5f, playing ? colour::bg : colour::text, 1.25f);
            // BPM per pattern: a pattern with a tempo of its own carries a dot.
            if (s.project.seq_tempo && s.project.pattern_tempo[n] && row == 0)
                p.fill(x + cw / 2 - 1, top + head_h - 1.5f, 2, 2, colour::text, 1);
        }
    }
}

// Push: SCENES, the columns as scenes and the patterns' follow actions (roadmap P5).
void push(Context& c) { c.open(PageId::Scenes); }

// The wheel: the playhead (Seq held turns the tempo instead: shell/seq_layer.cpp).
void wheel(Context& c, int delta) { c.locate(delta); }

void nudge(Context& c, int way) { c.select_track(unsigned(std::clamp<int>(c.ui.track + way, 0, int(track_count) - 1))); }

void soft(Context& c, int key) {
    static const PageId editors[] {PageId::Steps, PageId::Roll, PageId::Motion};
    c.open(editors[key % 3]);
}
} // namespace

const PageSpec patterns_page {draw, play_pad, wheel, push, soft, nudge};

} // namespace melodee
