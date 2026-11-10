// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// TIME CORRECT (Shift + Pad 14, as printed): the selected track's pattern
// moved onto its Q. The pads choose whose notes move (kit pads of the bank
// in view, the 909's drums, or the notes a synth's pads play); B2 chooses
// them all. K1 Q (the pattern's, as on Patterns), K2 shift (ticks, 96 a
// quarter: every chosen note moves by it, early or late), K3 swing (every
// second step of a straight Q late by it). B3 DO IT moves them, as one undo
// (Shift + Minus); B1 CANCEL goes back without moving anything.
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/step_edit.h" // pad_key

namespace melodee {
namespace {
using namespace ui;

juce::String pad_caption(const Snapshot& s, unsigned, int key) {
    const auto& t = s.track();
    if (key < 0) return {};
    if (t.engine == EngineKind::Sampler) { char label[4]; pad_label(unsigned(key) - sampler_first_note, label); return label; }
    return note_name(key);
}

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(pill("TIME CORRECT"));
    h.left.push_back(pill("P" + juce::String(s.track().pattern + 1)));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"CANCEL", s.ui.tc_all ? "ALL ON" : "ALL", "DO IT"}, s.ui.tc_all ? 1 : -1, c,
                 {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}, h);

    float x = zone::pad, y = zone::picture_y + 2, w = 150;
    p.text("Notes onto Q", x, y, w, 20, plain(colour::text, type::title, Weight::SemiBold));
    juce::String detail = s.ui.tc_all ? "Every note of the pattern." : "The notes of the pads lit.";
    p.text(detail, x, y + 22, w + 20, 12, plain(colour::sec, 9));
    // What moves where: the steps of Q over one beat, the swung ones late.
    const auto& pattern = s.pattern;
    unsigned q = pattern.q(), beat = s.project.beat_ticks();
    float bx = x, by = y + 46, bw = w;
    p.fill(bx, by, bw, 18, colour::panel, 3);
    double late = division_triplet(pattern.division) ? 0 : q * (s.ui.tc_swing - 50) / 50.0;
    for (unsigned k = 0; k * q < 2 * beat; ++k) {
        double at = k * double(q) + (k % 2 ? late : 0) + s.ui.tc_shift;
        float px = bx + float(at / (2.0 * beat)) * bw;
        if (px < bx || px > bx + bw - 2) continue;
        p.fill(px, by + 3, 2, 12, k % 2 ? c.withAlpha(0.6f) : c, 1);
    }
    p.text(juce::String(division_name(pattern.division)) + " GRID, 2 BEATS", bx, by + 21, bw, 10, caps_text(colour::dim, 7));

    // The pads as they sit, the chosen ones lit.
    float gx = x + w + 18, gw = zone::width - zone::pad - gx, gh = zone::picture_h - 8;
    float cw = (gw - 6) / 4, ch = (gh - 6) / 4;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned row = 3 - pad / 4, column = pad % 4;
        int key = pad_key(s.project, s.track(), pad);
        bool on = key >= 0 && (s.ui.tc_all || s.ui.tc_key(unsigned(key)));
        float cx = gx + column * (cw + 2), cy = zone::picture_y + 2 + row * (ch + 2);
        p.fill(cx, cy, cw, ch, on ? c : key >= 0 ? colour::surf : colour::quiet, 3);
        p.text(pad_caption(s, pad, key), cx, cy, cw, ch, plain(on ? colour::bg : colour::dim, 7, Weight::SemiBold), juce::Justification::centred);
    }
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (!down) return;
    int key = pad_key(c.project, c.track(), pad);
    if (key < 0) return;
    if (c.ui.tc_all) { c.ui.tc_all = false; c.ui.tc_keys = {}; } // from all to this one
    c.ui.tc_toggle(unsigned(key));
}

void soft(Context& c, int key) {
    if (key == 0) { c.back(); return; }
    if (key == 1) { c.ui.tc_all = !c.ui.tc_all; return; }
    c.time_correct();
}
} // namespace

const PageSpec time_correct_page {draw, pad, no_wheel, no_push, soft, [](Context&, int) {}};

} // namespace melodee
