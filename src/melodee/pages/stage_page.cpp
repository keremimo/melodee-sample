// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// STAGE (Sample): sixteen tracks drawn as the pads sit, pad 1 bottom left
// (pages/track_grid.cpp). Each cell shows engine, sound, the pattern playing and level. The wheel
// picks the track, Push opens its sound (or, on an empty cell, starts one).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    Header h = header_of(s);
    h.left.push_back(pill(juce::String(s.project.used_tracks()) + " OF 16 TRACKS"));
    h.right.push_back(label(s.project.name));
    frame(p, s, play_keys(s, 0), h);

    track_grid(p, s);
}

void wheel(Context& c, int delta) { c.select_track(unsigned(int(c.ui.track) + delta + 16) % 16); }

void push(Context& c) {
    if (!c.track().used()) {
        // An empty cell: the first engine that is built, with its first sound.
        c.load_sound(c.ui.track, EngineKind::Prophet, 0);
        c.open(PageId::Sounds);
        return;
    }
    c.open(c.track().engine == EngineKind::Sampler ? PageId::Trim : PageId::Edit);
}
} // namespace

const PageSpec stage_page {draw, play_pad, wheel, push, play_soft, octave};

} // namespace melodee
