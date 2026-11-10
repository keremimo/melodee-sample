// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MOTION (B3 on Steps): pot moves recorded over the pattern, one lane per
// pot. Seq Record + turn a pot records (here or on any page); Erase + turn a
// pot clears that lane. The lanes are the parameters holding motion on the
// track's pattern (its engine's or its effects'), then its favourites.
#include "melodee/model/limits.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/bindings.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    const auto& motion = s.project.motion;
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(pill("EVENTS " + juce::String(motion.count) + " / " + juce::String(motion_events)));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"STEPS", "ROLL", "MOTION"}, 2, c}, h);

    auto lanes = motion_lanes(s.project, s.ui);
    unsigned length = s.pattern.length;
    for (unsigned lane = 0; lane < 3; ++lane) {
        int param = lanes[lane];
        float x = zone::pad, y = zone::picture_y + 1.5f + lane * 49, w = zone::width - 2 * zone::pad, lh = 46;
        bool current = s.ui.turning == int(lane) && s.now - s.ui.turned_at < 1500;
        p.fill(x, y, w, lh, current ? colour::raised : colour::quiet, 4);
        if (param < 0) continue;
        auto shade = knob_shade(s.ui.track, lane);
        p.g.saveState();
        p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), 3, int(lh)));
        p.fill(x, y, 8, lh, shade, 4);
        p.g.restoreState();
        auto mp = motion_param(track, param);
        if (!mp.info) continue;
        const auto& pi = *mp.info;
        auto shown = show(pi, mp.value);
        p.text(pi.label, x + 9, y + 9, 52, 10, p.fit(pi.label, caps_text(shade), 56, 5));
        p.text(shown.value + (shown.unit.empty() ? "" : " " + shown.unit), x + 9, y + 20, 56, 15, plain(colour::text, 12, Weight::Medium));

        // The lane: the track's own value until the first event, then each event holds.
        float gx = x + 69, gw = w - 69 - 6, gy = y + 5, gh = 37;
        p.fill(gx, gy + gh - 1, gw, 1, colour::line);
        auto y_of = [&](int v) { return gy + gh - 2 - gauge_of(pi, v) * (gh - 4); };
        juce::Path line;
        int value = mp.value;
        line.startNewSubPath(gx, y_of(value));
        for (unsigned step = 0; step < length; ++step) {
            for (unsigned e = 0; e < motion.count; ++e) {
                const auto& ev = motion.events[e];
                if (ev.track == s.ui.track && ev.pattern == track.pattern && ev.param == param && ev.step == step) value = ev.value;
            }
            float x0 = gx + gw * step / length, x1 = gx + gw * (step + 1) / length;
            line.lineTo(x0, y_of(value));
            line.lineTo(x1, y_of(value));
        }
        p.g.setColour(shade);
        p.g.strokePath(line, juce::PathStrokeType(1.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
        if (s.playing) p.fill(gx + gw * (s.position % length) / length, y, 1, lh, colour::text);
    }
}

void soft(Context& c, int key) {
    static const PageId pages[] {PageId::Steps, PageId::Roll, PageId::Motion};
    c.open(pages[key % 3]);
}
} // namespace

const PageSpec motion_page {draw, play_pad, no_wheel, no_push, soft, [](Context&, int) {}}; // octave only on play pages

} // namespace melodee
