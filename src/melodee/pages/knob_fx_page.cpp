// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// KNOB FX (Knob FX, parity phase 7), as on the MPC: one effect on K1-K3
// (Shift + K1-K3 its second row), on every page while the mode is on. Here
// the wheel or Minus / Plus walk the list (the MPC's effects first, then
// Melodee's), Push loads the one under the cursor. The pads switch it on per
// kit pad of the bank in view (a kit track) or per track: lit bright when
// on. B2 ALL, B3 BYPASS. Knob FX again turns the mode off.
#include "melodee/engine/effect_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;
constexpr unsigned rows = 6;

void draw(Paint& p, const Snapshot& s) {
    const auto& k = s.project.knob_fx;
    bool kit = s.track().engine == EngineKind::Sampler;
    auto c = kit ? colour_of(s) : colour::text;
    Header h = header_of(s);
    h.left.push_back(pill("KNOB FX"));
    h.left.push_back(pill(k.bypass ? "BYPASS" : k.on ? "ON" : "OFF", k.bypass ? colour::mid : c));
    if (kit) h.right.push_back(tag_of(s, s.ui.track));
    else h.right.push_back(label("TRACKS", colour::mid));
    bool all = true;
    for (unsigned n = 0; n < 16; ++n) all &= kit ? k.pad_on(s.ui.track, s.track().bank % sampler_banks * 16 + n) : k.track_on(n);
    frame(p, s, {{"", all ? "ALL ON" : "ALL", "BYPASS"}, k.bypass ? 2 : all ? 1 : -1, c}, h);

    // The list: the cursor's entry in the middle where it can be, the loaded one marked.
    float x = zone::pad, y = zone::picture_y + 3, w = 168;
    unsigned count = knob_fx_count(), cursor = std::min<unsigned>(s.ui.knob_cursor, count - 1);
    unsigned first = cursor > rows / 2 ? std::min(cursor - rows / 2, count - rows) : 0;
    for (unsigned n = 0; n < rows && first + n < count; ++n) {
        // A list row (as ui::list_row draws one), its name shrunk to fit beside the tag rather than cut.
        unsigned e = first + n;
        bool current = e == cursor, melodee = e >= knob_fx_mpc_count();
        float ry = y + n * (list_row_h + list_row_gap);
        if (current) { p.fill(x, ry, w, list_row_h, colour::raised, 4); p.fill(x, ry, 3, list_row_h, c, 1); }
        p.text(juce::String(e + 1).paddedLeft('0', 2), x + 9, ry, 13, list_row_h, plain(colour::dim, 8, Weight::Medium));
        juce::String tag = e == k.choice ? "LOADED" : melodee ? "MELODEE" : "";
        auto tag_style = caps_text(e == k.choice ? c : colour::dim, 6.5f);
        float tw = tag.isEmpty() ? 0 : p.width(tag, tag_style) + 6;
        if (tw > 0) p.text(tag, x + w - 6 - tw, ry, tw, list_row_h, tag_style, juce::Justification::centredRight);
        juce::String name = knob_fx_entry(e).name;
        auto style = plain(current ? colour::text : melodee ? colour::dim : colour::sec, type::list, Weight::Medium);
        float nw = w - 28 - tw - 6;
        p.text(name, x + 28, ry, nw, list_row_h, p.fit(name, style, nw, 8));
    }
    p.text(juce::String(cursor + 1) + " / " + juce::String(count) + "  PUSH LOADS", x, y + rows * (list_row_h + list_row_gap) + 2, w, 10,
           caps_text(colour::dim, 6.5f), juce::Justification::centredRight);

    // The pads as they sit (pad 1 bottom left): kit pads of the bank, or tracks.
    float gx = x + w + 10, gw = zone::width - zone::pad - gx, gh = zone::picture_h - 8;
    float cw = (gw - 6) / 4, ch = (gh - 6) / 4;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned row = 3 - pad / 4, column = pad % 4;
        bool on;
        juce::String caption;
        juce::Colour fill = c;
        if (kit) {
            unsigned n = s.track().bank % sampler_banks * 16 + pad;
            on = k.pad_on(s.ui.track, n);
            char text[4]; pad_label(n, text);
            caption = text;
        } else {
            on = k.track_on(pad);
            caption = juce::String(pad + 1);
            fill = track_colour(pad);
            if (!s.project.tracks[pad].used()) fill = colour::mid;
        }
        if (k.bypass) fill = fill.withAlpha(0.45f);
        float cx = gx + column * (cw + 2), cy = zone::picture_y + 2 + row * (ch + 2);
        p.fill(cx, cy, cw, ch, on ? fill : colour::surf, 3);
        p.text(caption, cx, cy, cw, ch, plain(on ? colour::bg : colour::dim, 7, Weight::SemiBold), juce::Justification::centred);
    }
}

void pad(Context& c, unsigned pad, uint8_t, bool down) { if (down) c.knob_fx_pad(pad); }
void wheel(Context& c, int delta) { c.ui.knob_cursor = uint8_t(std::clamp<int>(c.ui.knob_cursor + delta, 0, int(knob_fx_count()) - 1)); }
void push(Context& c) {
    c.knob_fx_load(c.ui.knob_cursor);
    c.notice(std::string("Knob FX: ") + knob_fx_entry(c.ui.knob_cursor).name);
}
void soft(Context& c, int key) {
    if (key == 1) c.knob_fx_all();
    if (key == 2) c.knob_fx_bypass();
}
} // namespace

const PageSpec knob_fx_page {draw, pad, wheel, push, soft, wheel};

} // namespace melodee
