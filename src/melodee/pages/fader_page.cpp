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
    h.left.push_back(pill("FADER"));
    h.left.push_back(pill(on ? "ON" : "OFF", on ? c : colour::mid));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"BACK", "", on ? "FADER OFF" : "FADER ON"}, on ? 2 : -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);

    // The targets, one row each.
    constexpr float rh = 18;
    float x = zone::pad, w = 176, y = zone::picture_y + 1.5f;
    for (unsigned n = 0; n < fader_targets; ++n, y += rh) {
        auto target = FaderTarget(n);
        bool cursor = n == s.ui.fader_cursor, here = n == chosen && on, usable = kit || !fader_needs_kit(target);
        if (cursor) { p.fill(x, y, w, rh - 1, colour::raised, 4); p.fill(x, y, 3, rh - 1, c); }
        auto ink = !usable ? colour::dim : cursor ? colour::text : colour::sec;
        p.text(fader_target_name(target), x + 9, y, w - 50, rh - 1, plain(ink, type::small, Weight::Medium));
        if (here) p.fill(x + w - 16, y + rh / 2 - 4, 7, 7, c, 3.5f);  // the one the fader moves
        else if (!usable) p.text("KIT", x + w - 40, y, 32, rh - 1, caps_text(colour::dim), juce::Justification::right);
    }

    // The fader as it stands: a slot, the value's level, what it is.
    auto shown = fader_shown(s);
    float fx = x + w + 8, fw = zone::width - zone::pad - fx, top = zone::picture_y + 4, slot_h = 100;
    p.fill(fx, top - 2.5f, fw, zone::picture_h - 3, colour::panel, 6);
    float sw = 10, sx = fx + fw - 12 - sw; // the slot on the right, the words beside it
    p.fill(sx, top + 4, sw, slot_h, colour::quiet, 3);
    if (shown.value) {
        float level = std::clamp(shown.card.fill, 0.0f, 1.0f);
        if (shown.card.bipolar) { // from the middle
            float mid = top + 4 + slot_h / 2, at = top + 4 + slot_h * (1 - level);
            p.fill(sx, std::min(mid, at), sw, std::max(1.0f, std::abs(at - mid)), shown.waiting ? colour::mid : c, 2);
            p.fill(sx - 3, mid, sw + 6, 1, colour::line);
        } else {
            p.fill(sx, top + 4 + slot_h * (1 - level), sw, slot_h * level, shown.waiting ? colour::mid : c, 2);
        }
        if (shown.waiting) { // where the fader sits: it takes over the value once it gets there
            float py = top + 4 + slot_h * (1 - shown.card.pot);
            p.ring(sx - 4, py - 2, sw + 8, 4, colour::text, 2, 1);
        }
    }
    float tx = fx + 8, tw = sx - 8 - tx;
    p.text("FADER", tx, top + 1, tw, 14, caps_text(colour::mid));
    juce::String what = shown.off ? juce::String("Off") : shown.value ? shown.what : juce::String("Fader");
    auto what_style = plain(colour::text, type::small, Weight::SemiBold);
    auto what_lines = p.wrap(what, what_style, tw, 2);
    float ty = top + 16;
    for (int l = 0; l < what_lines.size(); ++l, ty += 13) p.text(what_lines[l], tx, ty, tw, 14, what_style);
    ty += 3;
    if (shown.value) {
        juce::String value = shown.card.value + (shown.card.unit.isNotEmpty() ? " " + shown.card.unit : juce::String());
        p.text(value, tx, ty, tw, 18, p.fit(value, plain(shown.waiting ? colour::mid : colour::text, type::list, Weight::Medium), tw, type::floor));
        ty += 20;
        auto wait = caps_text(colour::mid);
        if (shown.waiting)
            for (auto& line : p.wrap("MOVE TO PICK UP", wait, tw, 2)) { p.text(line, tx, ty, tw, 14, wait); ty += 13; }
    } else if (!shown.off) {
        p.text(shown.what, tx, ty, tw, 14, p.fit(shown.what, plain(colour::mid, type::small), tw, type::floor));
    }
    // How the fader takes over, set in Settings.
    auto dim = caps_text(colour::dim);
    auto takeover = p.wrap("TAKEOVER " + juce::String(takeover_name(s.ui.settings.fader_takeover)).toUpperCase() + " IN SETTINGS", dim, fw - 16, 3);
    float by = top - 2.5f + zone::picture_h - 3 - 6 - takeover.size() * 13;
    for (int l = 0; l < takeover.size(); ++l) p.text(takeover[l], fx + 8, by + l * 13, fw - 16, 14, dim);
}

void move(Context& c, int delta) {
    c.ui.fader_cursor = uint8_t(std::clamp(int(c.ui.fader_cursor) + delta, 0, int(fader_targets) - 1));
}
void push(Context& c) { set_fader_target(c, c.ui.fader_cursor); }
void soft(Context& c, int key) {
    if (key == 0) c.back();
    else if (key == 2) toggle_fader(c);
}
} // namespace

const PageSpec fader_page {draw, play_pad, move, push, soft, move};

} // namespace melodee
