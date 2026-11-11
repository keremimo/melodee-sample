// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// LEVELS (16 Levels): on a kit the last pad hit spreads over all sixteen
// pads, on a synth its last note. The type (Shift + 16 Levels goes round
// them, B1-B3 pick): Velocity soft to loud; Filter (kit pads) the cutoff
// from the bottom up to the pad's own over pads 1-8 and on to the top over
// 9-16; Tune by semitones, the original on pad 4; and Melodee's Scale, up
// the project's scale (B3 again on Tune, on a kit and a synth alike; B2 is
// Filter on a kit and nothing on a synth). Pad 1 is bottom left. The picture:
// the pads with their values, and beside them whose levels these are and the
// type drawn (a wedge, the sweep, a step a pad) with its two ends. The pots
// stay on that pad; of type Scale, K1 and K2 are the project's key, elsewhere
// on this page Shift + K1 and K2. On a kit in Tune or Scale, − / + move the
// pads an octave. 16 Levels again goes back.
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/ui/waveform.h"
#include <algorithm>
#include <string>

namespace melodee {
namespace {
using namespace ui;

LevelsKind kind_of(const Track& t) { return LevelsKind(t.levels_kind % unsigned(LevelsKind::Count)); }

// A kit's octave off its instrument's own pitch, signed: "+1", "-2".
juce::String octave_text(const Track& t) {
    const int octave = level_octave(t) / 12;
    return octave > 0 ? "+" + juce::String(octave) : juce::String(octave);
}

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
    h.left.push_back(chip(Icon::Velocity, "16 LEVELS", c));
    if (level_octave(t) && (kind == LevelsKind::Tune || kind == LevelsKind::Scale)) h.left.push_back(pill("OCT " + octave_text(t), colour::text));
    h.right.push_back(tag_of(s, s.ui.track));
    // The same three places on a kit and a synth: B1 Velocity, B2 Filter (kits), B3 Tune and Scale as a cycle with its dots.
    bool scale = kind == LevelsKind::Scale;
    int active = kind == LevelsKind::Velocity ? 0 : kind == LevelsKind::Filter ? 1 : 2;
    SoftKeys keys {{"VELOCITY", kit ? "FILTER" : "", scale ? "SCALE" : "TUNE"}, active, c};
    keys.dots[2] = 2;
    keys.dot_at[2] = scale ? 1 : 0;
    frame(p, s, keys, h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f, grid_w = 150;
    constexpr float w = (grid_w - 3 * gap) / 4, ch = (zone::picture_h - 2.0f - 3 * gap) / 4;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool hit = int(n) == s.ui.levels_hit;
        bool original = kind == LevelsKind::Tune && n == 3 && !level_octave(t);
        // Lighter as they rise.
        p.fill(x, y, w, ch, colour::bg, 4);
        p.fill(x, y, w, ch, hit ? colour::text : c.withAlpha(0.2f + 0.6f * n / 15.0f), 4);
        if (original && !hit) p.ring(x, y, w, ch, colour::text, 4, 1);
        auto fg = hit ? colour::bg : colour::text;
        p.text(juce::String(n + 1), x + 4, y + 2, w - 8, 14, plain(hit ? colour::bg : n < 4 ? colour::dim : colour::bg.withAlpha(0.8f), type::small, Weight::SemiBold));
        p.text(value_of(s, n), x + 4, y + ch - 16, w - 8, 14, plain(fg, type::small, Weight::Medium));
    }
    // Right: whose levels (the pad, or the last note), then the type drawn with its two ends; the pad hit lights on the grid.
    float x = zone::pad + grid_w + 12, y = zone::picture_y + 3, iw = zone::width - zone::pad - x;
    juce::String name;
    const PadSound* pad = nullptr;
    if (kit) {
        char label[4]; pad_label(t.levels_pad, label);
        p.text(juce::String(label), x, y, iw, 14, caps_text(colour::mid));
        pad = &t.pads[t.levels_pad % sampler_pads];
        name = pad->used() ? juce::String(s.project.samples[pad->sample % sample_slots].name) : juce::String("Empty");
    } else {
        icon(p, Icon::Note, x, y + 1, 12, colour::mid, 1.4f);
        name = juce::String(note_name(t.last_note));
    }
    bool lit = kit ? pad->used() : true;
    p.text(name, x, y + 14, iw, 28, p.fit(name, plain(lit ? colour::text : colour::dim, 24, Weight::SemiBold), iw, type::floor));

    float gy = y + 52, gh = 36, base = gy + gh;
    auto ends = plain(colour::sec, type::small, Weight::SemiBold);
    p.text(value_of(s, 0), x, base + 3, iw / 2, 14, ends);
    p.text(value_of(s, 15), x + iw / 2, base + 3, iw / 2, 14, ends, juce::Justification::centredRight);
    if (kind == LevelsKind::Velocity || (kind == LevelsKind::Filter && !kit)) { // soft to loud: a wedge
        juce::Path wedge;
        wedge.addTriangle(x, base, x + iw, base, x + iw, gy);
        p.g.setGradientFill(juce::ColourGradient(c.withAlpha(0.12f), x, base, c.withAlpha(0.9f), x + iw, base, false));
        p.g.fillPath(wedge);
    } else if (kind == LevelsKind::Filter) { // the cutoff from the bottom to the pad's own (pad 8), then on to the top
        juce::Path rise;
        rise.startNewSubPath(x, base); rise.lineTo(x + iw, gy);
        p.g.setColour(c);
        p.g.strokePath(rise, juce::PathStrokeType(2, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        float own = x + iw * 7.5f / 15;
        p.circle(own, base - gh * 7.5f / 15, 4, colour::text);
    } else { // Tune and Scale: a step for each pad, the one that plays as it is ringed
        float sw = iw / 16;
        for (unsigned n = 0; n < 16; ++n) {
            float hgt = 6 + (gh - 6) * n / 15.0f;
            p.fill(x + n * sw + 0.5f, base - hgt, std::max(1.5f, sw - 2), hgt, c.withAlpha(0.25f + 0.6f * n / 15.0f), 1);
        }
        if (kind == LevelsKind::Tune && !level_octave(t)) p.ring(x + 3 * sw - 1, base - gh - 3, sw + 1, gh + 6, colour::text, 2, 1);
        if (kind == LevelsKind::Scale) {
            juce::String key = juce::String(note_letter(s.project.root)) + " " + scales()[s.project.scale % scales().size()].name;
            p.text(key, x, gy - 14, iw, 14, p.fit(key, caps_text(c), iw, type::floor));
        }
    }
    if (pad && pad->used() && s.sample.frames) {
        uint32_t end = pad->end ? pad->end : s.sample.frames;
        float from = float(pad->start) / s.sample.frames, to = float(end) / s.sample.frames;
        ui::WaveHead head; // the pad as it is heard: a head and its wake
        bool heard = pad_head(s, t.levels_pad % sampler_pads, *pad, s.sample.frames, head);
        waveform(p, x, base + 24, iw, 22, s.sample.overview, from, to, from, to, c, &head, heard ? 1 : 0, 1);
        if (heard) draw_heads(p, x, base + 22, iw, 26, from, to, &head, 1, false);
    }
}

// B1 Velocity; B2 Filter (a kit; nothing on a synth); B3 Tune, and again Scale, on both.
void soft(Context& c, int key) {
    auto& t = c.track();
    bool kit = t.engine == EngineKind::Sampler;
    auto kind = LevelsKind(t.levels_kind % unsigned(LevelsKind::Count));
    if (key == 1 && !kit) return;
    LevelsKind next = key == 0 ? LevelsKind::Velocity
                    : key == 1 ? LevelsKind::Filter
                    : (kind == LevelsKind::Tune ? LevelsKind::Scale : LevelsKind::Tune);
    t.levels_kind = uint8_t(next);
    c.ui.levels_hit = -1;
}

// − / +: on a kit in Tune or Scale (an instrument's pad above all) the pads an
// octave down or up, as on the other play pages.
void nudge(Context& c, int way) {
    auto& t = c.track();
    const auto kind = kind_of(t);
    if (t.engine != EngineKind::Sampler || (kind != LevelsKind::Tune && kind != LevelsKind::Scale)) return;
    t.octave = int8_t(std::clamp(t.octave + way, kit_lowest_octave, kit_highest_octave));
    const int octave = t.octave - kit_level_octave;
    c.notice(octave ? std::string("Octave ") + (octave > 0 ? "+" : "") + std::to_string(octave) : std::string("Octave 0, as it is"));
}
} // namespace

const PageSpec levels_page {draw, play_pad, no_wheel, no_push, soft, nudge};

} // namespace melodee
