// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// LEVELS (16 Levels on a sampler track): the last pad hit spreads over all
// sixteen pads. B1-B3 pick what rises: velocity soft to loud, pitch up the
// project's scale, or pitch by semitones; pad 1, bottom left, is the lowest.
// The pots stay on that pad: pitch, level, pan. 16 Levels again goes back
// to the kit.
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/waveform.h"

namespace melodee {
namespace {
using namespace ui;

juce::String value_of(const Snapshot& s, unsigned pad) {
    const auto& t = s.track();
    if (t.levels_kind == 0) return juce::String(level_velocity(pad));
    int st = t.levels_kind == 1 ? level_scale_step(s.project, pad) : int(pad);
    return st ? "+" + juce::String(st) : juce::String("0");
}

void draw(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(chip("16 LEVELS", c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"VELOCITY", "SCALE", "SEMITONES"}, int(t.levels_kind % 3), c}, h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f, grid_w = 150;
    constexpr float w = (grid_w - 3 * gap) / 4, ch = (143.5f - 1.5f - 3 * gap) / 4;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool hit = int(n) == s.ui.levels_hit;
        // Lighter as they rise.
        p.fill(x, y, w, ch, colour::bg, 4);
        p.fill(x, y, w, ch, hit ? colour::text : c.withAlpha(0.2f + 0.6f * n / 15.0f), 4);
        auto fg = hit ? colour::bg : colour::text;
        p.text(juce::String(n + 1), x + 4, y + 3, w - 8, 8, plain(hit ? colour::bg : n < 4 ? colour::dim : colour::bg.withAlpha(0.8f), 6, Weight::SemiBold));
        p.text(value_of(s, n), x + 4, y + ch - 16, w - 8, 13, plain(fg, 11, Weight::Medium));
    }
    float x = zone::pad + grid_w + 10, y = zone::picture_y + 4.5f, iw = zone::width - zone::pad - x;
    char label[4]; pad_label(t.levels_pad, label);
    p.text(juce::String("PAD ") + juce::String(label) + " ON ALL 16", x, y, iw, 10, caps_text(colour::mid));
    const auto& pad = t.pads[t.levels_pad % sampler_pads];
    juce::String name = pad.used() ? juce::String(s.project.samples[pad.sample % sample_slots].name) : juce::String("Empty");
    p.text(name, x, y + 11, iw, 29, p.fit(name, plain(pad.used() ? colour::text : colour::dim, 26, Weight::SemiBold), iw, 12));
    static const char* kinds[] {"soft to loud", "up the scale", "by semitones"};
    juce::String range = t.levels_kind == 0 ? juce::String::fromUTF8(" \xc2\xb7 8 to 127")
                                             : juce::String::fromUTF8(" \xc2\xb7 0 to +") + value_of(s, 15).substring(1);
    p.text(juce::String(kinds[t.levels_kind % 3]) + range, x, y + 41, iw, 12, plain(colour::sec, 10));
    if (pad.used() && s.sample.frames) {
        uint32_t end = pad.end ? pad.end : s.sample.frames;
        float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
        waveform(p, x, y + 62, iw, 26, s.sample.overview, from, to, from, to, c, 1);
    }
    if (s.ui.levels_hit >= 0) {
        float lw = p.text("PLAYING", x, y + 100, 50, 12, caps_text(colour::mid, 7));
        p.text("Pad " + juce::String(s.ui.levels_hit + 1) + juce::String::fromUTF8(" \xc2\xb7 ") + value_of(s, unsigned(s.ui.levels_hit)),
               x + lw + 6, y + 100, iw - lw - 6, 12, plain(colour::text, 10, Weight::Medium));
    }
}

void soft(Context& c, int key) { c.track().levels_kind = uint8_t(key % 3); }
} // namespace

const PageSpec levels_page {draw, play_pad, no_wheel, no_push, soft, [](Context&, int) {}};

} // namespace melodee
