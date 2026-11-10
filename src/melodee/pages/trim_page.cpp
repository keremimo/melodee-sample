// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// TRIM (Push on a sampler track or a pad, 1 of 10 sampler pages): where
// the last pad's region of its sample starts and ends, and where a loop
// goes round from, as the MPC's Trim. K1 start, K2 end, K3 loop point;
// Shift + K1-K3 zoom in at that handle; closer in, each pot covers a
// smaller stretch, and Shift + wheel moves the last one by a millisecond.
// Shift + B1 Loop Lock (the loop point on the start), Shift + B2 Normalize
// (the region's loudest point to 0 dB, undoable). The handles wear the
// colours of the pots that move them. B1 / B2 / B3 go round the sampler page groups; Push opens their map.
#include "melodee/pages/sampler_common.h"
#include "melodee/ui/waveform.h"
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;

juce::String seconds(uint32_t frames) {
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f S", frames / 44100.0);
    return text;
}

void draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    const auto& pad = track.pads[track.last_pad % sampler_pads];
    auto c = colour_of(s);
    if (s.ui.shift) { // the page's Shift jobs on B1 and B2, as on the MPC
        Header h = header_of(s);
        h.right.push_back(tag_of(s, s.ui.track));
        frame(p, s, {{pad.loop_lock ? "LOCK OFF" : "LOOP LOCK", "NORMALIZE", ""}, -1, c}, h);
    } else sampler_top(p, s, sampler_pad_pill(s), false);
    if (s.ui.shift) p.text("Trim", zone::pad, zone::picture_y, 200, 22, plain(colour::text, type::title, Weight::SemiBold));
    uint32_t frames = s.sample.frames;
    if (!pad.used() || !frames) {
        p.text(pad.used() ? (s.sample.error.empty() ? "Loading" : juce::String(s.sample.error)) : juce::String("No sample on this pad"),
               zone::pad, zone::picture_y + 40, zone::width - 12, 40, plain(colour::dim, type::small, Weight::Medium), juce::Justification::centred);
        return;
    }
    uint32_t end = pad.end ? pad.end : frames, start = std::min(pad.start, end);
    uint32_t loop = std::clamp(pad.loop_point(), start, end);
    // What is in view: all of it, or a window round the handle Shift + K zoomed at, closer with each step.
    float region_from = float(start) / frames, region_to = float(end) / frames, loop_at = float(loop) / frames;
    float window = 1.0f / float(1 << s.ui.trim_zoom);
    float centre = s.ui.trim_focus == 1 ? region_to : s.ui.trim_focus == 2 ? loop_at : region_from;
    if (s.ui.trim_zoom == 0) centre = 0.5f;
    float from = std::clamp(centre - window / 2, 0.0f, 1.0f - window), to = from + window;
    float x = zone::pad, y = zone::picture_y + 27, w = zone::width - 2 * zone::pad, h2 = 82;
    auto x_of = [&](float at) { return x + (at - from) / (to - from) * w; };
    float sx = x_of(region_from), ex = x_of(region_to), lx = x_of(loop_at);
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), int(w + 1), int(h2 + 4)));
    p.fill(sx, y, ex - sx, h2, c.withAlpha(0.12f));
    if (pad.loop) p.fill(lx, y + h2 - 6, ex - lx, 6, knob_shade(s.ui.track, 2).withAlpha(0.35f)); // what goes round
    waveform(p, x, y, w, h2, s.sample.overview, from, to, region_from, region_to, c);
    p.fill(sx, y, 1, h2, knob_shade(s.ui.track, 0));
    p.fill(sx - 2.5f, y - 2.5f, 5, 5, knob_shade(s.ui.track, 0), 1);
    p.fill(ex - 1, y, 1, h2, knob_shade(s.ui.track, 1));
    p.fill(ex - 3.5f, y - 2.5f, 5, 5, knob_shade(s.ui.track, 1), 1);
    if (!pad.loop_lock || pad.loop) { // the loop point: a dashed line, a flag at the foot
        auto lc = pad.loop ? knob_shade(s.ui.track, 2) : knob_shade(s.ui.track, 2).withAlpha(0.5f);
        for (float d = y; d < y + h2; d += 6) p.fill(lx, d, 1, 3, lc);
        p.fill(lx, y + h2 - 5, 6, 5, lc, 1);
    }
    p.g.restoreState();
    auto axis = caps_text(colour::dim);
    float ay = y + h2 + 6;
    p.text(seconds(uint32_t(from * frames)), x, ay, 80, 14, axis);
    juce::String kept = "KEPT " + seconds(end - start);
    if (pad.loop) kept += pad.loop_lock ? juce::String::fromUTF8(" \xc2\xb7 LOOP LOCKED") : juce::String::fromUTF8(" \xc2\xb7 LOOP ") + seconds(loop);
    if (pad.gain) kept += juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(pad.gain / 100.0, 1) + " DB";
    // The ends' times sit at the sides; a long middle line takes a line of its own below.
    auto kept_style = caps_text(colour::sec);
    float kw = p.width(kept, kept_style), side = std::max(p.width(seconds(uint32_t(from * frames)), axis), p.width(seconds(uint32_t(to * frames)), axis));
    bool own_line = kw > w - 2 * side - 16;
    p.text(kept, x, own_line ? ay + 15 : ay, w, 14, kept_style, juce::Justification::centred);
    p.text(seconds(uint32_t(to * frames)), x + w - 80, ay, 80, 14, axis, juce::Justification::centredRight);
}

void soft(Context& c, int key) {
    if (!c.ui.shift) { sampler_soft(c, key); return; }
    if (key == 0) c.toggle_loop_lock(); // Shift + B1
    if (key == 1) c.normalize();        // Shift + B2
}
} // namespace

const PageSpec trim_page {draw, play_pad, sampler_wheel, sampler_push, soft, sampler_bank};

} // namespace melodee
