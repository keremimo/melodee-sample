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
    frame(p, s, {{"", "", "QUANTIZE"}, f.quantize ? 2 : -1, c}, h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f;
    constexpr float w = (zone::width - 2 * zone::pad - 3 * gap) / 4, ch = (145 - 1.5f - 3 * gap) / 4;
    unsigned playing = s.flex_playing;
    for (unsigned n = 0; n < flex_pads; ++n) {
        unsigned row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool on = n == playing, chosen = n == s.ui.flex_chosen && !on && n;
        p.fill(x, y, w, ch, on ? c : n == 0 ? colour::quiet : colour::surf, 4);
        if (chosen) p.ring(x, y, w, ch, c, 4, 1); // waiting for its sixteenth
        p.text(juce::String(n + 1), x + 6, y + 4, 20, 8, plain(on ? colour::bg : colour::dim, 6.5f, Weight::SemiBold));
        if (on && n) p.text(f.mode ? "LOOP" : "ON", x + w - 36, y + 4, 30, 8, caps_text(colour::bg, 6), juce::Justification::right);
        juce::String name = juce::String(flex_beat(n).name).toUpperCase();
        p.text(name, x + 6, y + ch - 16, w - 12, 13, p.fit(name, plain(on ? colour::bg : n ? c : colour::mid, 11, Weight::SemiBold), w - 12, 6.5f));
    }
}

void pad(Context& c, unsigned pad, uint8_t, bool down) { if (down) c.flex_pad(pad); }
void soft(Context& c, int key) {
    if (key != 2) return;
    c.set_flex(3, !c.project.flex.quantize);
    c.notice(c.project.flex.quantize ? "Quantize on" : "Quantize off");
}
} // namespace

const PageSpec flex_beat_page {draw, pad, no_wheel, no_push, soft, [](Context&, int) {}};

} // namespace melodee
