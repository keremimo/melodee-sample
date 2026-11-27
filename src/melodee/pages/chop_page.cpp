// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// CHOP (Chop on a sampler track; Chop again goes back to Trim): the MPC's
// Chop Mode on the last pad's region. Pads play their slice and pick it;
// Minus / Plus pick the one before or after. Past sixteen slices Pad Bank steps the pads through
// them (A 1-16, B 17-32 ...; the bank of the slice in hand, its pill BANK B
// beside the count). K1 the slice's start, K2 its end (a neighbour's end
// or start moves with it), K3 the chop type: Threshold, Regions, Beats
// (Melodee's) or Manual; Shift + K1 / K2 zoom at the slice's start or end
// (0-100%, smoothly, shell/zoom_view.h), Shift + K3 the type's amount
// (threshold, slices, Beats' division: 1/16 ... 4 bars). Fixing a cut: the
// wheel moves the picked slice's start (its end once K2 was touched, until
// another slice is picked), as the MPC's encoder does, that line drawn wider;
// unless Shift + K set a zoom, the view closes in on it as it turns (about
// 120 ms across, eased) and opens out again on the next pad, Minus / Plus or
// K1 / K2; once the cut has been still for 150 ms the slice plays once
// (Context::chop_tick), from 300 ms before a start (its last second for an
// end). SHIFT + the wheel jumps that cut from hit to hit (Threshold's hits,
// marked at the waveform's foot). Bottom left: what the wheel moves, and zoomed in, the
// view's place in the region and how many times closer. A cut
// made or moved by hand (K1 / K2, Split, Manual's taps) stays through a new
// chop by type or amount, drawn in the track's colour with a tab on top, and
// one taken out (Erase, Merge) stays out, as Simpler keeps its manual slices.
// Manual with no cut yet (K3 onto it, Erase + Chop; chop_fresh): pad 1
// blinks red and plays the region; then the red walks, the pad of the slice
// the next tap makes, and only it cuts (after pad 16 the pads turn to bank B,
// its pad 1 red; up to 128 slices, shell/chop_mode.cpp). A lit pad tapped
// meanwhile plays again from its slice, the cuts after it gone. Stopped short
// of the end (another control, the page left), the sound stops and the red
// stays on the next pad: tapped, it plays on from the last cut and the take
// goes on; played to the end, no red. A take is one undo step.
// Shift + B1 Extract (the slice as a file, onto the next empty pad), Shift +
// B2 Split (the slice in two), Shift + B3 Merge (with the slice before);
// Erase + pad takes a slice out (it joins the one before; slice 1 the one after). B3 TO PADS asks
// how they play, as Simpler's Playback (cards POLY, MONO, THRU, KIT, starting on
// the last pick, kept with the settings; the wheel moves, Push lays them, B1
// BACK), then lays every slice onto pads from the first free one as regions
// of the same file (MONO: one mute group; THRU: that, each on to the
// region's end). The PATTERN card (Push turns it on or off, the cards stay)
// writes a pattern of the slices too, as Live's Slice to New MIDI Track
// (Context::chop_to_pads; one undo step with the pads); B1
// drops the chop; B2 is free: Zero Snap is always on (every cut onto a zero
// crossing, shell/chop.h). Erase + Chop takes every cut out (one slice
// again; on Manual pad 1 red for a new take); Push only lays TO PADS' cards.
// The picture: the slices as bands, the one in hand lit with
// the pad it lands on as a chip above it, their numbers under them; the
// file's length at the top right (on Beats with the sample's own tempo before
// it, where it has one), the slice's at the bottom right.
#include "melodee/engine/engine_host.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/shell/chop.h"
#include "melodee/shell/zoom_view.h"
#include "melodee/ui/waveform.h"
#include "melodee/ui/widgets.h"
#include "melodee/model/slice_banks.h"
#include "melodee/model/slice_hits.h"
#include <cmath>
#include <cstdio>
#include <tuple>

namespace melodee {
namespace {
using namespace ui;

constexpr unsigned to_pads_pattern = unsigned(SlicePlay::Count), to_pads_rows = to_pads_pattern + 1; // TO PADS' cards: the four, PATTERN

// Every change of view eases there over view_ease_ms (the wheel closing in on a cut and the next pad opening out
// again, Shift + K): about the point at the same place across the screen in both views, the width by the same
// factor each frame, so what is zoomed into stays where it is. A move along at about the same width glides, unless it
// is small (the wheel carrying the view at its edges): that shows at once. A frame with no time passed since the
// last (the previews), long after it (the page just opened) or for another sample shows the view as it is.
struct ViewEase {
    float from = 0, to = 1;         // shown last
    float was_from = 0, was_to = 1; // where the ease began
    float aim_from = 0, aim_to = 1; // where it goes
    uint32_t began = 0, drawn = 0;
    uint16_t sample = no_sample;
};
ViewEase view_ease; // the screen thread's
constexpr uint32_t view_ease_ms = 180;

std::pair<float, float> eased_view(float aim_from, float aim_to, uint32_t now, uint16_t sample) {
    auto& e = view_ease;
    if (sample != e.sample || now <= e.drawn || now - e.drawn > 250) {
        e = {aim_from, aim_to, aim_from, aim_to, aim_from, aim_to, now, now, sample};
        return {aim_from, aim_to};
    }
    e.drawn = now;
    if (aim_from != e.aim_from || aim_to != e.aim_to) {
        const float w = e.to - e.from;
        const bool small = std::abs((aim_to - aim_from) - w) < w * 0.01f && std::abs(aim_from - e.from) < w * 0.25f;
        e.aim_from = aim_from; e.aim_to = aim_to;
        e.was_from = small ? aim_from : e.from; e.was_to = small ? aim_to : e.to;
        e.began = small ? now - view_ease_ms : now;
    }
    float t = std::min(1.0f, float(now - e.began) / view_ease_ms);
    if (t >= 1) { e.from = e.aim_from; e.to = e.aim_to; return {e.from, e.to}; }
    t = 1 - (1 - t) * (1 - t) * (1 - t); // eases out
    const float w0 = e.was_to - e.was_from, w1 = e.aim_to - e.aim_from;
    const float u = std::abs(w0 - w1) > w0 * 1e-4f ? (e.aim_from - e.was_from) / (w0 - w1) : -1; // the point that stays
    if (u >= 0 && u <= 1 && w0 > 0 && w1 > 0) {
        const float w = w0 * std::pow(w1 / w0, t);
        e.from = e.was_from + (w0 - w) * u; e.to = e.from + w;
    } else {
        e.from = e.was_from + (e.aim_from - e.was_from) * t; e.to = e.was_to + (e.aim_to - e.was_to) * t;
    }
    return {e.from, e.to};
}

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
    if (s.ui.chop_cleaning) h.left.push_back(pill(Icon::Layers, "SEPARATING", c)); // CLEAN's stems
    else h.left.push_back(pill(Icon::Scissors, juce::String(slices.count), c));
    if (slices.count > 16) // the sixteen slices on the pads (Pad Bank steps them)
        h.left.push_back(pill(Icon::Pads, "BANK " + juce::String::charToString(juce::juce_wchar('A' + slice_bank(s.ui.chop_slice))), colour::text));
    h.right.push_back(tag_of(s, s.ui.track));
    if (s.ui.sheet_open && slices.count) frame(p, s, {{"BACK", "", ""}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h); // TO PADS' cards
    else if (s.ui.shift) frame(p, s, {{"EXTRACT", "SPLIT", "MERGE"}, -1, c}, h);
    else {
        const bool clean = s.ui.chop_mode == ChopMode::Auto && !s.ui.chop_cleaning; // B2 CLEAN, its LED lit while on
        SoftKeys keys {{"CANCEL", clean ? "CLEAN" : "", "TO PADS"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}};
        if (clean) {
            keys.toggles = 2;
            keys.lit = s.ui.chop_clean && s.ui.chop_clean_for == s.ui.chop_sample && s.ui.chop_clean_slot >= 0 ? 2 : 0;
        }
        frame(p, s, keys, h);
    }
    float x = zone::pad, w = zone::width - 2 * zone::pad;
    // The file's length at the top right: drawn after the slice's chip, and left out where the chip covers it.
    juce::String length;
    const auto length_style = plain(colour::dim, type::small, Weight::SemiBold);
    // Auto: what it found the sample to be, in the track's colour, before the tempo and length.
    const SoundKind kind = s.ui.chop_mode == ChopMode::Auto ? s.sample.autos.kind : SoundKind::None;
    const juce::String kind_word = sound_kind_name(kind);
    const auto kind_style = caps_text(c);
    auto top_right = [&] {
        p.text(length, x, zone::picture_y + 2, w, 14, length_style, juce::Justification::centredRight);
        if (kind_word.isNotEmpty())
            p.text(kind_word, x, zone::picture_y + 2, w - p.width(length, length_style) - 6, 14, kind_style, juce::Justification::centredRight);
    };
    auto top_right_width = [&] { return p.width(length, length_style) + (kind_word.isEmpty() ? 0 : p.width(kind_word, kind_style) + 6); };
    if (frames) {
        length = seconds(frames);
        const bool tempo = (s.ui.chop_mode == ChopMode::Beats || kind == SoundKind::Drums || kind == SoundKind::Music) && s.sample.beats.found();
        if (tempo) length = juce::String(s.sample.beats.tempo / 10.0, 1) + " BPM  " + length; // the tempo it cuts at, the sample's own
        if (s.ui.chop_cleaning && s.stems.busy) { // CLEAN's stems: the time left
            char left[24];
            std::snprintf(left, sizeof(left), "%u:%02u left", s.stems.seconds_left / 60, s.stems.seconds_left % 60);
            length = left;
        }
        if (!slices.count) top_right();
    }
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
    std::tie(from, to) = eased_view(from, to, s.now, s.ui.chop_sample);
    WaveDetail detail;
    if (s.detail_to > s.detail_from) detail = {&s.detail, float(s.detail_from) / frames, float(s.detail_to) / frames};
    float y = zone::picture_y + 22, wh = 92;
    auto x_of = [&](float at) { return x + (at - from) / (to - from) * w; };
    // What plays now (mock 5): the slice a pad started (through the kit pad, or the audition voice with no kit pad),
    // with its head and wake, or Manual's take (the audition voice); the newer of the two.
    PadSound audition;
    audition.sample = s.ui.chop_sample;
    WaveHead head;
    const PlayHead* playing = pad_head(s, sampler_pads, audition, frames, head);
    const int kit = chop_kit_pad(s.track(), s.ui.chop_sample);
    if (kit >= 0) {
        WaveHead kit_head;
        const PlayHead* hit = pad_head(s, unsigned(kit), audition, frames, kit_head); // the heads are newest first
        if (hit && chop_sound(hit, slices, frames, false).slice >= 0 && (!playing || hit < playing)) { playing = hit; head = kit_head; }
    }
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
    const WaveSides sides = wave_sides(s);
    waveform(p, x, y, w, wh, s.sample.overview, sides, from, to, a, b, c, nullptr, 0, detail);
    if (sounding >= 0) { // its bars again, in full colour (dim ones too, while it plays), the head and its wake over them
        p.g.saveState();
        float l = std::floor(x_of(sa)), r = std::ceil(x_of(sb));
        p.g.reduceClipRegion(juce::Rectangle<int>(int(l), int(y), int(std::max(0.0f, r - l)), int(wh)));
        p.fill(l, y, r - l, wh, sounding == int(selected) ? colour::raised : colour::bg);
        p.fill(l, y, r - l, wh, c.withAlpha(0.17f * head.wake));
        waveform(p, x, y, w, wh, s.sample.overview, sides, from, to, sa, sb, c, &head, 1, detail);
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
        const bool hand = n && s.ui.chop_hand.pinned(slices.start[n]); // made or moved by hand: kept through a new chop
        if (n) p.fill(sx, y, 1, wh, edge ? colour::text : hand ? c : colour::line);
        if (hand) p.fill(sx - 2, y, 5, 3, c); // its tab
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
    // The hits (shell/chop.h mark_floor): a small mark at the foot of each, where SHIFT + the wheel lands a cut.
    {
        const uint8_t floor = mark_floor(s.ui.chop_amount[size_t(ChopMode::Threshold)]);
        for (unsigned n = 0; n < s.sample.hit_count && n < max_hits; ++n) {
            const auto& h = s.sample.hits[n];
            if (!h.frame || h.strength < floor) continue;
            const float hx = x_of(float(h.frame) / frames);
            if (hx >= x - 2 && hx <= x + w + 2) p.fill(hx - 1.5f, y + wh - 3, 3, 3, colour::sec, 1);
        }
    }
    // A slice heard backwards (its own Reverse, Shift + Loop, against the kit pad's): the reverse arrow at its top left.
    for (unsigned n = 0; n < slices.count; ++n) {
        if (!slice_heard_reversed(s.track(), s.ui.chop_sample, slices, n)) continue;
        const float l = std::max(x, x_of(float(slices.start[n]) / frames)), r = std::min(x + w, x_of(float(slices.end(n, frames)) / frames));
        if (r - l >= 15) icon(p, Icon::Reverse, l + 2, y + 2, 11, n == selected ? colour::text : c, 1.4f);
    }
    float wide_start = !s.ui.chop_focus ? 2 : 1, wide_end = s.ui.chop_focus ? 2 : 1; // the one the wheel moves
    p.fill(x_of(a), y, wide_start, wh, knob_shade(s.ui.track, 0));            // K1's handle
    p.fill(x_of(b) - wide_end, y, wide_end, wh, knob_shade(s.ui.track, 1));   // K2's
    // Manual's take: where the audition voice is heard in the region now (its region's start until it is heard).
    bool starting = s.ui.chop_take_at && !sound.take && s.now >= s.ui.chop_take_at && s.now - s.ui.chop_take_at < 250;
    bool taking = sound.take || starting;
    if (taking) p.fill(x_of(sound.take ? float(playing->at / frames) : float(s.ui.chop_take_from) / frames), y, 2, wh, colour::text);
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
    const bool chip_shown = taking || lands.isNotEmpty();
    if (chip_shown) {
        p.fill(cx, cy, cw, 15, c, 3);
        icon(p, taking ? Icon::Scissors : Icon::Pads, cx + 4, cy + 2.5f, 10, colour::bg, 1.6f);
        if (lands.isNotEmpty()) p.text(lands, cx + 17, cy, cw - 17, 15, chip_style);
    }
    if (!chip_shown || cx + cw + 6 < x + w - top_right_width()) top_right();
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f s", (slices.end(selected, frames) - slices.start[selected]) / 44100.0);
    p.text(text, x, y + wh + 13, w, 16, plain(colour::text, type::body, Weight::SemiBold), juce::Justification::centredRight);
    // Bottom left: what the wheel moves (the slice's START or END, in that K's colour), and zoomed in, the view's place
    // in the region (a strip, the view in the track's colour) and how many times closer. Not while a take plays.
    if (!chop_take_playing(s.project, s.ui, s.now)) {
        const float by = y + wh + 13;
        const juce::Colour job_colour = knob_shade(s.ui.track, s.ui.chop_focus ? 1 : 0);
        icon(p, Icon::Wheel, x, by + 2, 12, job_colour, 1.6f);
        float bx = x + 16;
        bx += p.text(s.ui.chop_focus ? "END" : "START", bx, by, 60, 16, caps_text(job_colour)) + 10;
        const float region = float(region_to - region_from) / frames, shown = to - from, rf = float(region_from) / frames;
        if (region > 0 && shown > 0 && shown < region * 0.98f) {
            const float sw = 64, sh = 5, sy = by + 5.5f;
            p.fill(bx, sy, sw, sh, colour::line, 2);
            const float l = bx + std::clamp((from - rf) / region, 0.0f, 1.0f) * sw, r = bx + std::clamp((to - rf) / region, 0.0f, 1.0f) * sw;
            p.fill(std::min(l, bx + sw - 2), sy, std::max(2.0f, r - l), sh, c, 2);
            const double times = double(region) / shown;
            p.text(juce::String::fromUTF8("\xc3\x97") + (times < 10 ? juce::String(times, 1) : juce::String(int(std::lround(times)))),
                   bx + sw + 6, by, 48, 16, caps_text(c));
        }
    }
    if (s.ui.sheet_open) { // TO PADS asks how the slices play, as Simpler's Playback: the wheel moves, Push lays them
        CardSheet m;
        m.chip = "TO PADS"; m.chip_colour = c; m.accent = c;
        m.title = juce::String(slices.count) + " slices";
        unsigned first = 0;
        while (first < sampler_pads && s.track().pads[first].used()) ++first;
        if (first < sampler_pads) { char label[4]; pad_label(first, label); m.detail = "from " + juce::String(label); }
        m.items[0] = card_item(Icon::Layers, slice_play_name(SlicePlay::Poly), "overlap");
        m.items[1] = card_item(Icon::OneShot, slice_play_name(SlicePlay::Mono), "one at a time");
        m.items[2] = card_item(Icon::ArrowRight, slice_play_name(SlicePlay::Thru), "on to the end");
        const size_t sounds = kit_sounds(slices, frames, s.sample.hits, s.sample.hit_count, s.sample.autos).size();
        m.items[3] = card_item(Icon::Pads, slice_play_name(SlicePlay::Kit), juce::String(sounds) + (sounds == 1 ? " sound" : " sounds"));
        const bool pattern = s.ui.settings.slice_pattern; // Push turns it on or off; the cards stay
        m.items[to_pads_pattern] = card_item(Icon::Pattern, "PATTERN", pattern ? "ON" : "OFF", pattern);
        m.count = to_pads_rows; m.current = s.ui.sheet_row % m.count;
        card_sheet(p, m);
    }
}

void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    auto& ui = c.ui;
    if (!down) { c.slice_release(pad); return; } // a slice hit let go: as long as it was held (issue 24)
    ui.sheet_open = false; // a pad leaves TO PADS' cards and plays as ever
    if (ui.erase) { // the slice out: it joins the one before (slice 1: the one after), the rest move down a pad
        const unsigned slice = pad_slice(chop_bank_slice(c.project, ui, c.now), pad); // in the bank in view
        if (slice < ui.slices.count && ui.slices.count > 1) // slice hits from there on: asked first (issue 24)
            c.chop_change(slice ? slice - 1 : 0, [&c, slice] { c.chop_erase(slice); });
        return;
    }
    c.chop_tap(pad, velocity);
}

// Minus / Plus: the slice before or after, its start for the wheel, the view opened out again.
void pick(Context& c, int delta) {
    auto& ui = c.ui;
    if (!ui.slices.count) return;
    ui.chop_slice = uint8_t(std::clamp<int>(ui.chop_slice + delta, 0, int(ui.slices.count) - 1));
    ui.chop_fine = false; ui.chop_focus = 0; ui.chop_heard_at = 0;
    chop_unfollow(ui);
}

void wheel(Context& c, int delta) {
    auto& ui = c.ui;
    if (ui.sheet_open) { ui.sheet_row = uint8_t(std::clamp<int>(ui.sheet_row + delta, 0, int(to_pads_rows) - 1)); return; }
    const auto& sl = ui.slices;
    const uint32_t frames = chop_frames(c.project, ui);
    if (!sl.count || !frames) return;
    const uint32_t region_from = sl.start[0], region_to = sl.end(sl.count - 1, frames);
    if (chop_take_playing(c.project, ui, c.now)) return; // a take playing: only the pads act
    unsigned n = std::min<unsigned>(ui.chop_slice, sl.count - 1), k = ui.chop_focus % 2; // the picked slice's start, or its end after K2
    chop_follow(ui, region_to - region_from); // closes in on the cut, unless Shift + K set a zoom
    if (ui.shift) { // SHIFT + the wheel: that cut onto the next hit the way it turns, a hit a detent
        ui.shift_used = true;
        c.chop_jump(delta);
        return;
    }
    c.fine_edit(Binding {k ? Binding::Kind::ChopEnd : Binding::Kind::ChopStart, int32_t(n), 0, 1, 0}, delta);
    auto& pot = ui.pots[k];
    pot.engaged = false; pot.bound = 0; // the value left the pot: it picks it up again
    ui.turning = int8_t(k); ui.turned_at = c.now;
}

// TO PADS' card picked: POLY, MONO, THRU or KIT lays the slices that way (and their pattern, PATTERN on), the choice
// kept for next time, and their sound opened; PATTERN turns the pattern on or off, the cards staying.
void lay(Context& c) {
    auto& ui = c.ui;
    if (ui.sheet_row % to_pads_rows == to_pads_pattern) {
        ui.settings.slice_pattern = !ui.settings.slice_pattern;
        if (c.store) c.store->save_settings(ui.settings.text());
        return;
    }
    ui.sheet_open = false;
    ui.settings.slice_play = uint8_t(ui.sheet_row % unsigned(SlicePlay::Count));
    if (c.store) c.store->save_settings(ui.settings.text());
    c.chop_to_pads(SlicePlay(ui.settings.slice_play), ui.settings.slice_pattern);
    c.open(PageId::Stage); // the slices' sound (Sample held shows their pads)
}

void push(Context& c) { // TO PADS' cards: lays them; else nothing (it once started a take afresh, the cuts lost)
    if (c.ui.sheet_open) lay(c);
}

void soft(Context& c, int key) {
    auto& ui = c.ui;
    if (ui.sheet_open) { if (key == 0) c.back(); return; } // TO PADS' cards: B1 BACK; the wheel and Push do the rest
    if (ui.shift) {
        if (key == 0) c.extract_slice();
        if (key == 1) c.chop_change(ui.chop_slice, [&c] { // slice hits on it and after: asked first (issue 24)
            auto& ui = c.ui;
            uint32_t frames = ui.chop_sample < sample_slots ? c.project.samples[ui.chop_sample].frames : 0;
            if (ui.slices.split(ui.chop_slice, frames)) {
                unsigned cut = ui.chop_slice + 1u; // the new cut, onto a zero crossing with Zero Snap on
                ui.slices.move_start(cut, c.zero_cut(ui.slices.start[cut]), frames);
                ui.chop_hand.pin(ui.slices.start[cut]); // a cut by hand: it stays through a new chop
            }
            else c.notice("Split: too short");
        });
        if (key == 2) {
            if (ui.chop_slice == 0) { c.notice("Merge: the first slice"); return; }
            c.chop_change(ui.chop_slice - 1u, [&c] { // slice hits on the one before and after: asked first (issue 24)
                auto& ui = c.ui;
                if (ui.chop_slice == 0 || ui.chop_slice >= ui.slices.count) return;
                ui.chop_hand.take_out(ui.slices.start[ui.chop_slice]); // a new chop leaves it out
                ui.slices.remove(ui.chop_slice);
                ui.chop_slice = uint8_t(ui.chop_slice - 1);
            });
        }
        return;
    }
    if (key == 0 && ui.chop_cleaning) { if (c.library) c.library->stop_stems(); return; } // CANCEL: CLEAN's stems called off
    if (key == 0) c.back();
    if (key == 1 && ui.chop_mode == ChopMode::Auto) c.chop_clean_key();
    if (key == 2 && ui.slices.count) { ui.sheet_open = true; ui.sheet_row = uint8_t(ui.settings.slice_play % unsigned(SlicePlay::Count)); } // asks: POLY, MONO, THRU or KIT
}
} // namespace

const PageSpec chop_page {draw, pad, wheel, push, soft, pick}; // Minus / Plus pick slices

} // namespace melodee
