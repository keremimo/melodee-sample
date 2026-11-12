// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// KNOB FX (Knob FX, parity phase 7; round 2, knob fx C "two settings and
// the fader between"): one effect on K1-K3 (Shift + K1-K3 its second row),
// on every page while the mode is on. K1-K3 keep two settings, A (B1) and
// B (B2): the pots change the one in hand, heard at it, and the fader here
// slides the effect from A to B, a filter opening while a delay closes in
// one move. The wheel or Minus / Plus walk the list (the MPC's effects first,
// then Melodee's), Push loads the one turned to (A and B both its initial
// values). The pads switch it on per kit pad of the bank in view (a kit
// track) or per track, their lights showing which; Shift + B2 all, B3
// BYPASS. Knob FX again turns the mode off. Picture: the effect turned to,
// then for each pot its A, its B, the way between them and where it is now.
#include "melodee/engine/effect_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    const auto& k = s.project.knob_fx;
    bool kit = s.track().engine == EngineKind::Sampler;
    Header h = header_of(s);
    h.left.push_back(pill(Icon::Knob, "KNOB FX", colour::text));
    h.left.push_back(pill(Icon::Fader, "A TO B"));
    if (kit) h.right.push_back(tag_of(s, s.ui.track));
    else h.right.push_back(label("TRACKS", colour::mid));
    SoftKeys keys {{"A", "B", "BYPASS"}, int(k.editing), colour::text};
    keys.toggles = 4;
    keys.lit = k.bypass ? 4 : 0;
    frame(p, s, keys, h);

    // The effect the wheel has turned to: loaded, or amber with Push to load it; its place in the list.
    unsigned count = knob_fx_count(), cursor = std::min<unsigned>(s.ui.knob_cursor, count - 1);
    const auto& entry = knob_fx_entry(cursor);
    bool loaded = cursor == k.choice;
    auto ink = loaded ? colour::text : colour::amber;
    float y = zone::picture_y + 4;
    icon(p, family_icon(effect_info(entry.kind).family), zone::pad + 2, y + 1, 16, ink, 1.4f);
    juce::String name = entry.name;
    auto style = plain(ink, type::list, Weight::SemiBold);
    float nw = std::min(p.width(name, style), 200.0f);
    p.text(name, zone::pad + 24, y, nw, 18, p.fit(name, style, 200, type::floor));
    if (!loaded) icon(p, Icon::Push, zone::pad + 30 + nw, y + 2, 14, colour::amber, 1.5f);
    p.text(juce::String(cursor + 1) + " / " + juce::String(count), zone::width - zone::pad - 70, y, 70, 18, caps_text(colour::dim),
           juce::Justification::centredRight);

    // Each pot: its A and B as marks either side of a slot, the way between them, and where the fader has it now.
    const auto& info = effect_info(EffectKind(k.kind));
    constexpr float top = zone::picture_y + 32, slot_h = zone::knobs_y - 10 - top;
    float fade = k.bypass ? 0.45f : 1.0f;
    for (unsigned q = 0; q < 3 && k.kind; ++q) {
        const auto& pi = info.params[q];
        if (!pi.label) continue;
        float cx = zone::pad + 50 + q * 104;
        auto at = [&](int v) { return top + slot_h * (1 - std::clamp(gauge_of(pi, v), 0.0f, 1.0f)); };
        float ya = at(k.a[q]), yb = at(k.b[q]), now = at(k.params[q]);
        p.fill(cx - 2, top, 4, slot_h, colour::line, 2);
        p.fill(cx - 2, std::min(ya, yb), 4, std::max(1.0f, std::fabs(ya - yb)), colour::mid.withMultipliedAlpha(fade), 2);
        auto mark = [&](float x, float my, const char* letter, bool in_hand, float tick_from, float tick_to) {
            auto c = (in_hand ? colour::text : colour::mid).withMultipliedAlpha(fade);
            p.circle(x, my, 8, colour::surf);
            p.text(letter, x - 8, my - 8, 16, 16, plain(c, type::small, Weight::Bold), juce::Justification::centred);
            p.fill(tick_from, my - 0.6f, tick_to - tick_from, 1.2f, c);
        };
        mark(cx - 16, ya, "A", k.editing == 0, cx - 8, cx - 2);
        mark(cx + 16, yb, "B", k.editing == 1, cx + 2, cx + 8);
        p.fill(cx - 9, now - 2.5f, 18, 5, colour::text.withMultipliedAlpha(fade), 2.5f);
    }
}

void pad(Context& c, unsigned pad, uint8_t, bool down) { if (down) c.knob_fx_pad(pad); }
void wheel(Context& c, int delta) { c.ui.knob_cursor = uint8_t(std::clamp<int>(c.ui.knob_cursor + delta, 0, int(knob_fx_count()) - 1)); }
void push(Context& c) {
    c.knob_fx_load(c.ui.knob_cursor);
    c.notice(std::string("Knob FX: ") + knob_fx_entry(c.ui.knob_cursor).name);
}
void soft(Context& c, int key) {
    if (key == 1 && c.ui.shift) { c.ui.shift_used = true; c.knob_fx_all(); return; } // Shift + B2: every pad or track
    if (key < 2) c.knob_fx_edit(unsigned(key));
    else c.knob_fx_bypass();
}
} // namespace

const PageSpec knob_fx_page {draw, pad, wheel, push, soft, wheel};

} // namespace melodee
