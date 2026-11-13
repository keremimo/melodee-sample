// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PAD FX (Pad FX; parity phase 7b, as on the MPC): the bank's sixteen
//   punches on the pads: the MPC's own (Half Speed ... Reverb, bank MPC)
//   or Melodee's 31 on two more banks (Pad Bank or Minus / Plus). Hold pads
//   to punch, up to four at once; a fifth bypasses the one engaged first
//   until a newer one lets go. Pressure is each punch's depth. B1 LATCH keeps
//   the punch pressed last at its depth (B1 again on it lets go; Push does the
//   same). B2 aims them at the master or the selected track. Shift + B3
//   QUANTIZE (on at first): while playing, a punch starts on its snap grid's
//   next line; off, at once (letting go is at once either way), the header's
//   magnet saying OFF. An MPC punch in
//   hand: K1-K3 its own (Shift: depth, snap, tail); Melodee's: K1 depth, K2
//   snap grid, K3 tail. Round 2, pad-fx C "punches you can keep": over the
//   named pads, the bar the selected track's pattern is in, a row per punch
//   (named at its left, and its pad in its colour), each drawn where it
//   sounds in the pass it is played. The punches of one pass are a take; a
//   punch in a later pass begins a new one. Until B3 KEEP only the live
//   punches sound (its LED lit while a take waits); KEEP lays the take in
//   the pattern, looping with it like Motion (drawn quieter), quiet while a
//   live punch sounds; Erase + KEEP takes them out.
#include "melodee/engine/pad_fx_info.h"
#include "melodee/engine/punch.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/shell/ask.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace melodee {
namespace {
using namespace ui;

// ---------- Pad FX ----------
const char* bank_name(unsigned bank) { static const char* const names[] {"MELODEE 1", "MELODEE 2", "MPC"}; return names[bank % punch_banks]; }

// A punch in the bar drawn (pad-fx C): where it sounds, in the pattern's ticks, and whether the pattern keeps it.
struct BarPunch { uint32_t tick = 0, length = 0; uint8_t kind = 0; bool kept = false; };

void punch_draw(Paint& p, const Snapshot& s) {
    bool master = s.project.punch_target == 0;
    juce::Colour c = master ? colour::text : colour_of(s);
    unsigned bank = s.project.punch_bank % punch_banks;
    int selected = s.ui.punch_selected, at = selected >= 0 ? s.ui.engaged_at(selected) : -1;
    bool latched = at >= 0 && s.ui.engaged[size_t(at)].latched;
    Header h = header_of(s);
    h.left.push_back(pill("PAD FX"));
    h.left.push_back(pill(bank_name(bank), bank == 2 ? c : colour::mid));
    // Shift + B3 quantize off: the magnet says OFF (as Patterns' Rec Quantize), on the right (two chips at most on the left).
    if (!s.project.punch_quantize) h.right.push_back(pill(Icon::Magnet, "OFF", colour::dim));
    if (master) h.right.push_back(pill("MASTER", colour::text));
    else h.right.push_back(tag_of(s, s.ui.track));
    // The pattern the selected track plays, where it is, and the take waiting for KEEP (pad-fx C): its punches as the
    // pattern's ticks at the track's speed. Until KEEP only the live punches sound, and the bar draws the take only in
    // the pass it is played; the key's LED says one waits.
    const auto& pattern = s.pattern;
    uint32_t length = std::max<uint32_t>(pattern.length, 1), bar = std::max(1u, s.project.bar_ticks());
    int pos = s.playing ? s.positions[s.ui.track % track_count] : -1;
    uint32_t b0 = pos >= 0 ? uint32_t(pos) / bar * bar : 0;
    Speed sp = speed_of(s.track().speed);
    auto written = [&](uint64_t ticks) { return ticks * sp.num / sp.den; };
    std::array<PunchSpan, UiState::span_capacity> spans;
    for (unsigned k = 0; k < s.ui.span_count; ++k) {
        spans[k] = s.ui.spans[k];
        spans[k].from = written(spans[k].from);
        if (spans[k].until) spans[k].until = written(spans[k].until);
    }
    bool waiting = pos >= 0 && !heard_punches(spans.data(), s.ui.span_count, written(s.ticks), uint32_t(pos), length).empty();
    SoftKeys keys {{"LATCH", master ? "MASTER" : "TRACK", "KEEP"}, -1, c};
    keys.toggles = 1 | 4;
    keys.lit = uint8_t((latched ? 1 : 0) | (waiting ? 4 : 0));
    keys.glyphs = {Icon::None, Icon::None, Icon::Check};
    if (s.ui.shift) { // Shift + B3: quantize, as Flex Beat's B3 (its LED says which)
        keys = SoftKeys {{"", "", "QUANTIZE"}, -1, c};
        keys.toggles = 4;
        keys.lit = s.project.punch_quantize ? 4 : 0;
    }
    frame(p, s, keys, h);

    // Across the top, the bar the selected track's pattern is in: what was punched in this pass of it, as it is
    // played, and what the pattern keeps; a row per punch named at the left.
    std::vector<BarPunch> all;
    for (const auto& e : pattern.punches) all.push_back({e.tick, e.length, e.kind, true});
    if (pos >= 0 && s.ui.span_count) {
        uint64_t pass = written(s.ticks) >= uint32_t(pos) ? written(s.ticks) - uint32_t(pos) : 0; // where this pass began
        unsigned live = 0;
        for (unsigned k = 0; k < s.ui.span_count; ++k) {
            if (spans[k].until && spans[k].until <= pass) continue; // a pass ago: not drawn again until it is kept
            spans[live] = spans[k];
            spans[live].from = std::max(spans[live].from, pass);
            ++live;
        }
        for (const auto& e : heard_punches(spans.data(), live, written(s.ticks), uint32_t(pos), length))
            all.push_back({e.tick, e.length, e.kind, false});
    }
    // Each one's part in this bar (a punch running past the pattern's end goes on from its start).
    struct Piece { float from, to; uint8_t kind; bool kept; };
    std::vector<Piece> pieces;
    for (const auto& e : all) {
        uint64_t a = e.tick, b = uint64_t(e.tick) + e.length;
        for (int wrap = 0; wrap < 2; ++wrap, a = 0, b = b > length ? b - length : 0) {
            uint64_t from = std::max<uint64_t>(a, b0), to = std::min<uint64_t>({b, uint64_t(length), uint64_t(b0) + bar});
            if (to > from) pieces.push_back({float(from - b0) / bar, float(to - b0) / bar, e.kind, e.kept});
            if (b <= length) break;
        }
    }
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& x, const Piece& y) { return x.from < y.from; });
    std::vector<uint8_t> rows; // a row per punch, as they come in the bar, four at most
    for (const auto& piece : pieces)
        if (std::find(rows.begin(), rows.end(), piece.kind) == rows.end() && rows.size() < 4) rows.push_back(piece.kind);
    auto row_of = [&](int kind) { auto it = std::find(rows.begin(), rows.end(), uint8_t(kind)); return it == rows.end() ? -1 : int(it - rows.begin()); };

    constexpr float sx = zone::pad, sy = zone::picture_y + 3, sw = zone::width - 2 * zone::pad, sh = 50, lw = 68;
    constexpr float bx = sx + lw, bw = sw - lw;
    p.fill(bx, sy, bw, sh, colour::quiet, 3);
    unsigned beats = std::max(1u, bar / std::max(1u, s.project.beat_ticks()));
    for (unsigned k = 1; k < beats; ++k) p.fill(bx + bw * k / beats - 0.75f, sy, 1.5f, sh, colour::bg);
    if (b0 + bar > length) { float end = bx + bw * float(length - b0) / bar; p.fill(end, sy, bx + bw - end, sh, colour::bg.withAlpha(0.5f), 3); } // past the end
    float rh = sh / float(std::max<size_t>(3, rows.size())), span_h = std::max(6.0f, rh - 7);
    juce::StringArray row_names; // one case for the rows
    for (size_t r = 0; r < rows.size(); ++r) row_names.add(punch_title(PunchKind(rows[r] % punch_count)));
    bool row_capitals = cells_in_capitals(p, row_names, plain(colour::text, type::small, Weight::SemiBold), lw - 6);
    for (size_t r = 0; r < rows.size(); ++r) {
        auto style = plain(knob_shade(s.ui.track, unsigned(r)), type::small, Weight::SemiBold);
        juce::String name = cell_name(row_names[int(r)], row_capitals);
        p.text(name, sx, sy + r * rh, lw - 6, rh, p.fit(name, style, lw - 6, type::floor));
    }
    for (const auto& piece : pieces) {
        int row = row_of(piece.kind);
        if (row < 0) continue;
        auto shade = knob_shade(s.ui.track, unsigned(row));
        float x = bx + piece.from * bw, pw = std::max(3.0f, (piece.to - piece.from) * bw - 2), y = sy + row * rh + (rh - span_h) / 2;
        p.fill(x + 1, y, pw, span_h, piece.kept ? shade.withAlpha(0.45f) : shade, 3); // kept with the pattern: quieter
    }
    if (pos >= 0) p.fill(bx + bw * float(uint32_t(pos) - b0) / bar - 0.5f, sy, 1, sh, colour::text.withAlpha(0.7f)); // the playhead

    // Below, the pads as they sit on the MPC: each punch by name, in its row's colour while it is in the bar; the
    // ones sounding lit, the ones waiting raised, a latched one underlined, the one in hand ringed. Erase held: the
    // ones the pattern keeps red (Erase + pad takes them out), the rest dim.
    constexpr float gap = 3, gy = sy + sh + 5, gh = zone::knobs_y - 2 - gy;
    constexpr float cw = (sw - 3 * gap) / 4, ch = (gh - 3 * gap) / 4;
    juce::StringArray pad_names; // one case for the sixteen
    for (unsigned n = 0; n < 16; ++n) if (punch_on_pad(bank, n) >= 0) pad_names.add(punch_title(PunchKind(punch_on_pad(bank, n))));
    bool pad_capitals = cells_in_capitals(p, pad_names, plain(colour::text, type::small, Weight::SemiBold), cw - 8);
    for (unsigned n = 0; n < 16; ++n) {
        int kind = punch_on_pad(bank, n);
        float x = sx + (n % 4) * (cw + gap), y = gy + (3 - n / 4) * (ch + gap);
        if (kind < 0) { p.fill(x, y, cw, ch, colour::quiet, 3); continue; }
        int e = s.ui.engaged_at(kind), row = row_of(kind);
        bool on = s.ui.punch_sounding(kind), waiting = e >= 0 && !on, latch = e >= 0 && s.ui.engaged[size_t(e)].latched;
        p.fill(x, y, cw, ch, on ? c : waiting ? colour::raised : colour::surf, 3);
        if (kind == selected) p.ring(x, y, cw, ch, on ? colour::bg : c, 3, 1.5f);
        if (latch) p.fill(x + 5, y + ch - 3, cw - 10, 1.5f, on ? colour::bg : c, 0.75f);
        bool kept = std::any_of(pattern.punches.begin(), pattern.punches.end(), [&](const PatternPunch& k) { return k.kind == uint8_t(kind); });
        auto ink = s.ui.erase ? (kept ? colour::rec : colour::dim) : on ? colour::bg : row >= 0 ? knob_shade(s.ui.track, unsigned(row)) : colour::text;
        auto style = plain(ink, type::small, Weight::SemiBold);
        juce::String name = cell_name(punch_title(PunchKind(kind)), pad_capitals);
        p.text(name, x + 5, y - 0.5f, cw - 8, ch, p.fit(name, style, cw - 8, type::floor));
    }
}

void punch_pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (down && c.ui.erase) { c.erase_kept_punch(pad); return; } // Erase + pad: its kept punches go (a release still lets go)
    c.punch_pad(pad, down);
}
void punch_soft(Context& c, int key) {
    if (c.ui.shift) { // Shift + B3: quantize on or off; B1 and B2 have no Shift job
        c.ui.shift_used = true;
        if (key == 2) c.toggle_punch_quantize();
        return;
    }
    if (key == 0) { c.latch_punch(); return; }
    if (key == 1) { c.set_punch_target(c.project.punch_target ? 0 : 1); return; }
    if (c.ui.erase) { // Erase + KEEP: the kept punches go, once B3 says so
        c.ui.erase_used = true;
        if (!c.pattern().punches.empty()) ask(c, UiState::Ask::ErasePunches, 0);
        return;
    }
    c.keep_punches(); // KEEP (pad-fx C)
}
void punch_bank(Context& c, int way) { c.step_punch_bank(way); }

} // namespace

const PageSpec punch_page {punch_draw, punch_pad, no_wheel, [](Context& c) { c.latch_punch(); }, punch_soft, punch_bank};

} // namespace melodee
