// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// CHOP (Chop on a sampler track; Chop again goes back to Trim): the MPC's
// Chop Mode on the last pad's region. Pads play their slice and pick it; the
// wheel picks one too. K1 the slice's start, K2 its end (a neighbour's end
// or start moves with it), K3 the chop type: Threshold, Regions, Beats
// (Melodee's) or Manual; Shift + K1 / K2 zoom at the slice's start or end
// (0-100%, smoothly, shell/zoom_view.h), Shift + K3 the type's amount
// (threshold, slices, beats a slice). Once K1 or K2 (or their zoom) was
// touched the wheel fine-edits that handle, as the MPC's encoder does, its
// line drawn wider, until a pad or Minus / Plus picks another slice. Editing
// a slice makes the chop Manual. Manual armed (K3 onto it, or Push): pad 1
// plays the region and the pads tapped meanwhile cut it, up to 16 slices.
// Shift + B1 Extract (the slice as a file, onto the next empty pad), Shift +
// B2 Split (the slice in two), Shift + B3 Merge (with the slice before);
// Erase + pad takes a slice out (it joins the one before). B3 lays every
// slice onto pads from the first free one as regions of the same file; B1
// drops the chop; B2 SNAP turns Zero Snap on or off, its LED lit while on
// (every cut onto a zero crossing, shell/chop.h; on by default, kept with
// the settings). The picture: the slices as bands, the one in hand lit with
// the pad it lands on as a chip above it, their numbers under them; the
// file's length at the top right, the slice's at the bottom right.
#include "melodee/engine/engine_host.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/shell/zoom_view.h"
#include "melodee/ui/waveform.h"
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;

juce::String seconds(uint32_t frames) {
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f s", frames / 44100.0);
    return text;
}

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    const auto& slices = s.ui.slices;
    uint32_t frames = s.sample.frames;
    Header h = header_of(s);
    h.left.push_back(chip("CHOP", c));
    h.left.push_back(pill(Icon::Scissors, juce::String(slices.count), c));
    h.right.push_back(tag_of(s, s.ui.track));
    if (s.ui.shift) frame(p, s, {{"EXTRACT", "SPLIT", "MERGE"}, -1, c}, h);
    else {
        SoftKeys keys {{"CANCEL", "SNAP", "TO PADS"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}};
        keys.toggles = 2; keys.lit = s.ui.settings.zero_snap ? 2 : 0;
        frame(p, s, keys, h);
    }
    float x = zone::pad, w = zone::width - 2 * zone::pad;
    if (frames) p.text(seconds(frames), x, zone::picture_y + 2, w, 14, plain(colour::dim, type::small, Weight::SemiBold), juce::Justification::centredRight);
    if (!frames || !slices.count) {
        icon(p, s.sample.error.empty() ? Icon::Clock : Icon::Warn, zone::width / 2.0f - 14, zone::picture_y + 44, 28, colour::dim, 1.4f);
        if (!s.sample.error.empty())
            p.text(juce::String(s.sample.error), x, zone::picture_y + 80, w, 16, p.fit(juce::String(s.sample.error), plain(colour::amber, type::small), w, type::floor),
                   juce::Justification::centred);
        return;
    }
    unsigned selected = std::min<unsigned>(s.ui.chop_slice, slices.count - 1);
    uint32_t region_from = slices.start[0], region_to = slices.end(slices.count - 1, frames);
    // In view: the region, or closer in at the slice's start or end (Shift + K1 / K2), its own peaks that close.
    float a = float(slices.start[selected]) / frames, b = float(slices.end(selected, frames)) / frames;
    float from = float(region_from) / frames, to = float(region_to) / frames;
    if (s.ui.chop_zoom) {
        FrameView view = chop_view(s.project, s.ui);
        if (view.width()) { from = float(view.from) / frames; to = float(view.to) / frames; }
    }
    WaveDetail detail;
    if (s.detail_to > s.detail_from) detail = {&s.detail, float(s.detail_from) / frames, float(s.detail_to) / frames};
    float y = zone::picture_y + 22, wh = 92;
    auto x_of = [&](float at) { return x + (at - from) / (to - from) * w; };
    // What the audition voice plays now (mock 5): the slice a pad started, with its head and wake, or Manual's take.
    PadSound audition;
    audition.sample = s.ui.chop_sample;
    WaveHead head;
    const PlayHead* playing = pad_head(s, sampler_pads, audition, frames, head);
    ChopSound sound = chop_sound(playing, slices, frames, s.ui.chop_take_at != 0);
    int sounding = sound.slice >= 0 && head.wake > 0 ? sound.slice : -1;
    float sa = 0, sb = 0; // the sounding slice, 0..1 of the file
    if (sounding >= 0) { sa = float(slices.start[sounding]) / frames; sb = float(slices.end(unsigned(sounding), frames)) / frames; }
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), int(w + 1), int(wh + 13)));
    for (unsigned n = 1; n < slices.count; n += 2) { // the slices as bands, every other one a shade darker
        float l = x_of(float(slices.start[n]) / frames), r = x_of(float(slices.end(n, frames)) / frames);
        p.fill(l, y, r - l, wh, colour::quiet);
    }
    p.fill(x_of(a), y, x_of(b) - x_of(a), wh, c.withAlpha(0.22f)); // the one in hand, lit
    waveform(p, x, y, w, wh, s.sample.overview, from, to, a, b, c, nullptr, 0, detail);
    if (sounding >= 0) { // its bars again, in full colour (dim ones too, while it plays), the head and its wake over them
        p.g.saveState();
        float l = std::floor(x_of(sa)), r = std::ceil(x_of(sb));
        p.g.reduceClipRegion(juce::Rectangle<int>(int(l), int(y), int(std::max(0.0f, r - l)), int(wh)));
        p.fill(l, y, r - l, wh, sounding == int(selected) ? colour::raised : colour::bg);
        p.fill(l, y, r - l, wh, c.withAlpha(0.17f * head.wake));
        waveform(p, x, y, w, wh, s.sample.overview, from, to, sa, sb, c, &head, 1, detail);
        p.g.restoreState();
    }
    // The numbers under the slices: where they would run into each other, some step aside; the slice in hand and
    // the one sounding always keep theirs, and no number sits on theirs.
    auto number_style = [&](unsigned n) {
        bool heard = int(n) == sounding;
        return plain(heard ? c : n == selected ? colour::text : colour::mid, type::small, heard || n == selected ? Weight::Bold : Weight::SemiBold);
    };
    auto number_span = [&](unsigned n) {
        float l = x_of(float(slices.start[n]) / frames) + 1;
        return std::pair<float, float> {l, l + p.width(juce::String(n + 1), number_style(n))};
    };
    auto kept_number = [&](unsigned n) { return n == selected || int(n) == sounding; };
    constexpr float number_gap = 3;
    float drawn_to = -1e9f;
    for (unsigned n = 0; n < slices.count; ++n) {
        float sx = x_of(float(slices.start[n]) / frames);
        bool edge = n == selected || n == selected + 1;
        if (n) p.fill(sx, y, 1, wh, edge ? colour::text : colour::line);
        auto [l, r] = number_span(n);
        bool show = kept_number(n);
        if (!show && l >= drawn_to + number_gap) {
            show = true;
            for (unsigned k = 0; k < slices.count && show; ++k) // keep clear of the numbers that always show
                if (k != n && kept_number(k)) {
                    auto [kl, kr] = number_span(k);
                    show = r + number_gap <= kl || l >= kr + number_gap;
                }
        }
        if (!show) continue;
        p.text(juce::String(n + 1), sx + 1, y + wh + 2, 16, 9, number_style(n));
        drawn_to = std::max(drawn_to, r);
    }
    float wide_start = s.ui.chop_fine && !s.ui.chop_focus ? 2 : 1, wide_end = s.ui.chop_fine && s.ui.chop_focus ? 2 : 1; // the wheel's
    p.fill(x_of(a), y, wide_start, wh, knob_shade(s.ui.track, 0));            // K1's handle
    p.fill(x_of(b) - wide_end, y, wide_end, wh, knob_shade(s.ui.track, 1));   // K2's
    // Manual's take: where the audition voice is heard in the region now (its region's start until it is heard).
    bool starting = s.ui.chop_take_at && !sound.take && s.now >= s.ui.chop_take_at && s.now - s.ui.chop_take_at < 250;
    bool taking = sound.take || starting;
    if (taking) p.fill(x_of(sound.take ? float(playing->at / frames) : float(region_from) / frames), y, 2, wh, colour::text);
    p.g.restoreState();
    if (sounding >= 0) draw_heads(p, x, y, w, wh, from, to, &head, 1);
    // Over the slice in hand, the pad it lands on as a chip (taking Manual's cuts: the scissors); its length below right.
    juce::String lands;
    if (!taking) {
        const auto& track = s.track();
        unsigned pad = 0, n = 0;
        for (; pad < sampler_pads; ++pad) if (!track.pads[pad].used() && n++ == selected) break;
        if (pad < sampler_pads) { char label[4]; pad_label(pad, label); lands = juce::String(label); }
    }
    auto chip_style = caps_text(colour::bg);
    float cw = 4 + 10 + (lands.isEmpty() ? 0 : 3 + p.width(lands, chip_style)) + 5;
    float cx = std::clamp((x_of(a) + x_of(b)) / 2 - cw / 2, x, x + w - cw), cy = zone::picture_y + 3;
    if (taking || lands.isNotEmpty()) {
        p.fill(cx, cy, cw, 15, c, 3);
        icon(p, taking ? Icon::Scissors : Icon::Pads, cx + 4, cy + 2.5f, 10, colour::bg, 1.6f);
        if (lands.isNotEmpty()) p.text(lands, cx + 17, cy, cw - 17, 15, chip_style);
    }
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f s", (slices.end(selected, frames) - slices.start[selected]) / 44100.0);
    p.text(text, x, y + wh + 13, w, 16, plain(colour::text, type::body, Weight::SemiBold), juce::Justification::centredRight);
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

// Minus / Plus, and the wheel before K1 or K2 was touched: the slice before or after.
void pick(Context& c, int delta) {
    if (!c.ui.slices.count) return;
    c.ui.chop_slice = uint8_t(std::clamp<int>(c.ui.chop_slice + delta, 0, int(c.ui.slices.count) - 1));
    c.ui.chop_fine = false;
}

void wheel(Context& c, int delta) {
    auto& ui = c.ui;
    if (!ui.chop_fine || !ui.slices.count) { pick(c, delta); return; }
    unsigned n = std::min<unsigned>(ui.chop_slice, ui.slices.count - 1), k = ui.chop_focus % 2;
    c.fine_edit(Binding {k ? Binding::Kind::ChopEnd : Binding::Kind::ChopStart, int32_t(n), 0, 1, 0}, delta);
    auto& pot = ui.pots[k];
    pot.engaged = false; pot.bound = 0; // the value left the pot: it picks it up again
    ui.turning = int8_t(k); ui.turned_at = c.now;
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
            if (ui.slices.split(ui.chop_slice, frames)) {
                unsigned cut = ui.chop_slice + 1u; // the new cut, onto a zero crossing with Zero Snap on
                ui.slices.move_start(cut, c.zero_cut(ui.slices.start[cut]), frames);
                ui.chop_mode = ChopMode::Manual; ui.chop_armed = false;
            }
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
    if (key == 1) c.toggle_zero_snap();
    if (key == 2) { c.chop_to_pads(); c.open(PageId::Pads); }
}
} // namespace

const PageSpec chop_page {draw, pad, wheel, push, soft, pick}; // Minus / Plus pick slices

} // namespace melodee
