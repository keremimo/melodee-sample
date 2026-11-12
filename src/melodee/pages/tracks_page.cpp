// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// TRACKS (Sample's second stop: Stage, Tracks, then Keys or Pads): the
// sixteen tracks as the pads sit, each with its pattern, engine and level,
// and the track in hand as a strip on the right (pages/track_grid.cpp): its
// fader, meter, pan and send. A pad picks its track; K1 the track, K2 its
// level, K3 its pan; the wheel or Minus / Plus the track, Push its Stage.
// B2 LOOPER, B3 SENDS (every track's send to one bus).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    Header h = header_of(s);
    h.left.push_back(pill(juce::String(s.project.used_tracks()) + " USED"));
    if (s.track().speed != speed_normal) // the strip in hand plays its patterns at a speed (Shift + K1, roadmap P1)
        h.left.push_back(pill(juce::String::fromUTF8(speed_name(s.track().speed)), colour_of(s)));
    h.right.push_back(label(s.project.name));
    frame(p, s, {{"TRACKS", "LOOPER", "SENDS"}, 0, colour_of(s)}, h);
    track_grid(p, s);
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (down) c.select_track(pad % track_count);
}

void wheel(Context& c, int delta) { c.select_track(unsigned(int(c.ui.track) + delta + int(track_count)) % track_count); }
void push(Context& c) { c.open(PageId::Stage); }

void soft(Context& c, int key) {
    if (key == 1) c.open(PageId::Looper);
    if (key == 2) c.open(PageId::Sends);
}
} // namespace

const PageSpec tracks_page {draw, pad, wheel, push, soft, wheel}; // Minus / Plus: the track, as the wheel

} // namespace melodee
