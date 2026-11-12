// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// FADER (Shift + Pad 9; round 2, fader C "learn from a pot"): what the
// fader moves on the selected track. No list: turn any pot (here the
// track's favourites, or on any page) and the fader takes that value, any
// of the track's, not just eight; without PIN it follows the pot turned
// last. The wheel (or Minus / Plus) walks the stock targets instead: the
// MPC's eight (Pad Volume, Pan, Tune, Attack, Decay / Release, Cutoff, Kit
// Volume, Mod Wheel; a synth has the mod wheel alone), then the track's
// Level, Pan and Sends A-D on every track, and a pot turned after takes over
// again. B1 back, B2 PIN, B3 the fader off and on for the track (pages with
// a job of their own keep it). The pads play, so a hit picks the pad.
// Picture: the pot it follows in that pot's shade (a stock target: its
// glyph), its name and value, a dashed arrow (the pin on it while PIN holds)
// to the fader, hollow where the hand still has to reach the value.
#include "melodee/model/fader.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/fader.h"
#include "melodee/shell/fader_job.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;

Icon target_icon(FaderTarget t) { // the stock targets' glyphs
    static const Icon icons[] {Icon::Speaker, Icon::Swap, Icon::Note, Icon::Dynamics, Icon::Dirt, Icon::Tone, Icon::Pads, Icon::Wheel,
                               Icon::Sliders, Icon::Swap, Icon::Route, Icon::Route, Icon::Route, Icon::Route};
    static_assert(sizeof(icons) / sizeof(icons[0]) == fader_targets, "a glyph for each target");
    return unsigned(t) < fader_targets ? icons[unsigned(t)] : Icon::Fader;
}

void draw(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    bool on = t.fader_on && t.used();
    Header h = header_of(s);
    h.left.push_back(pill(Icon::Fader, "FADER"));
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{"BACK", "PIN", "FADER"}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    keys.toggles = 2 | 4;
    keys.lit = uint8_t((t.fader_pin ? 2 : 0) | (on ? 4 : 0));
    frame(p, s, keys, h);

    auto job = fader_job(s.project, s.ui);
    auto shown = fader_shown(s);
    bool learned = job.source == FaderJob::Source::Target && job.learned;
    auto shade = learned ? knob_shade(s.ui.track, t.fader_learned.pot) : c;
    auto ink = on && shown.value ? shade : colour::dim;

    // What it follows: the pot's knob in its shade, or the stock target's glyph; its name and value under it.
    constexpr float gx = 72, gy = 74, gs = 44, cx = gx + gs / 2;
    icon(p, learned ? Icon::Knob : target_icon(fader_target(t)), gx, gy, gs, ink, 1.5f);
    juce::String name = shown.off ? juce::String("Off") : name_case(shown.what);
    auto title = plain(on ? colour::text : colour::mid, type::list, Weight::SemiBold);
    p.text(name, cx - 70, gy + gs + 6, 140, 18, p.fit(name, title, 140, type::floor), juce::Justification::centred);
    if (shown.value) {
        juce::String value = shown.card.value + (shown.card.unit.isNotEmpty() ? " " + shown.card.unit : juce::String());
        p.text(value, cx - 70, gy + gs + 24, 140, 15, plain(shown.waiting ? colour::mid : colour::sec, type::body), juce::Justification::centred);
    }
    if (!learned && on) { // the targets the wheel walks on this track, the one it has lit
        bool kit = t.engine == EngineKind::Sampler;
        unsigned first = kit ? 0 : unsigned(FaderTarget::ModWheel), count = fader_targets - first;
        constexpr float dot = 4, gap = 5;
        float x0 = cx - (count * dot + (count - 1) * gap) / 2;
        for (unsigned n = 0; n < count; ++n)
            p.fill(x0 + n * (dot + gap), gy + gs + 45, dot, dot, first + n == unsigned(fader_target(t)) ? colour::text : colour::line, 2);
    }

    // The dashed arrow to the fader; PIN sits on it while it holds.
    constexpr float ax0 = 132, ax1 = 222, ay = gy + gs / 2;
    if (on) {
        juce::Path line;
        if (t.fader_pin) { line.startNewSubPath(ax0, ay); line.lineTo(166, ay); line.startNewSubPath(188, ay); line.lineTo(ax1, ay); }
        else { line.startNewSubPath(ax0, ay); line.lineTo(ax1, ay); }
        juce::Path dashed;
        const float dashes[] {2, 4};
        juce::PathStrokeType(1.5f).createDashedStroke(dashed, line, dashes, 2);
        p.g.setColour(colour::dim);
        p.g.fillPath(dashed);
        juce::Path head;
        head.startNewSubPath(ax1 - 6, ay - 6); head.lineTo(ax1, ay); head.lineTo(ax1 - 6, ay + 6);
        p.g.strokePath(head, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (t.fader_pin) icon(p, Icon::Pin, 170, ay - 7, 14, colour::text, 1.5f);
    }

    // The fader: its slot, the value filled up to a cap in the shade; while it waits to pick the value up
    // (takeover), where the hand has it, hollow, and an arrow the way to move.
    constexpr float sx = 241, top = 52, slot_h = 124;
    p.fill(sx + 9, top, 6, slot_h, colour::line, 3);
    if (shown.value && on) {
        float level = std::clamp(shown.card.fill, 0.0f, 1.0f), at = top + slot_h * (1 - level);
        if (shown.card.bipolar) p.fill(sx + 2, top + slot_h / 2 - 0.5f, 20, 1, colour::mid);
        p.fill(sx + 9, at, 6, top + slot_h - at, shade.withAlpha(0.35f), 3);
        p.fill(sx, at - 4.5f, 24, 9, shade, 2);
        if (shown.waiting) {
            float hand = top + slot_h * (1 - std::clamp(shown.card.pot, 0.0f, 1.0f));
            p.g.setColour(colour::mid);
            p.g.drawRoundedRectangle(sx + 0.5f, hand - 4, 23, 8, 2, 1.2f);
            float mid = (hand + at) / 2;
            icon(p, hand < at ? Icon::ArrowDown : Icon::ArrowUp, sx + 28, mid - 7, 14, colour::amber, 1.8f);
        }
    }
}

void step(Context& c, int delta) { step_fader_target(c, delta); }
void soft(Context& c, int key) {
    if (key == 0) c.back();
    else if (key == 1) toggle_fader_pin(c);
    else toggle_fader(c);
}
} // namespace

const PageSpec fader_page {draw, play_pad, step, no_push, soft, step};

} // namespace melodee
