// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MOTION (B3 on a sequence page, or Seq held + B3): pot moves recorded over the pattern, one lane per
// pot. Seq Record + turn a pot records (here or on any page); Erase + turn a
// pot asks, then (B3) clears that lane. The lanes are the parameters holding
// motion on the track's pattern (its engine's, its effects', or on a kit the
// last pad's own: level, tune, pan, envelope), then on a kit that pad's
// tune, level and pan, else the track's favourites.
#include "melodee/model/limits.h"
#include "melodee/model/pad_params.h"
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
    unsigned length = std::max<unsigned>(s.pattern.length, 1); // ticks
    for (unsigned lane = 0; lane < 3; ++lane) {
        int param = lanes[lane]; // a lane: a parameter, or a kit pad's value on one pad (bindings.h)
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
        bool pad_lane = pad_param_of(lane_param(param)) >= 0;
        if (pad_lane) { // whose lane: the kit pad, over the value's name
            char label[4];
            pad_label(lane_pad(param), label);
            p.text(label, x + 9, y + 1, 56, 13, caps_text(colour::mid));
        }
        float ly = pad_lane ? y + 13 : y + 5;
        p.text(pi.label, x + 9, ly, 56, 13, p.fit(pi.label, caps_text(shade), 56, type::floor));
        p.text(shown.value + (shown.unit.empty() ? "" : " " + shown.unit), x + 9, ly + 14, 56, 16, plain(colour::text, type::body, Weight::Medium));

        // The lane: the track's own value until the first event, then each event holds.
        float gx = x + 69, gw = w - 69 - 6, gy = y + 5, gh = 37;
        p.fill(gx, gy + gh - 1, gw, 1, colour::line);
        auto y_of = [&](int v) { return gy + gh - 2 - gauge_of(pi, v) * (gh - 4); };
        juce::Path line;
        int value = mp.value;
        line.startNewSubPath(gx, y_of(value));
        // The lane's events by sixteenth (patterns run to 128 bars: walk the events, not the steps).
        std::array<std::pair<uint16_t, int16_t>, motion_events> at {};
        unsigned count = 0, sixteenths = std::max(1u, (length + sixteenth_ticks - 1) / sixteenth_ticks);
        for (unsigned e = 0; e < motion.count; ++e) {
            const auto& ev = motion.events[e];
            if (ev.same_lane(s.ui.track, track.pattern, uint8_t(lane_param(param)), uint8_t(lane_pad(param))) && ev.step < sixteenths)
                at[count++] = {ev.step, ev.value};
        }
        std::sort(at.begin(), at.begin() + count);
        for (unsigned k = 0; k < count; ++k) {
            float ex = gx + gw * at[k].first / sixteenths;
            line.lineTo(ex, y_of(value));
            value = at[k].second;
            line.lineTo(ex, y_of(value));
        }
        line.lineTo(gx + gw, y_of(value));
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
