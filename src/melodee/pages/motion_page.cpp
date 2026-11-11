// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MOTION (B3 on a sequence page, or Seq held + B3): pot moves recorded over the pattern, one lane per
// pot. Seq Record + turn a pot records (here or on any page); Erase + turn a
// pot asks, then (B3) clears that lane. The lanes are the parameters holding
// motion on the track's pattern (its engine's, its effects', or on a kit the
// last pad's own: level, tune, pan, envelope), then on a kit that pad's
// tune, level and pan, else the track's favourites. The picture is one large
// graph of the pattern with all three lanes on it, each in its knob's colour,
// the one in hand heavy and the others thin; the cards are its legend.
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
    h.left.push_back(pill(Icon::Motion, juce::String(motion.count) + " / " + juce::String(motion_events)));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"STEPS", "ROLL", "MOTION"}, 2, c}, h);

    // One graph: the pattern across, bars and beats marked, quarters of the range up.
    constexpr float x = zone::pad, y = zone::picture_y + 4, w = zone::width - 2 * zone::pad, gh = zone::picture_h - 8;
    p.fill(x, y, w, gh, colour::quiet, 3);
    unsigned length = std::max<unsigned>(s.pattern.length, 1), bar = std::max(1u, s.project.bar_ticks()), beat = std::max(1u, s.project.beat_ticks());
    unsigned beats = length / beat;
    for (unsigned b = 1; b < beats && beats <= 64; ++b) // the beats, the bars bolder
        p.fill(x + w * float(b * beat) / length - 0.5f, y, b * beat % bar ? 1.0f : 1.5f, gh, colour::bg.withAlpha(b * beat % bar ? 0.5f : 1.0f));
    for (float q : {0.25f, 0.5f, 0.75f}) p.fill(x, y + gh * q - 0.5f, w, 1, colour::bg);
    auto lanes = motion_lanes(s.project, s.ui);
    int hand = s.ui.turning >= 0 && s.now - s.ui.turned_at < 1500 ? s.ui.turning : s.ui.last_pot >= 0 ? s.ui.last_pot % 3 : 0;
    float playhead_y = -1;
    for (unsigned order = 0; order < 3; ++order) { // the one in hand last, over the others
        unsigned lane = order == 2 ? unsigned(hand) : order < unsigned(hand) ? order : order + 1;
        if (lane > 2) continue;
        int param = lanes[lane]; // a lane: a parameter, or a kit pad's value on one pad (bindings.h)
        if (param < 0) continue;
        auto mp = motion_param(track, param);
        if (!mp.info) continue;
        const auto& pi = *mp.info;
        auto y_of = [&](int v) { return y + gh - 3 - gauge_of(pi, v) * (gh - 6); };
        // The track's own value until the first event, then each event holds.
        juce::Path line;
        int value = mp.value;
        line.startNewSubPath(x, y_of(value));
        std::array<std::pair<uint16_t, int16_t>, motion_events> at {};
        unsigned count = 0, sixteenths = std::max(1u, (length + sixteenth_ticks - 1) / sixteenth_ticks);
        for (unsigned e = 0; e < motion.count; ++e) {
            const auto& ev = motion.events[e];
            if (ev.same_lane(s.ui.track, track.pattern, uint8_t(lane_param(param)), uint8_t(lane_pad(param))) && ev.step < sixteenths)
                at[count++] = {ev.step, ev.value};
        }
        std::sort(at.begin(), at.begin() + count);
        float play_x = s.playing ? x + w * float(s.position % length) / length : -1;
        int value_now = value;
        for (unsigned k = 0; k < count; ++k) {
            float ex = x + w * at[k].first / sixteenths;
            if (ex <= play_x) value_now = at[k].second;
            line.lineTo(ex, y_of(value));
            value = at[k].second;
            line.lineTo(ex, y_of(value));
        }
        line.lineTo(x + w, y_of(value));
        bool in_hand = int(lane) == hand;
        p.g.setColour(knob_shade(s.ui.track, lane).withAlpha(in_hand ? 1.0f : 0.7f));
        p.g.strokePath(line, juce::PathStrokeType(in_hand ? 2.2f : 1.2f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
        if (in_hand && s.playing) playhead_y = y_of(value_now);
    }
    if (s.playing) { // where it plays, the lane in hand's value there
        float px = x + w * float(s.position % length) / length;
        p.fill(px - 0.5f, y, 1, gh, colour::text.withAlpha(0.7f));
        if (playhead_y >= 0) p.circle(px, playhead_y, 4, colour::text);
    }
}

void soft(Context& c, int key) {
    static const PageId pages[] {PageId::Steps, PageId::Roll, PageId::Motion};
    c.open(pages[key % 3]);
}
} // namespace

const PageSpec motion_page {draw, play_pad, no_wheel, no_push, soft, [](Context&, int) {}}; // octave only on play pages

} // namespace melodee
