// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// FADER (Shift + Pad 9, parity phase 8), as on the MPC: what the fader moves
// on the selected track. The wheel (or Minus / Plus) runs down the targets,
// Push picks one: Pad Volume, Pad Pan, Pad Tune, Pad Attack, Pad Decay /
// Release (Decay on a one shot, Release on Note On), Pad Cutoff (each the
// last pad hit's), Kit Volume, and Melodee's Mod Wheel. Pad and kit targets
// are for kit tracks; a new track takes Pad Volume on a kit, the mod wheel on
// a synth. B3 turns the fader off and on for the track (pages with a job of
// their own, Mix, Sends, Step Edit, Knob FX, the Compressor and Flex Beat,
// keep it); B1 goes back. The pads play, so a hit picks the pad. Right: the
// fader as it drives the value now, hollow where it still has to reach it.
#include "melodee/model/fader.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/fader.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    bool kit = t.engine == EngineKind::Sampler, on = t.fader_on && t.used();
    unsigned chosen = unsigned(fader_target(t));
    Header h = header_of(s);
    h.left.push_back(pill(Icon::Fader, "FADER"));
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{"BACK", "", "FADER"}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    keys.toggles = 4;
    keys.lit = on ? 4 : 0;
    frame(p, s, keys, h);

    // The targets the track can take, one row each (a synth has the mod wheel alone).
    constexpr float rh = 18;
    float x = zone::pad, w = 184, y = zone::picture_y + 2;
    for (unsigned n = 0; n < fader_targets; ++n) {
        auto target = FaderTarget(n);
        if (!kit && fader_needs_kit(target)) continue;
        bool cursor = n == s.ui.fader_cursor || (!kit && fader_needs_kit(FaderTarget(s.ui.fader_cursor % fader_targets))), here = n == chosen && on;
        if (cursor) { p.fill(x, y, w, rh - 1, c.withAlpha(0.16f), 3); p.fill(x, y, 2.5f, rh - 1, c); }
        p.text(fader_target_name(target), x + 9, y, w - 30, rh - 1, plain(cursor ? colour::text : colour::sec, type::body, cursor ? Weight::SemiBold : Weight::Regular));
        if (here) p.circle(x + w - 10, y + rh / 2 - 0.5f, 3.5f, c); // the one the fader moves
        y += rh;
    }

    // The fader drawn: its slot, the value as a cap in the track colour, and while it has to reach the value (takeover),
    // where the hand has it, dashed, with an arrow the way to move.
    auto shown = fader_shown(s);
    float sx = zone::width - zone::pad - 40, top = zone::picture_y + 8, slot_h = 128, mid = top + slot_h / 2;
    p.fill(sx + 9, top, 6, slot_h, colour::line, 3);
    if (shown.card.bipolar) p.fill(sx + 2, mid - 0.5f, 20, 1, colour::mid);
    if (shown.value) {
        float level = std::clamp(shown.card.fill, 0.0f, 1.0f), at = top + slot_h * (1 - level);
        p.fill(sx, at - 4.5f, 24, 9, c, 2);
        if (shown.waiting) {
            float hand = top + slot_h * (1 - std::clamp(shown.card.pot, 0.0f, 1.0f));
            p.g.setColour(colour::mid);
            p.g.drawRoundedRectangle(sx + 0.5f, hand - 4, 23, 8, 2, 1.2f);
            float ay = (hand + at) / 2;
            if (hand < at) { // the value is below the hand: the arrow points down
                juce::Path down;
                float ax = sx + 35;
                down.startNewSubPath(ax, ay - 7); down.lineTo(ax, ay + 7);
                down.startNewSubPath(ax - 6, ay + 1); down.lineTo(ax, ay + 7); down.lineTo(ax + 6, ay + 1);
                p.g.setColour(colour::amber);
                p.g.strokePath(down, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            } else {
                icon(p, Icon::ArrowUp, sx + 28, ay - 7, 14, colour::amber, 1.8f);
            }
        }
    }
    // Its value and what it moves, beside it.
    float vx = x + w + 6, vw = sx - 6 - vx;
    if (shown.value) {
        juce::String value = shown.card.value + (shown.card.unit.isNotEmpty() ? " " + shown.card.unit : juce::String());
        auto big = plain(shown.waiting ? colour::mid : colour::text, type::value, Weight::SemiBold);
        p.text(value, vx, mid - 26, vw, 26, p.fit(value, big, vw, type::body), juce::Justification::centredRight);
    }
    juce::String what = shown.off ? juce::String("OFF") : shown.what.toUpperCase();
    auto label = caps_text(colour::mid);
    auto lines = p.wrap(what, label, vw, 2);
    for (int l = 0; l < lines.size(); ++l) p.text(lines[l], vx, mid + 2 + l * 13, vw, 14, label, juce::Justification::centredRight);
}

void move(Context& c, int delta) { // over the targets the track can take
    bool kit = c.track().engine == EngineKind::Sampler;
    int at = std::min(int(c.ui.fader_cursor), int(fader_targets) - 1);
    if (delta != 0)
        for (int n = at + delta; n >= 0 && n < int(fader_targets); n += delta)
            if (kit || !fader_needs_kit(FaderTarget(n))) { at = n; break; }
    if (!kit && fader_needs_kit(FaderTarget(at))) // a synth: onto the one it can take
        for (unsigned n = 0; n < fader_targets; ++n) if (!fader_needs_kit(FaderTarget(n))) { at = int(n); break; }
    c.ui.fader_cursor = uint8_t(at);
}
void push(Context& c) {
    move(c, 0); // a synth's cursor onto the one target it can take
    set_fader_target(c, c.ui.fader_cursor);
}
void soft(Context& c, int key) {
    if (key == 0) c.back();
    else if (key == 2) toggle_fader(c);
}
} // namespace

const PageSpec fader_page {draw, play_pad, move, push, soft, move};

} // namespace melodee
