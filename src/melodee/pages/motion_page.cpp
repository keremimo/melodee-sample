// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MOTION (B3 on a sequence page, or Seq held + B3; B3 again: MOD, then MACRO, pages/mod_page.cpp): pot moves recorded over the pattern, one lane per
// pot. Seq Record + turn a pot records (here or on any page); Erase + turn a
// pot asks, then (B3) clears that lane. The lanes are the parameters holding
// motion on the track's pattern (its engine's, its effects', or on a kit the
// last pad's own: level, tune, pan, envelope), then on a kit that pad's
// tune, level and pan, else the track's favourites. The picture is one large
// graph of the pattern with all three lanes on it, each in its knob's colour,
// the one in hand heavy and the others thin; the cards are its legend. Motion
// is the pattern's own (roadmap P1), at tick resolution; a lock (a pot turned
// with a step held on Step Edit or a note on Roll) is drawn as a short raised bar
// at its value for as long as it holds. Shift + B3 turns draw mode on or off: points
// set by hand (shell/motion_points.h). The wheel moves a cursor, Push adds a point
// on the lane drawn (the parameter turned last) that goes in a straight line to the
// next (Push on a point: line or hold), Erase takes it out, Shift + wheel moves it,
// its knob sets its value (Seq Record off).
#include "melodee/model/limits.h"
#include "melodee/model/pad_params.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/features.h"
#include "melodee/shell/motion_points.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    const auto& motion = s.pattern.motion;
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(edit_pattern_chip(s));
    h.left.push_back(s.ui.motion_draw ? pill(Icon::Motion, "DRAW", c) : pill(Icon::Motion, juce::String(unsigned(motion.size())))); // drawing: says so
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{"SEQUENCES", "ROLL", "MOTION"}, 2, c};
    if (mod_shown(s.ui)) { keys.dots[2] = 3; keys.dot_at[2] = 0; } // B3's cycle: MOTION, MOD, MACRO (pages/mod_page.cpp)
    if (s.ui.shift) keys = SoftKeys {{"SEQUENCES", "ROLL", "DRAW"}, s.ui.motion_draw ? 2 : -1, c}; // Shift + B3: draw mode, lit while on (B1, B2 as they are)
    frame(p, s, keys, h);

    // One graph: the pattern across, bars and beats marked, quarters of the range up.
    constexpr float x = zone::pad, y = zone::picture_y + 4, w = zone::width - 2 * zone::pad, gh = zone::picture_h - 8;
    p.fill(x, y, w, gh, colour::quiet, 3);
    unsigned length = std::max<unsigned>(s.pattern.length, 1), bar = std::max(1u, s.project.bar_ticks()), beat = std::max(1u, s.project.beat_ticks());
    unsigned beats = length / beat;
    for (unsigned b = 1; b < beats && beats <= 64; ++b) // the beats, the bars bolder
        p.fill(x + w * float(b * beat) / length - 0.5f, y, b * beat % bar ? 1.0f : 1.5f, gh, colour::bg.withAlpha(b * beat % bar ? 0.5f : 1.0f));
    for (float q : {0.25f, 0.5f, 0.75f}) p.fill(x, y + gh * q - 0.5f, w, 1, colour::bg);
    auto lanes = motion_lanes(s.project, s.ui, s.pattern);
    auto x_of = [&](uint32_t tick) { return x + w * float(std::min<uint32_t>(tick, length)) / float(length); };
    int hand = motion_hand(s.ui, s.now);
    bool drawing = s.ui.motion_draw;
    if (drawing) { // the lane drawn on is the one in hand, when it shows
        int drawn = motion_draw_lane(s.project, s.ui, s.pattern, s.now);
        for (int k = 0; k < 3; ++k) if (lanes[size_t(k)] == drawn && drawn >= 0) hand = k;
    }
    uint32_t cursor = motion_cursor(s.ui, s.pattern);
    if (drawing) { // the cursor: where Push adds a point (shell/motion_points.h)
        float cx = x_of(cursor);
        p.fill(cx - 0.5f, y, 1, gh, colour::text.withAlpha(0.35f));
        p.fill(cx - 3, y, 6, 2, colour::text.withAlpha(0.8f), 1);
    }
    float playhead_y = -1;
    auto ends = s.pattern.ramp_ends(); // each point's ramp_end(), in one pass
    for (unsigned order = 0; order < 3; ++order) { // the one in hand last, over the others
        unsigned lane = order == 2 ? unsigned(hand) : order < unsigned(hand) ? order : order + 1;
        if (lane > 2) continue;
        int param = lanes[lane]; // a lane: a parameter, or a kit pad's value on one pad (bindings.h)
        if (param < 0) continue;
        auto mp = motion_param(track, param, &s.project);
        if (!mp.info) continue;
        const auto& pi = *mp.info;
        auto y_of = [&](int v) { return y + gh - 3 - gauge_of(pi, v) * (gh - 6); };
        // The track's own value until the first point (that point's, when it was set by hand), then each point
        // holds, or goes in a line to the next (a ramp), the last to the end; locks drawn apart, over the line.
        juce::Path line;
        unsigned lp = lane_param(param), lk = lane_pad(param);
        int value = s.pattern.lane_value(0, lp, lk, mp.value); // a first point set by hand: level with it from the start
        line.startNewSubPath(x, y_of(value));
        uint32_t play_at = s.playing ? uint32_t(s.position % length) : 0;
        int value_now = s.pattern.lane_value(play_at, lp, lk, mp.value);
        bool ramping = false; // the point before goes in a line to this one
        unsigned points = 0;
        for (size_t n = 0; n < motion.size(); ++n) {
            const auto& m = motion[n];
            if (!m.on(lp, lk) || m.lock() || !m.within(length)) continue; // locks drawn apart
            float ex = x_of(m.tick);
            if (!ramping) line.lineTo(ex, y_of(value));
            value = m.value;
            line.lineTo(ex, y_of(value));
            ramping = ends[n] < motion.size();
            ++points;
        }
        line.lineTo(x + w, y_of(value));
        bool in_hand = int(lane) == hand;
        auto shade = knob_shade(s.ui.track, lane);
        p.g.setColour(shade.withAlpha(in_hand ? 1.0f : 0.7f));
        p.g.strokePath(line, juce::PathStrokeType(in_hand ? 2.2f : 1.2f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
        if (in_hand && drawing) // its points: those set by hand (and all, when few were recorded), the one under the cursor ringed
            for (const auto& m : motion) {
                if (!m.on(lp, lk) || m.lock() || !m.within(length) || (!m.ramp && points > 64 && m.tick != cursor)) continue;
                float px = x_of(m.tick), py = y_of(m.value);
                if (m.tick == cursor) p.circle(px, py, 5.5f, colour::text);
                p.circle(px, py, 3.5f, m.ramp ? shade : colour::bg);
                if (!m.ramp) p.ring(px - 3.5f, py - 3.5f, 7, 7, shade, 3.5f, 1.5f);
            }
        for (const auto& m : motion) { // locks: a bar at the value for as long as each holds
            if (!m.on(lp, lk) || !m.lock() || m.tick >= length) continue;
            float lx = x_of(m.tick), lw = std::max(x_of(m.tick + m.hold) - lx, 3.0f), ly = y_of(m.value);
            p.fill(lx, ly - (in_hand ? 2.5f : 1.5f), lw, in_hand ? 5.0f : 3.0f, shade.withAlpha(in_hand ? 1.0f : 0.7f), 1.5f);
            if (s.playing && play_at >= m.tick && play_at < m.tick + m.hold) value_now = m.value;
        }
        if (in_hand && s.playing) playhead_y = y_of(value_now);
    }
    if (s.playing) { // where it plays, the lane in hand's value there
        float px = x + w * float(s.position % length) / length;
        p.fill(px - 0.5f, y, 1, gh, colour::text.withAlpha(0.7f));
        if (playhead_y >= 0) p.circle(px, playhead_y, 4, colour::text);
    }
}

void wheel(Context& c, int delta) {
    if (!c.ui.motion_draw) return;
    if (c.ui.shift) { // Shift: the point under the cursor, nothing else
        c.ui.shift_used = true;
        if (!motion_point_move(c, delta)) c.notice("No point here");
        return;
    }
    motion_cursor_wheel(c, delta);
}

void soft(Context& c, int key) {
    if (c.ui.shift) { // Shift + B3: draw mode (B1, B2 as they are)
        if (key == 2) { c.ui.shift_used = true; motion_draw_toggle(c); return; }
    }
    if (key == 2) { // MOTION again, lit: its next page, MOD (the modulators, then MACRO; Full's, shell/features.h)
        if (mod_shown(c.ui)) { c.ui.mod_view = 0; c.open(PageId::ModPage); }
        return;
    }
    seq_hub_soft(c, key);
}
} // namespace

const PageSpec motion_page {draw, play_pad, wheel, motion_point_push, soft, [](Context&, int) {}}; // octave only on play pages

} // namespace melodee
