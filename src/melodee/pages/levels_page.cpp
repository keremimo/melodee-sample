// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// LEVELS (16 Levels): on a kit the last pad hit spreads over all sixteen
// pads, on a synth its last note. The type (Shift + 16 Levels goes round
// them, B1-B3 pick): Velocity soft to loud; Filter (kit pads) the cutoff
// from the bottom up to the pad's own over pads 1-8 and on to the top over
// 9-16; Tune by semitones, the original on pad 4; and Melodee's Scale, up
// the project's scale (B3 again on Tune, or a synth's B2). Pad 1 is bottom
// left. The pots stay on that pad. 16 Levels again goes back.
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/waveform.h"

namespace melodee {
namespace {
using namespace ui;

LevelsKind kind_of(const Track& t) { return LevelsKind(t.levels_kind % unsigned(LevelsKind::Count)); }

juce::String value_of(const Snapshot& s, unsigned pad) {
    auto level = levels_of(s.project, s.track(), pad);
    switch (kind_of(s.track())) {
    case LevelsKind::Filter: if (s.track().engine == EngineKind::Sampler) return juce::String(level.cutoff); [[fallthrough]];
    case LevelsKind::Velocity: return juce::String(level.velocity);
    default: return level.semitones > 0 ? "+" + juce::String(level.semitones) : juce::String(level.semitones);
    }
}

void draw(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    bool kit = t.engine == EngineKind::Sampler;
    auto c = colour_of(s);
    auto kind = kind_of(t);
    Header h = header_of(s);
    h.left.push_back(chip("16 LEVELS", c));
    h.right.push_back(tag_of(s, s.ui.track));
    int active = kind == LevelsKind::Velocity ? 0 : kind == LevelsKind::Tune ? 2 : kind == LevelsKind::Filter ? 1 : kit ? 2 : 1;
    frame(p, s, {{"VELOCITY", kit ? "FILTER" : "SCALE", kind == LevelsKind::Scale && kit ? "SCALE" : "TUNE"}, active, c}, h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f, grid_w = 150;
    constexpr float w = (grid_w - 3 * gap) / 4, ch = (143.5f - 1.5f - 3 * gap) / 4;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool hit = int(n) == s.ui.levels_hit;
        bool original = kind == LevelsKind::Tune && n == 3;
        // Lighter as they rise.
        p.fill(x, y, w, ch, colour::bg, 4);
        p.fill(x, y, w, ch, hit ? colour::text : c.withAlpha(0.2f + 0.6f * n / 15.0f), 4);
        if (original && !hit) p.ring(x, y, w, ch, colour::text, 4, 1);
        auto fg = hit ? colour::bg : colour::text;
        p.text(juce::String(n + 1), x + 4, y + 3, w - 8, 8, plain(hit ? colour::bg : n < 4 ? colour::dim : colour::bg.withAlpha(0.8f), 6, Weight::SemiBold));
        p.text(value_of(s, n), x + 4, y + ch - 16, w - 8, 13, plain(fg, 11, Weight::Medium));
    }
    float x = zone::pad + grid_w + 10, y = zone::picture_y + 4.5f, iw = zone::width - zone::pad - x;
    juce::String name;
    const PadSound* pad = nullptr;
    if (kit) {
        char label[4]; pad_label(t.levels_pad, label);
        p.text(juce::String("PAD ") + juce::String(label) + " ON ALL 16", x, y, iw, 10, caps_text(colour::mid));
        pad = &t.pads[t.levels_pad % sampler_pads];
        name = pad->used() ? juce::String(s.project.samples[pad->sample % sample_slots].name) : juce::String("Empty");
    } else {
        p.text("THE LAST NOTE ON ALL 16", x, y, iw, 10, caps_text(colour::mid));
        name = juce::String(note_name(t.last_note));
    }
    bool lit = kit ? pad->used() : true;
    p.text(name, x, y + 11, iw, 29, p.fit(name, plain(lit ? colour::text : colour::dim, 26, Weight::SemiBold), iw, 12));
    juce::String range;
    switch (kind) {
    case LevelsKind::Velocity: range = juce::String::fromUTF8("soft to loud \xc2\xb7 8 to 127"); break;
    case LevelsKind::Filter: range = kit ? juce::String::fromUTF8("its cutoff swept \xc2\xb7 pad 8 its own")
                                         : juce::String::fromUTF8("soft to loud \xc2\xb7 8 to 127"); break;
    case LevelsKind::Tune: range = juce::String::fromUTF8("by semitones \xc2\xb7 pad 4 as it is"); break;
    default: range = juce::String::fromUTF8("up the scale \xc2\xb7 0 to +") + value_of(s, 15).substring(1); break;
    }
    p.text(range, x, y + 41, iw, 12, plain(colour::sec, 10));
    if (pad && pad->used() && s.sample.frames) {
        uint32_t end = pad->end ? pad->end : s.sample.frames;
        float from = float(pad->start) / s.sample.frames, to = float(end) / s.sample.frames;
        waveform(p, x, y + 62, iw, 26, s.sample.overview, from, to, from, to, c, 1);
    }
    if (s.ui.levels_hit >= 0) {
        float lw = p.text("PLAYING", x, y + 100, 50, 12, caps_text(colour::mid, 7));
        p.text("Pad " + juce::String(s.ui.levels_hit + 1) + juce::String::fromUTF8(" \xc2\xb7 ") + value_of(s, unsigned(s.ui.levels_hit)),
               x + lw + 6, y + 100, iw - lw - 6, 12, plain(colour::text, 10, Weight::Medium));
    }
}

// B1 Velocity; B2 Filter (a kit) or Scale (a synth); B3 Tune, and again Scale.
void soft(Context& c, int key) {
    auto& t = c.track();
    bool kit = t.engine == EngineKind::Sampler;
    auto kind = LevelsKind(t.levels_kind % unsigned(LevelsKind::Count));
    LevelsKind next = key == 0 ? LevelsKind::Velocity
                    : key == 1 ? (kit ? LevelsKind::Filter : LevelsKind::Scale)
                    : (kit && kind == LevelsKind::Tune ? LevelsKind::Scale : LevelsKind::Tune);
    t.levels_kind = uint8_t(next);
    c.ui.levels_hit = -1;
}
} // namespace

const PageSpec levels_page {draw, play_pad, no_wheel, no_push, soft, [](Context&, int) {}};

} // namespace melodee
