// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// LENGTH (B3 on Patterns): up to 256 steps a pattern, sixteen bars, every
// step drawn as a tick. K1 moves the length by the bar (a pot has only 128
// positions), the wheel trims it by the step. K2 division, K3 swing (for the
// whole project). The white tick is the step playing.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    const auto& pattern = s.pattern;
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(chip("P" + juce::String(s.track().pattern + 1), c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"PATTERNS", "SONG", "LENGTH"}, 2, c}, h);

    unsigned length = pattern.length, bars = (length + bar_steps - 1) / bar_steps;
    float x = zone::pad, y = zone::picture_y + 3;
    float nw = p.text(juce::String(length), x, y, 120, 30, plain(colour::text, 30, Weight::SemiBold));
    juce::String detail = juce::String(length == 1 ? "step" : "steps") + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(bars)
        + (bars == 1 ? " bar at " : " bars at ") + division_name(pattern.division);
    if (length % bar_steps) detail = juce::String(length == 1 ? "step" : "steps") + " at " + division_name(pattern.division);
    p.text(detail, x + nw + 6, y + 9, zone::width - x - nw - 12, 20, plain(colour::sec, 13));

    // Sixteen bars of sixteen ticks, two rows of eight.
    constexpr float gap = 6, bw = (zone::width - 2 * zone::pad - 7 * gap) / 8, th = 17;
    int playing = s.playing ? s.steps[s.ui.track] : -1;
    for (unsigned b = 0; b < max_steps / bar_steps; ++b) {
        float bx = x + (b % 8) * (bw + gap), by = zone::picture_y + 44 + (b / 8) * 42;
        bool inside = b < bars, here = playing >= 0 && unsigned(playing) / bar_steps == b;
        p.text(juce::String(b + 1), bx, by, bw, 9, plain(here ? colour::text : inside ? colour::mid : colour::dim, 7, Weight::SemiBold));
        for (unsigned k = 0; k < bar_steps; ++k) {
            unsigned step = b * bar_steps + k;
            float tx = bx + k * (bw / bar_steps);
            auto col = step < length ? (int(step) == playing ? colour::text : c) : colour::line;
            p.fill(tx, by + 11, bw / bar_steps - 0.7f, th, col);
        }
    }
    float py = zone::picture_y + 129;
    float w1 = p.width("UP TO 256 STEPS", caps_text(c)) + 9;
    p.fill(x, py, w1, 12.5f, colour::surf, 3);
    p.text("UP TO 256 STEPS", x, py, w1, 12.5f, caps_text(c), juce::Justification::centred);
    juce::String most = "16 BARS AT " + juce::String(division_name(pattern.division));
    float w2 = p.width(most, caps_text(colour::mid)) + 9;
    p.fill(x + w1 + 4, py, w2, 12.5f, colour::surf, 3);
    p.text(most, x + w1 + 4, py, w2, 12.5f, caps_text(colour::mid), juce::Justification::centred);
}

void wheel(Context& c, int delta) { c.set_length(unsigned(std::max(1, int(c.pattern().length) + delta))); }

void soft(Context& c, int key) {
    static const PageId pages[] {PageId::Patterns, PageId::Song, PageId::Length};
    c.open(pages[key % 3]);
}
} // namespace

const PageSpec length_page {draw, play_pad, wheel, no_push, soft, octave};

} // namespace melodee
