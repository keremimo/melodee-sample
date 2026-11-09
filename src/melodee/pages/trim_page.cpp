// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// TRIM (Push on a sampler track or a pad): where the last pad's region of
// its sample starts and ends. K1 start, K2 end, K3 zoom; closer in, each pot
// covers a smaller stretch, and Shift + wheel moves the last one by a
// millisecond. The handles wear the colours of the pots that move them.
// (The sampler's other pages, Pad to Cloud, come with their own boards.)
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
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
    char label[4]; pad_label(track.last_pad, label);
    Header h = header_of(s);
    juce::String name = pad.used() ? juce::String(s.project.samples[pad.sample % sample_slots].name) : juce::String("EMPTY");
    h.left.push_back(pill(juce::String("PAD ") + juce::String(label) + juce::String::fromUTF8(" \xc2\xb7 ") + name, c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"", "TRIM", ""}, 1, c}, h);
    p.text("Trim", zone::pad, zone::picture_y, 200, 22, plain(colour::text, type::title, Weight::SemiBold));
    uint32_t frames = s.sample.frames;
    if (!pad.used() || !frames) {
        p.text(pad.used() ? (s.sample.error.empty() ? "Loading" : juce::String(s.sample.error)) : juce::String("No sample on this pad"),
               zone::pad, zone::picture_y + 40, zone::width - 12, 40, plain(colour::dim, 11, Weight::Medium), juce::Justification::centred);
        return;
    }
    uint32_t end = pad.end ? pad.end : frames, start = std::min(pad.start, end);
    // What is in view: all of it, or a window round the region, closer with each zoom step.
    float region_from = float(start) / frames, region_to = float(end) / frames;
    float window = 1.0f / float(1 << s.ui.trim_zoom), centre = (region_from + region_to) / 2;
    float from = std::clamp(centre - window / 2, 0.0f, 1.0f - window), to = from + window;
    float x = zone::pad, y = zone::picture_y + 27, w = zone::width - 2 * zone::pad, h2 = 82;
    auto x_of = [&](float at) { return x + (at - from) / (to - from) * w; };
    float sx = x_of(region_from), ex = x_of(region_to);
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), int(w + 1), int(h2 + 4)));
    p.fill(sx, y, ex - sx, h2, c.withAlpha(0.12f));
    waveform(p, x, y, w, h2, s.sample.overview, from, to, region_from, region_to, c);
    p.fill(sx, y, 1, h2, knob_shade(s.ui.track, 0));
    p.fill(sx - 2.5f, y - 2.5f, 5, 5, knob_shade(s.ui.track, 0), 1);
    p.fill(ex - 1, y, 1, h2, knob_shade(s.ui.track, 1));
    p.fill(ex - 3.5f, y - 2.5f, 5, 5, knob_shade(s.ui.track, 1), 1);
    p.g.restoreState();
    auto axis = caps_text(colour::dim, 7);
    float ay = y + h2 + 6;
    p.text(seconds(uint32_t(from * frames)), x, ay, 80, 9, axis);
    p.text("KEPT " + seconds(end - start), x + w / 2 - 60, ay, 120, 9, caps_text(colour::sec, 7), juce::Justification::centred);
    p.text(seconds(uint32_t(to * frames)), x + w - 80, ay, 80, 9, axis, juce::Justification::centredRight);
}

void push(Context&) {}
} // namespace

const PageSpec trim_page {draw, play_pad, no_wheel, push, [](Context&, int) {}, [](Context& c, int way) {
    c.track().bank = uint8_t((c.track().bank + sampler_banks + way) % sampler_banks);
}};

} // namespace melodee
