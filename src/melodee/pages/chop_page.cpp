// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// CHOP (Chop on a sampler track; Chop again goes back to Trim): the MPC's
// Chop Mode on the last pad's region. Pads play their slice and pick it; the
// wheel picks one too. K1 the slice's start, K2 its end (a neighbour's end
// or start moves with it), K3 the chop type: Threshold, Regions, Beats
// (Melodee's) or Manual; Shift + K1 / K2 zoom at the slice's start or end,
// Shift + K3 the type's amount (threshold, slices, beats a slice). Editing
// a slice makes the chop Manual. Manual armed (K3 onto it, or Push): pad 1
// plays the region and the pads tapped meanwhile cut it, up to 16 slices.
// Shift + B1 Extract (the slice as a file, onto the next empty pad), Shift +
// B2 Split (the slice in two), Shift + B3 Merge (with the slice before);
// Erase + pad takes a slice out (it joins the one before). B3 lays every
// slice onto pads from the first free one as regions of the same file; B1
// drops the chop.
#include "melodee/engine/engine_host.h"
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
    auto c = colour_of(s);
    const auto& slices = s.ui.slices;
    uint32_t frames = s.sample.frames;
    Header h = header_of(s);
    h.left.push_back(chip("CHOP", c));
    h.left.push_back(pill(juce::String(chop_mode_name(s.ui.chop_mode)).toUpperCase(), c));
    h.right.push_back(tag_of(s, s.ui.track));
    if (s.ui.shift) frame(p, s, {{"EXTRACT", "SPLIT", "MERGE"}, -1, c}, h);
    else frame(p, s, {{"DROP", "", "TO PADS"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}, h);
    float x = zone::pad, w = zone::width - 2 * zone::pad;
    p.text(juce::String(slices.count) + (slices.count == 1 ? " slice" : " slices"), x, zone::picture_y, 150, 22,
           plain(colour::text, type::title, Weight::SemiBold));
    p.text(juce::String(s.sample.name) + juce::String::fromUTF8(" \xc2\xb7 ") + seconds(frames), x + 150, zone::picture_y, w - 150, 22,
           caps_text(colour::mid, 7), juce::Justification::centredRight);
    if (!frames || !slices.count) {
        p.text(s.sample.error.empty() ? "Loading" : juce::String(s.sample.error), x, zone::picture_y + 40, w, 40,
               plain(colour::dim, 11, Weight::Medium), juce::Justification::centred);
        return;
    }
    unsigned selected = std::min<unsigned>(s.ui.chop_slice, slices.count - 1);
    uint32_t region_from = slices.start[0], region_to = slices.end(slices.count - 1, frames);
    // In view: the region (and a little round it), or closer in at the slice's start or end (Shift + K1 / K2).
    float a = float(slices.start[selected]) / frames, b = float(slices.end(selected, frames)) / frames;
    float from = float(region_from) / frames, to = float(region_to) / frames;
    if (s.ui.chop_zoom) {
        float window = std::max(1.0f / float(1 << s.ui.chop_zoom), 0.002f);
        float centre = s.ui.chop_focus ? b : a;
        from = std::clamp(centre - window / 2, 0.0f, 1.0f - window); to = from + window;
    }
    float y = zone::picture_y + 27, wh = 75;
    auto x_of = [&](float at) { return x + (at - from) / (to - from) * w; };
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), int(w + 1), int(wh + 13)));
    p.fill(x_of(a), y, x_of(b) - x_of(a), wh, colour::raised);
    waveform(p, x, y, w, wh, s.sample.overview, from, to, a, b, c);
    for (unsigned n = 0; n < slices.count; ++n) {
        float sx = x_of(float(slices.start[n]) / frames);
        bool edge = n == selected || n == selected + 1;
        if (n) p.fill(sx, y, 1, wh, edge ? colour::text : colour::line);
        p.text(juce::String(n + 1), sx + 1, y + wh + 2, 16, 9, plain(n == selected ? colour::text : colour::mid, 7, n == selected ? Weight::Bold : Weight::SemiBold));
    }
    p.fill(x_of(a), y, 1, wh, knob_shade(s.ui.track, 0));            // K1's handle
    p.fill(x_of(b) - 1, y, 1, wh, knob_shade(s.ui.track, 1));        // K2's
    // Manual's take: where the region is now.
    uint32_t heard = s.ui.chop_take_at && s.now >= s.ui.chop_take_at ? uint32_t(uint64_t(s.now - s.ui.chop_take_at) * 441 / 10) : 0;
    bool taking = s.ui.chop_take_at && region_from + heard < region_to;
    if (taking) p.fill(x_of(float(region_from + heard) / frames), y, 2, wh, colour::text);
    p.g.restoreState();
    // The slice in hand, and what Manual waits for.
    float fy = y + wh + 14;
    p.fill(x, fy, w, 15, colour::quiet, 4);
    float lw = p.text("SLICE " + juce::String(selected + 1), x + 7, fy, 60, 15, caps_text(colour::mid, 7));
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f s", (slices.end(selected, frames) - slices.start[selected]) / 44100.0);
    p.text(text, x + 7 + lw + 6, fy, 80, 15, plain(colour::text, 11, Weight::Medium));
    juce::String hint;
    if (taking) hint = "TAP PADS TO CUT";
    else if (s.ui.chop_mode == ChopMode::Manual && s.ui.chop_armed) hint = "PAD 1 PLAYS, TAP TO CUT";
    else {
        const auto& track = s.track();
        unsigned pad = 0, n = 0;
        for (; pad < sampler_pads; ++pad) if (!track.pads[pad].used() && n++ == selected) break;
        if (pad < sampler_pads) { char label[4]; pad_label(pad, label); hint = juce::String("LANDS ON PAD ") + juce::String(label); }
    }
    p.text(hint, x, fy, w - 7, 15, caps_text(c, 7), juce::Justification::centredRight);
}

void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    auto& ui = c.ui;
    if (!down) return;
    if (ui.erase) { // the slice out: it joins the one before (the chop is Manual from here)
        if (pad == 0 || pad >= ui.slices.count) return;
        ui.slices.remove(pad);
        ui.chop_mode = ChopMode::Manual; ui.chop_armed = false;
        ui.chop_slice = uint8_t(std::min<unsigned>(ui.chop_slice, ui.slices.count - 1));
        return;
    }
    c.chop_tap(pad, velocity);
}

void wheel(Context& c, int delta) {
    if (!c.ui.slices.count) return;
    c.ui.chop_slice = uint8_t(std::clamp<int>(c.ui.chop_slice + delta, 0, int(c.ui.slices.count) - 1));
}

void push(Context& c) { // Manual, armed: pad 1 plays the region again and the taps cut it afresh
    c.ui.chop_mode = ChopMode::Manual;
    c.rechop();
}

void soft(Context& c, int key) {
    auto& ui = c.ui;
    if (ui.shift) {
        uint32_t frames = ui.chop_sample < sample_slots ? c.project.samples[ui.chop_sample].frames : 0;
        if (key == 0) c.extract_slice();
        if (key == 1) {
            if (ui.slices.split(ui.chop_slice, frames)) { ui.chop_mode = ChopMode::Manual; ui.chop_armed = false; }
            else c.notice("Split: too short");
        }
        if (key == 2) {
            if (ui.chop_slice == 0) { c.notice("Merge: the first slice"); return; }
            ui.slices.remove(ui.chop_slice);
            ui.chop_slice = uint8_t(ui.chop_slice - 1);
            ui.chop_mode = ChopMode::Manual; ui.chop_armed = false;
        }
        return;
    }
    if (key == 0) c.back();
    if (key == 2) { c.chop_to_pads(); c.open(PageId::Pads); }
}
} // namespace

const PageSpec chop_page {draw, pad, wheel, push, soft, wheel}; // Minus / Plus pick slices as the wheel

} // namespace melodee
