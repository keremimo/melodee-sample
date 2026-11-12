// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// FLEX BEAT (Shift + Pad FX, parity phase 7b, as on the MPC): time effects
// over the whole sequence while it plays. Pad 1 is Empty (nothing changed,
// where it starts); pads 2-16 each warp time, pitch or level for a bar: tape
// stop, backspin, half speed, reverse, scratch, stutter, repeats, gates,
// shuffle, pitch down and up, pump. K1 One Shot (back to Empty after its bar)
// or Loop (again until another pad or Empty), K3 mix, B3 quantize (start on
// the next sixteenth). Pad FX goes back to Pad FX.
#include "melodee/engine/pad_fx_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour::text;
    const auto& f = s.project.flex;
    Header h = header_of(s);
    h.left.push_back(pill("FLEX BEAT"));
    h.left.push_back(pill(f.mode ? "LOOP" : "ONE SHOT", colour::mid));
    if (!s.playing) h.left.push_back(pill("PRESS PLAY", colour::mid));
    h.right.push_back(label("MASTER", colour::mid));
    SoftKeys keys {{"", "", "QUANTIZE"}, -1, c};
    keys.toggles = 4; // on or off: its LED
    keys.lit = f.quantize ? 4 : 0;
    frame(p, s, keys, h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f;
    constexpr float w = (zone::width - 2 * zone::pad - 3 * gap) / 4, ch = (zone::picture_h - 1.5f - 3 * gap) / 4;
    unsigned playing = s.flex_playing;
    juce::StringArray names; // one case for all sixteen
    for (unsigned n = 0; n < flex_pads; ++n) names.add(juce::String(flex_beat(n).name).toUpperCase());
    bool capitals = cells_in_capitals(p, names, plain(c, type::small, Weight::SemiBold), w - 8);
    for (unsigned n = 0; n < flex_pads; ++n) {
        unsigned row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool on = n == playing, chosen = n == s.ui.flex_chosen && !on && n;
        p.fill(x, y, w, ch, on ? c : n == 0 ? colour::quiet : colour::surf, 4);
        if (chosen) p.ring(x, y, w, ch, c, 4, 1); // waiting for its sixteenth
        float nw = p.text(juce::String(n + 1), x + 5, y + 2, 20, 14, plain(on ? colour::bg : colour::dim, type::small, Weight::SemiBold));
        if (on && n) p.text(f.mode ? "LOOP" : "ON", x + 5 + nw + 4, y + 2, w - 14 - nw, 14, caps_text(colour::bg), juce::Justification::right);
        auto style = plain(on ? colour::bg : n ? c : colour::mid, type::small, Weight::SemiBold);
        juce::String name = cell_name(names[int(n)], capitals);
        p.text(name, x + 4, y + ch - 17, w - 8, 14, style);
    }
}

void pad(Context& c, unsigned pad, uint8_t, bool down) { if (down) c.flex_pad(pad); }
void soft(Context& c, int key) {
    if (key != 2) return;
    c.set_flex(3, !c.project.flex.quantize); // its LED says which
}
} // namespace

const PageSpec flex_beat_page {draw, pad, no_wheel, no_push, soft, [](Context&, int) {}};

} // namespace melodee
