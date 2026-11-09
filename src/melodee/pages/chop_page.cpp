// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// CHOP (Chop on a sampler track): the MPC's own job on the last pad's
// sample. K1 mode: Hits, Even or Beats; K2 how readily a hit is heard, the
// slice count or the beats a slice; pads audition the slices, the wheel
// picks one, K3 moves its start, Push splits it with a marker, Erase + pad
// removes one. B3 lays the slices onto pads from the first free one; each
// pad is a region of the same file. B1 drops the chop.
#include "melodee/engine/engine_host.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/waveform.h"
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    const auto& slices = s.ui.slices;
    uint32_t frames = s.sample.frames;
    Header h = header_of(s);
    h.left.push_back(chip("CHOP", c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"DROP", "", "TO PADS"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}, h);
    float x = zone::pad, w = zone::width - 2 * zone::pad;
    p.text(juce::String(slices.count) + (slices.count == 1 ? " slice" : " slices"), x, zone::picture_y, 150, 22,
           plain(colour::text, type::title, Weight::SemiBold));
    char length[24];
    std::snprintf(length, sizeof(length), "%.1f S", frames / 44100.0);
    p.text(juce::String(s.sample.name) + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(length), x + 150, zone::picture_y, w - 150, 22,
           caps_text(colour::mid, 7), juce::Justification::centredRight);
    if (!frames) {
        p.text(s.sample.error.empty() ? "Loading" : juce::String(s.sample.error), x, zone::picture_y + 40, w, 40,
               plain(colour::dim, 11, Weight::Medium), juce::Justification::centred);
        return;
    }
    unsigned selected = std::min<unsigned>(s.ui.chop_slice, slices.count ? slices.count - 1 : 0);
    float y = zone::picture_y + 27, wh = 75;
    float a = float(slices.start[selected]) / frames, b = float(slices.end(selected, frames)) / frames;
    p.fill(x + a * w, y, (b - a) * w, wh, colour::raised);
    waveform(p, x, y, w, wh, s.sample.overview, 0, 1, a, b, c);
    for (unsigned n = 0; n < slices.count; ++n) {
        float sx = x + float(slices.start[n]) / frames * w;
        bool edge = n == selected || n == selected + 1;
        if (n) p.fill(sx, y, 1, wh, edge ? colour::text : colour::line);
        p.text(juce::String(n + 1), sx + 1, y + wh + 2, 16, 9, plain(n == selected ? colour::text : colour::mid, 7, n == selected ? Weight::Bold : Weight::SemiBold));
    }
    if (selected + 1 < slices.count) p.fill(x + b * w, y, 1, wh, colour::text);
    // The slice in hand and where it would land.
    float fy = y + wh + 14;
    p.fill(x, fy, w, 15, colour::quiet, 4);
    float lw = p.text("SLICE " + juce::String(selected + 1), x + 7, fy, 60, 15, caps_text(colour::mid, 7));
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f s", (slices.end(selected, frames) - slices.start[selected]) / 44100.0);
    p.text(text, x + 7 + lw + 6, fy, 80, 15, plain(colour::text, 11, Weight::Medium));
    const auto& track = s.track();
    unsigned pad = 0, n = 0;
    for (; pad < sampler_pads; ++pad) if (!track.pads[pad].used() && n++ == selected) break;
    if (pad < sampler_pads) {
        char label[4]; pad_label(pad, label);
        p.text(juce::String("LANDS ON PAD ") + juce::String(label), x, fy, w - 7, 15, caps_text(c, 7), juce::Justification::centredRight);
    }
}

void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    auto& ui = c.ui;
    if (!down || pad >= ui.slices.count) return;
    if (ui.erase) { ui.slices.remove(pad); ui.chop_slice = uint8_t(std::min<unsigned>(ui.chop_slice, ui.slices.count - 1)); return; }
    ui.chop_slice = uint8_t(pad);
    uint32_t frames = c.project.samples[ui.chop_sample % sample_slots].frames;
    Command a {Command::Type::Audition, ui.track, 0, 0, int16_t(velocity)};
    a.pad.sample = ui.chop_sample; a.pad.start = ui.slices.start[pad]; a.pad.end = ui.slices.end(pad, frames);
    c.send(a);
}

void wheel(Context& c, int delta) {
    if (!c.ui.slices.count) return;
    c.ui.chop_slice = uint8_t(std::clamp<int>(c.ui.chop_slice + delta, 0, int(c.ui.slices.count) - 1));
}

void push(Context& c) { // a marker in the middle of the slice in hand
    auto& ui = c.ui;
    uint32_t frames = c.project.samples[ui.chop_sample % sample_slots].frames;
    if (!frames || ui.chop_slice >= ui.slices.count) return;
    uint32_t a = ui.slices.start[ui.chop_slice], b = ui.slices.end(ui.chop_slice, frames);
    if (b - a > 882) ui.slices.add(a + (b - a) / 2); // not below 20 ms
}

void soft(Context& c, int key) {
    if (key == 0) c.back();
    if (key == 2) { c.chop_to_pads(); c.open(PageId::Pads); }
}
} // namespace

const PageSpec chop_page {draw, pad, wheel, push, soft, [](Context&, int) {}};

} // namespace melodee
