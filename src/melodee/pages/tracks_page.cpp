// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// TRACKS (Pad Bank tapped twice, shell/pad_bank.cpp): the sixteen tracks as
// the pads sit, eight at a time (pages/track_grid.cpp): engine, sound, the
// pattern playing and level. B1 tracks 1-8, B2 tracks 9-16, B3 back to
// Stage. A pad picks its track and goes back to Stage; K1 the track, K2 its
// level, K3 its pattern; the wheel the track, Push back to Stage.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    unsigned half = s.ui.tracks_half % 2;
    Header h = header_of(s);
    h.left.push_back(pill(juce::String(s.project.used_tracks()) + " USED"));
    h.right.push_back(label(s.project.name));
    frame(p, s, {{"TRACKS 1-8", "TRACKS 9-16", "STAGE"}, int(half), colour_of(s)}, h);
    track_grid(p, s, half);
}

void pick(Context& c, unsigned track) {
    c.select_track(track % track_count);
    c.ui.tracks_half = uint8_t(c.ui.track / 8);
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (!down) return;
    pick(c, pad);
    c.open(PageId::Stage);
}

void wheel(Context& c, int delta) { pick(c, unsigned(int(c.ui.track) + delta + int(track_count))); }
void push(Context& c) { c.open(PageId::Stage); }

void soft(Context& c, int key) {
    if (key == 2) { c.open(PageId::Stage); return; }
    c.ui.tracks_half = uint8_t(key);
}
} // namespace

const PageSpec tracks_page {draw, pad, wheel, push, soft, wheel}; // Minus / Plus: the track, as the wheel

} // namespace melodee
