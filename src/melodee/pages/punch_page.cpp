// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PAD FX (Pad FX; parity phase 7b, as on the MPC): the bank's sixteen
//   punches on the pads: the MPC's own (Half Speed ... Reverb, bank MPC)
//   or Melodee's 31 on two more banks (Pad Bank or Minus / Plus). Hold pads
//   to punch, up to four at once; a fifth bypasses the one engaged first
//   until a newer one lets go. Pressure is each punch's depth. B1 LATCH keeps
//   the punch pressed last at its depth (B1 again on it lets go; Push does the
//   same). B2 aims them at the master or the selected track; B3 opens Pad
//   Punch on a sampler track. An MPC punch in hand: K1-K3 its own (Shift:
//   depth, snap, tail); Melodee's: K1 depth, K2 snap grid, K3 tail.
// PAD PUNCH (Pad FX again, or B3): each kit pad carries a punch of its own;
//   hit the pad and it alone plays punched while the kit runs on. Pressure
//   is depth. K1 picks the punch of the last pad hit, K2 its rate, K3 the
//   depth when pressure is not used.
#include "melodee/engine/pad_fx_info.h"
#include "melodee/engine/punch.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/waveform.h"
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;

bool kit(const Snapshot& s) { return s.track().engine == EngineKind::Sampler; }

SoftKeys punch_keys(const Snapshot& s, int active) {
    juce::Colour c = active == 0 ? colour::text : colour_of(s);
    return {{"MASTER", "TRACK", kit(s) ? "PAD" : ""}, active, c};
}

// ---------- Pad FX ----------
const char* bank_name(unsigned bank) { static const char* const names[] {"MELODEE 1", "MELODEE 2", "MPC"}; return names[bank % punch_banks]; }

void punch_draw(Paint& p, const Snapshot& s) {
    bool master = s.project.punch_target == 0;
    juce::Colour c = master ? colour::text : colour_of(s);
    unsigned bank = s.project.punch_bank % punch_banks;
    int selected = s.ui.punch_selected, at = selected >= 0 ? s.ui.engaged_at(selected) : -1;
    bool latched = at >= 0 && s.ui.engaged[size_t(at)].latched;
    unsigned sounding = 0;
    for (auto k : s.ui.punch_slot) sounding += k >= 0;
    Header h = header_of(s);
    h.left.push_back(pill("PAD FX"));
    h.left.push_back(pill(bank_name(bank), bank == 2 ? c : colour::mid));
    if (s.ui.engaged_count) h.left.push_back(pill(juce::String(sounding) + " / 4 ON", c));
    if (master) h.right.push_back(pill("MASTER", colour::text));
    else h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"LATCH", master ? "MASTER" : "TRACK", kit(s) ? "PAD" : ""}, latched ? 0 : -1, c}, h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f;
    constexpr float w = (zone::width - 2 * zone::pad - 3 * gap) / 4, ch = (145 - 1.5f - 3 * gap) / 4;
    for (unsigned n = 0; n < 16; ++n) {
        int kind = punch_on_pad(bank, n);
        unsigned row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        if (kind < 0) { p.fill(x, y, w, ch, colour::quiet, 4); continue; }
        int e = s.ui.engaged_at(kind);
        bool on = s.ui.punch_sounding(kind), waiting = e >= 0 && !on, latch = e >= 0 && s.ui.engaged[size_t(e)].latched;
        p.fill(x, y, w, ch, on ? c : waiting ? colour::raised : colour::surf, 4);
        if (latch || kind == selected) p.ring(x, y, w, ch, on ? colour::bg : c, 4, kind == selected ? 1.5f : 1);
        p.text(juce::String(n + 1), x + 6, y + 4, 20, 8, plain(on ? colour::bg : colour::dim, 6.5f, Weight::SemiBold));
        juce::String mark = latch ? "LATCH" : waiting ? "BYPASS" : on ? "ON" : "";
        if (mark.isNotEmpty())
            p.text(mark, x + w - 40, y + 4, 34, 8, caps_text(on ? colour::bg : c, 6), juce::Justification::right);
        juce::String name = punch_name(PunchKind(kind));
        p.text(name, x + 6, y + ch - 16, w - 12, 13, p.fit(name, plain(on ? colour::bg : c, 11, Weight::SemiBold), w - 12, 6.5f));
    }
}
void punch_pad(Context& c, unsigned pad, uint8_t, bool down) { c.punch_pad(pad, down); }
void punch_soft(Context& c, int key) {
    if (key == 0) { c.latch_punch(); return; }
    if (key == 1) { c.set_punch_target(c.project.punch_target ? 0 : 1); return; }
    if (c.track().engine == EngineKind::Sampler) c.open(PageId::PadPunch);
}
void punch_bank(Context& c, int way) { c.step_punch_bank(way); }

// ---------- Pad Punch ----------
void pad_punch_draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    auto c = colour_of(s);
    if (!kit(s)) {
        frame(p, s, punch_keys(s, 2), header_of(s));
        p.text("Pad Punch is for a sampler track", zone::pad, zone::picture_y + 60, zone::width - 2 * zone::pad, 16,
               plain(colour::dim, 11), juce::Justification::centred);
        return;
    }
    unsigned bank = track.bank % sampler_banks, with = 0;
    for (const auto& pad : track.pads) with += pad.used() && pad.punch;
    Header h = header_of(s);
    h.left.push_back(pill(juce::String(with) + " OF " + juce::String(sampler_pads) + " PADS"));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, punch_keys(s, 2), h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f, grid_w = 165;
    constexpr float w = (grid_w - 3 * gap) / 4, ch = (145 - 1.5f - 3 * gap) / 4;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned k = bank * 16 + n, row = 3 - n / 4, column = n % 4;
        const auto& pad = track.pads[k];
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool held = s.ui.pad_punch_held == int(k);
        p.fill(x, y, w, ch, pad.used() && pad.punch ? colour::surf : held ? colour::surf : colour::quiet, 4);
        if (held) { // pressure fills the pad from below
            float press = s.ui.pad_punch_pressure ? s.ui.pad_punch_pressure / 127.0f : pad.punch_depth / 100.0f;
            p.fill(x, y + ch * (1 - press), w, ch * press, c, 4);
        }
        float press = held ? (s.ui.pad_punch_pressure ? s.ui.pad_punch_pressure / 127.0f : pad.punch_depth / 100.0f) : 0;
        juce::String name = pad_name(s, k, true);
        if (name.isNotEmpty())
            p.text(name, x + 3.5f, y + 3, w - 7, 8, p.fit(name, plain(press > 0.85f ? colour::bg : colour::mid, 6.5f, Weight::SemiBold), w - 7, 5));
        if (pad.used() && pad.punch) {
            juce::String fx = punch_name(PunchKind(pad.punch - 1));
            p.text(fx, x + 3.5f, y + ch - 11, w - 7, 8, p.fit(fx, caps_text(held ? colour::bg : c, 6), w - 7, 4.5f));
        }
    }
    // The last pad hit, and its punch.
    const auto& pad = track.pads[track.last_pad % sampler_pads];
    float x = zone::pad + grid_w + 10, y = zone::picture_y + 3, iw = zone::width - zone::pad - x;
    char label[4]; pad_label(track.last_pad, label);
    juce::String title = juce::String("PAD ") + juce::String(label);
    if (pad.used()) title += juce::String::fromUTF8(" \xc2\xb7 ") + pad_name(s, track.last_pad % sampler_pads, false).toUpperCase();
    p.text(title, x, y, iw, 10, p.fit(title, caps_text(colour::mid), iw, 6));
    juce::String fx = !pad.used() ? juce::String("Empty") : pad.punch ? juce::String(punch_title(PunchKind(pad.punch - 1))) : juce::String("No punch");
    p.text(fx, x, y + 11, iw, 28, p.fit(fx, plain(pad.used() && pad.punch ? colour::text : colour::dim, 24, Weight::SemiBold), iw, 12));
    p.text(pad.punch ? "on this pad alone" : "K1 gives it one", x, y + 40, iw, 12, plain(colour::sec, 10));
    if (pad.used() && s.sample.frames) {
        uint32_t end = pad.end ? pad.end : s.sample.frames;
        float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
        waveform(p, x, y + 58, iw, 26, s.sample.overview, from, to, from, to, c, 1);
    }
    bool pressing = s.ui.pad_punch_held >= 0 && s.ui.pad_punch_pressure;
    int depth = pressing ? s.ui.pad_punch_pressure * 100 / 127 : pad.punch_depth;
    p.text("PRESSURE", x, y + 98, iw / 2, 10, caps_text(colour::mid, 7));
    p.text("DEPTH " + juce::String(depth) + " %", x + iw / 2, y + 98, iw / 2, 10, caps_text(colour::sec, 7), juce::Justification::right);
    p.fill(x, y + 112, iw, 4, colour::line, 2);
    p.fill(x, y + 112, iw * depth / 100.0f, 4, knob_shade(s.ui.track, 2), 2);
}
void pad_punch_pad(Context& c, unsigned pad, uint8_t velocity, bool down) { c.pad_punch(pad, velocity, down); }
void pad_punch_soft(Context& c, int key) {
    if (key == 2) return;
    c.set_punch_target(unsigned(key));
    c.open(PageId::Punch);
}
void pad_punch_bank(Context& c, int way) {
    c.punch_release();
    c.track().bank = uint8_t((c.track().bank + sampler_banks + way) % sampler_banks);
}
} // namespace

const PageSpec punch_page {punch_draw, punch_pad, no_wheel, [](Context& c) { c.latch_punch(); }, punch_soft, punch_bank};
const PageSpec pad_punch_page {pad_punch_draw, pad_punch_pad, no_wheel, no_push, pad_punch_soft, pad_punch_bank};

} // namespace melodee
