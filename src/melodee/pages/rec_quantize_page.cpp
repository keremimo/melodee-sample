// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// REC QUANTIZE (Shift + Pad 10): where Seq Record puts the notes played.
// On, each lands on the nearest step of the grid: the pattern's Q, or one of
// its own, 1/4 .. 1/64 with triplets (K1, the wheel or Minus / Plus); off,
// it keeps the time it was played. B3 turns it off and on and keeps the
// grid; B1 goes back. The pads play, so a take can start from here. The
// picture: the grid's name, and one beat with a line at each place a note
// can land. Kept with Melodee's settings (shell/settings.h).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    bool on = s.ui.settings.rec_quantize;
    unsigned grid = s.ui.settings.rec_grid;
    Division division = grid_division(grid, s.pattern.division);
    Header h = header_of(s);
    h.left.push_back(pill("REC QUANTIZE"));
    h.left.push_back(pill(on ? "ON" : "OFF", on ? c : colour::mid));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"BACK", "", on ? "REC Q OFF" : "REC Q ON"}, on ? 2 : -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);

    float x = zone::pad + 4, y = zone::picture_y + 6, w = zone::width - 2 * zone::pad - 8;
    auto ink = on ? colour::text : colour::mid;
    float nw = p.text(division_name(division), x, y, 140, 40, plain(ink, 34, Weight::SemiBold));
    juce::String whose = grid ? juce::String("its own grid") : "the pattern's Q";
    p.text(whose, x + nw + 10, y + 16, w - nw - 10, 16, plain(on ? c : colour::mid, type::body, Weight::Medium));

    // One beat, a line wherever a note can land (its start bright); triplets three to a step of the beat.
    unsigned beat = s.project.beat_ticks(), step = std::max(1u, division_ticks(division));
    float by = y + 54, bh = 30;
    p.text("ONE BEAT", x, by - 15, 120, 12, caps_text(colour::mid));
    p.fill(x, by, w, bh, colour::panel, 4);
    for (unsigned t = 0; t <= beat; t += step) {
        float lx = x + w * float(std::min(t, beat)) / float(beat);
        bool edge = t == 0 || t >= beat;
        p.fill(std::clamp(lx - 1, x, x + w - 2), by + (edge ? 0 : 5), 2, edge ? bh : bh - 10, edge ? colour::sec : on ? c : colour::dim, 1);
    }
    unsigned places = std::max(1u, beat / step);
    juce::String detail = on ? juce::String(places) + (places == 1 ? " place a beat for a note to land" : " places a beat for notes to land")
                             : juce::String("Off: notes keep the time they were played");
    p.text(detail, x, by + bh + 6, w, 16, plain(colour::sec, type::small, Weight::Medium));
}

// The wheel, Minus / Plus: the pattern's Q, then 1/4 .. 1/64 (K1 the same).
void move(Context& c, int delta) {
    auto& settings = c.ui.settings;
    settings.rec_grid = uint8_t(std::clamp(int(settings.rec_grid) + delta, 0, int(Division::Count)));
    c.settings_save_at = std::max<uint32_t>(c.now, 1) + 1000; // the card gets the last of a turn
}

void soft(Context& c, int key) {
    if (key == 0) { c.back(); return; }
    if (key != 2) return;
    auto& settings = c.ui.settings;
    settings.rec_quantize = !settings.rec_quantize;
    c.notice(settings.rec_quantize ? "Rec Quantize on" : "Rec Quantize off");
    c.settings_save_at = std::max<uint32_t>(c.now, 1);
}
} // namespace

const PageSpec rec_quantize_page {draw, play_pad, move, no_push, soft, move};

} // namespace melodee
