// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Stage on a kit pad holding a multisampled instrument (an SFZ, EXS, MPC keygroup, SF2 preset; round 3
// "instrument C"). ZONES (Trim's place) draws the whole map: a tile a zone, its keys across and its velocities up,
// over a keyboard of its range, the zone in hand outlined and named under it. AMP ENV, FILT ENV, TUNE, FILTER and
// KIT FILT keep a strip of the same keys on top, as a sample's pages keep the whole file over the page, and draw
// their own picture under it. The other kit pages draw the map. A file still being read is an outline, a silent
// one amber; what it asked for and is not played is named under the map.
#include "melodee/engine/engine_info.h"
#include "melodee/engine/sampler.h"
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/sampler_common.h"
#include <algorithm>
#include <cmath>

namespace melodee {
using namespace ui;

namespace {

constexpr float px = zone::pad, pw = zone::width - 2 * zone::pad, top = zone::picture_y, bottom = zone::picture_y + zone::picture_h;
const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
const juce::String dash = juce::String::fromUTF8("\xe2\x80\x93");

bool white_key(int k) { static const bool w[12] {true, false, true, false, true, true, false, true, false, true, false, true}; return w[((k % 12) + 12) % 12]; }

// Keys lo..hi (white at both ends) across x..x+w: each key's share of the line (l..r, the zones' edges) and the key
// itself as drawn (x, w).
struct KeyRow {
    int lo = 60, hi = 72;
    std::array<float, 128> l {}, r {}, x {}, w {};
    float centre(int k) const { return x[size_t(k)] + w[size_t(k)] / 2; }
};
KeyRow key_row(int lo, int hi, float x0, float width) {
    KeyRow k;
    lo = std::clamp(lo, 0, 127); hi = std::clamp(hi, lo, 127);
    while (lo > 0 && !white_key(lo)) --lo;
    while (hi < 127 && !white_key(hi)) ++hi;
    k.lo = lo; k.hi = hi;
    unsigned whites = 0;
    for (int n = lo; n <= hi; ++n) whites += white_key(n);
    const float ww = width / float(std::max(1u, whites)), bw = ww * 0.62f;
    float at = x0;
    std::array<float, 128> wx {};
    for (int n = lo; n <= hi; ++n) if (white_key(n)) { wx[size_t(n)] = at; at += ww; }
    for (int n = lo; n <= hi; ++n) {
        if (white_key(n)) {
            float a = wx[size_t(n)], b = a + ww;
            if (n - 1 >= lo && !white_key(n - 1)) a += bw / 2;
            if (n + 1 <= hi && !white_key(n + 1)) b -= bw / 2;
            k.l[size_t(n)] = a; k.r[size_t(n)] = b; k.x[size_t(n)] = wx[size_t(n)]; k.w[size_t(n)] = ww;
        } else {
            float bx = wx[size_t(n + 1)] - bw / 2;
            k.l[size_t(n)] = bx; k.r[size_t(n)] = bx + bw; k.x[size_t(n)] = bx; k.w[size_t(n)] = bw;
        }
    }
    return k;
}

// The instrument's keys, its pad's key with them, out to whole octaves, two at least.
KeyRow instrument_row(const LoadedSample& m, int pad_key, float x, float w) {
    int lo = std::min<int>(m.low_key, pad_key), hi = std::max<int>(m.high_key, pad_key);
    if (lo > hi) { lo = pad_key; hi = pad_key; }
    lo = lo / 12 * 12; hi = std::min(120, (hi + 11) / 12 * 12);
    while (hi - lo < 24) {
        bool grew = false;
        if (lo >= 12) { lo -= 12; grew = true; }
        if (hi - lo < 24 && hi <= 108) { hi += 12; grew = true; }
        if (!grew) break;
    }
    return key_row(lo, hi, x, w);
}

PartState state_of(const LoadedSample& m, const ZoneShape& z) {
    return z.part < m.part_state.size() ? m.part_state[z.part] : PartState::Landed;
}

// The zones as tiles over the row in y..y+h, velocity up: read ones in the colour (`lit` full), files being read as
// outlines, silent ones amber; `ring` outlines `lit` in the text colour (the zone in hand). Root dots where they fit.
void zone_tiles(Paint& p, const LoadedSample& m, const KeyRow& k, float y, float h, juce::Colour c, int lit, bool ring, bool roots, float alpha) {
    auto box = [&](const ZoneShape& z, float& x0, float& y0, float& w, float& hh) {
        int lo = std::max<int>(z.low_key, k.lo), hi = std::min<int>(z.high_key, k.hi);
        if (lo > hi) return false;
        x0 = k.l[size_t(lo)] + 0.5f; w = std::max(1.0f, k.r[size_t(hi)] - 0.5f - x0);
        y0 = y + h * (1 - z.high_velocity / 127.0f) + 0.5f; hh = std::max(1.0f, h * (z.high_velocity - z.low_velocity + 1) / 127.0f - 1);
        return true;
    };
    for (size_t n = 0; n < m.shapes.size(); ++n) {
        const auto& z = m.shapes[n];
        if (z.flags & ZoneShape::Release) continue; // a key let go: under the press's own zone
        float x0, y0, w, hh;
        if (!box(z, x0, y0, w, hh)) continue;
        const float r = std::min(2.0f, std::min(w, hh) / 3);
        switch (state_of(m, z)) {
        case PartState::Waiting: p.ring(x0, y0, w, hh, colour::dim, r, 1); break;
        case PartState::Failed: p.fill(x0, y0, w, hh, colour::amber.withAlpha(0.16f), r); p.ring(x0, y0, w, hh, colour::amber, r, 1); break;
        case PartState::Landed: p.fill(x0, y0, w, hh, int(n) == lit ? c : c.withAlpha(alpha), r); break;
        }
        if (roots && w >= 5 && hh >= 8 && z.root >= k.lo && z.root <= k.hi && z.root >= z.low_key && z.root <= z.high_key && state_of(m, z) == PartState::Landed)
            p.circle(k.centre(z.root), y0 + hh - 3.5f, 1.3f, int(n) == lit ? colour::bg : colour::text.withAlpha(0.7f));
    }
    if (ring && lit >= 0 && size_t(lit) < m.shapes.size()) {
        float x0, y0, w, hh;
        if (box(m.shapes[size_t(lit)], x0, y0, w, hh)) p.ring(x0 - 0.5f, y0 - 0.5f, w + 1, hh + 1, colour::text, 2, 1.2f);
    }
}

// The keyboard: white keys in `white`, black ones in BG, each in `lit` when it is lit.
void keyboard(Paint& p, const KeyRow& k, float y, float h, juce::Colour white, const std::array<juce::Colour, 128>& lit, const std::array<bool, 128>& on,
              int ghost = -1, juce::Colour ghost_c = {}) {
    for (int n = k.lo; n <= k.hi; ++n)
        if (white_key(n)) p.fill(k.x[size_t(n)] + 0.5f, y, k.w[size_t(n)] - 1, h, on[size_t(n)] ? lit[size_t(n)] : white, 1.5f);
    for (int n = k.lo; n <= k.hi; ++n)
        if (!white_key(n)) p.fill(k.x[size_t(n)], y, k.w[size_t(n)], h * 0.6f, on[size_t(n)] ? lit[size_t(n)] : colour::bg, 1);
    if (ghost >= k.lo && ghost <= k.hi) { // where the key was: outlined
        bool w = white_key(ghost);
        p.ring(k.x[size_t(ghost)] + (w ? 0.5f : 0), y, k.w[size_t(ghost)] - (w ? 1 : 0), w ? h : h * 0.6f, ghost_c, 1.5f, 1.5f);
    }
}

void c_labels(Paint& p, const KeyRow& k, float y, int marked, juce::Colour c) {
    for (int n = k.lo; n <= k.hi; ++n)
        if (n % 12 == 0) {
            auto name = juce::String(note_name(n));
            auto style = caps_text(n == marked ? c : colour::mid);
            float w = p.width(name, style) + 1, x = std::min(k.x[size_t(n)] + 1, px + pw - w); // the last one inside the row
            p.text(name, x, y, w, 12, style);
        }
}

juce::String format_tag(const std::string& path) {
    auto file = juce::String(path).upToLastOccurrenceOf("#", false, false);
    if (juce::String(path).containsChar('#') && file.endsWithIgnoreCase(".sf2")) return "SF2";
    auto ext = file.fromLastOccurrenceOf(".", false, false).toUpperCase();
    if (ext == "DSPRESET") return "DS";
    return ext.length() <= 4 ? ext : juce::String();
}

// A number and its word: "22 ZONES". Returns the x after it.
float fact(Paint& p, float x, float y, const juce::String& number, const juce::String& word, juce::Colour c = colour::text, juce::Colour wc = colour::mid) {
    auto ns = plain(c, type::body, Weight::SemiBold);
    float nw = p.width(number, ns);
    p.text(number, x, y, nw + 1, 14, ns);
    x += nw + 4;
    if (word.isNotEmpty()) { auto ws = caps_text(wc); float w = p.width(word, ws); p.text(word, x, y + 1, w + 1, 13, ws); x += w; }
    return x;
}

// What the instrument is: its format, zones, files (or how many are read), layers and round robin.
void facts_row(Paint& p, const LoadedSample& m, float y) {
    float x = px;
    auto tag = format_tag(m.path);
    if (tag.isNotEmpty()) {
        auto ts = caps_text(colour::sec);
        float tw = p.width(tag, ts) + 8;
        p.fill(x, y, tw, 14, colour::raised, 3);
        p.text(tag, x, y, tw, 14, ts, juce::Justification::centred);
        x += tw + 8;
    }
    x = fact(p, x, y, juce::String(m.zones), m.zones == 1 ? "ZONE" : "ZONES") + 10;
    const bool reading = m.parts_ready + m.parts_failed < m.parts;
    if (reading) { // a ring filling as the files land, and how many of how many
        float cx = x + 6, cy = y + 7, r = 5;
        juce::Path arc;
        arc.addCentredArc(cx, cy, r, r, 0, 0, juce::MathConstants<float>::twoPi * float(m.parts_ready + m.parts_failed) / float(std::max(1u, m.parts)), true);
        p.g.setColour(colour::line); p.g.drawEllipse(cx - r, cy - r, 2 * r, 2 * r, 2);
        p.g.setColour(colour::sec); p.g.strokePath(arc, juce::PathStrokeType(2, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        x = fact(p, x + 16, y, juce::String(m.parts_ready + m.parts_failed) + " / " + juce::String(m.parts), "READING") + 10;
    } else x = fact(p, x, y, juce::String(m.parts), m.parts == 1 ? "FILE" : "FILES") + 10;
    if (m.layers > 1) x = fact(p, x, y, juce::String(m.layers), "LAYERS") + 10;
    if (m.rounds > 1) { // round robin: the loop glyph and how many turns
        icon(p, Icon::Loop, x, y + 1, 12, colour::sec, 1.4f);
        fact(p, x + 15, y, juce::String(m.rounds), {});
    }
}

// What went wrong, amber, in up to `rows` rows from y: the silent files and why, the zones left out, and what it asked
// for that is not played, a chip each. Returns the rows it needs (drawing nothing when `draw` is false).
unsigned problems(Paint& p, const LoadedSample& m, float y, unsigned rows, bool draw) {
    struct Item { juce::String text; bool chip = false, warn = false, label = false; juce::Colour c; };
    std::vector<Item> items;
    if (m.parts_failed) {
        items.push_back({juce::String(m.parts_failed) + " SILENT", false, true, false, colour::amber});
        if (!m.error.empty()) items.push_back({juce::String(m.error).toUpperCase(), false, false, true, colour::amber.withAlpha(0.75f)});
    } else if (!m.error.empty()) items.push_back({juce::String(m.error).toUpperCase(), false, true, false, colour::amber});
    if (m.left) items.push_back({juce::String(m.left) + " LEFT OUT", false, false, false, colour::sec});
    if (!m.ignored.empty()) {
        items.push_back({"NOT PLAYED", false, false, true, colour::mid});
        for (auto& name : juce::StringArray::fromTokens(juce::String(m.ignored), ",", ""))
            if (name.trim().isNotEmpty()) items.push_back({name.trim(), true, false, false, colour::amber});
    }
    if (items.empty()) return 0;
    auto width_of = [&](const Item& it) {
        auto st = it.chip ? plain(it.c, type::small, Weight::Medium) : caps_text(it.c);
        return p.width(it.text, st) + (it.chip ? 10 : 0) + (it.warn ? 16 : 0);
    };
    unsigned row = 0;
    float x = px;
    for (size_t n = 0; n < items.size(); ++n) {
        const auto& it = items[n];
        float w = width_of(it);
        if (x > px && x + w > px + pw) { ++row; x = px; }
        if (row >= rows && draw) { // no room left: how many more
            juce::String more = "+" + juce::String(int(items.size() - n));
            p.text(more, px + pw - 22, y + 15 * float(rows - 1), 22, 14, caps_text(colour::amber), juce::Justification::centredRight);
            return rows;
        }
        if (draw) {
            float ry = y + 15 * float(row);
            float tx = x;
            if (it.warn) { icon(p, Icon::Warn, x, ry + 1, 12, colour::amber, 1.4f); tx += 16; }
            if (it.chip) {
                p.ring(x, ry, w, 14, colour::amber.withAlpha(0.55f), 3, 1);
                p.text(it.text, x + 5, ry, w - 10, 14, plain(it.c, type::small, Weight::Medium));
            } else p.text(it.text, tx, ry + 1, w, 13, caps_text(it.c));
        }
        x += w + (it.label ? 6 : 10);
    }
    return row + 1;
}

// Volume and pan as the sample pages have them: thin meters at the sides of y..y+h.
void side_meters(Paint& p, const Snapshot& s, const PadSound& pad, float y, float h, juce::Colour c) {
    p.fill(px, y, 5, h, colour::line, 2.5f);
    float vol = pad.level / 127.0f;
    p.fill(px, y + h * (1 - vol), 5, h * vol, c, 2.5f);
    p.fill(px + pw - 5, y, 5, h, colour::line, 2.5f);
    p.fill(px + pw - 7, y + h / 2 - 0.5f, 9, 1, colour::dim);
    p.fill(px + pw - 7, y + h / 2 - 3 + pad.pan / 50.0f * (h / 2 - 3), 9, 6, knob_shade(s.ui.track, 2), 1.5f);
}

// The whole map from y0 to y1: facts, tiles, keys and their Cs, what went wrong; ZONES adds the zone in hand under
// it. `lit`: the zone outlined (ZONES) or the one a hit plays.
void map_picture(Paint& p, const Snapshot& s, float y1, int lit, bool zones_page) {
    const auto& m = s.sample;
    const auto& pad = s.track().pads[s.track().last_pad % sampler_pads];
    auto c = colour_of(s);
    const int pad_key = std::clamp(60 + int(pad.pitch), 0, 127);
    facts_row(p, m, top + 3);
    float y = y1;
    if (zones_page && lit >= 0 && size_t(lit) < m.shapes.size()) { // the zone in hand: its file, keys, root, velocities
        const auto& z = m.shapes[size_t(lit)];
        y -= 15;
        // right to left: keys, ROOT, velocities (their glyph), round robin's turn (the loop), a keyswitch's (the keys)
        struct Part { Icon glyph; juce::String text; };
        std::vector<Part> parts;
        juce::String keys = juce::String(note_name(z.low_key));
        if (z.high_key != z.low_key) keys += dash + juce::String(note_name(z.high_key));
        parts.push_back({Icon::None, keys});
        parts.push_back({Icon::None, "ROOT " + juce::String(note_name(z.root))});
        if (z.low_velocity > 1 || z.high_velocity < 127) parts.push_back({Icon::Velocity, juce::String(z.low_velocity) + dash + juce::String(z.high_velocity)});
        if (z.round) parts.push_back({Icon::Loop, juce::String(z.round)});
        if (z.flags & ZoneShape::Switched) parts.push_back({Icon::Keyboard, {}});
        auto rs = caps_text(colour::sec);
        float rw = 0;
        for (const auto& part : parts) rw += (part.glyph != Icon::None ? 14 : 0) + p.width(part.text, rs) + 9;
        juce::String name = s.zone_file.empty() ? "Zone " + juce::String(lit + 1) : juce::String(s.zone_file);
        auto ns = p.fit(name, plain(colour::text, type::body, Weight::Medium), std::max(40.0f, pw - rw - 6), type::floor);
        float nw = std::min(p.width(name, ns), pw - rw - 6);
        p.text(name, px, y, nw + 1, 14, ns);
        float rx = px + pw - rw + 9;
        for (const auto& part : parts) {
            if (part.glyph != Icon::None) { icon(p, part.glyph, rx, y + 1, 12, colour::mid, 1.4f); rx += 14; }
            float w = p.width(part.text, rs);
            if (w > 0) p.text(part.text, rx, y + 1, w + 1, 13, rs);
            rx += w + 9;
        }
    }
    unsigned rows = problems(p, m, 0, 2, false);
    rows = std::min(rows, 2u);
    y -= 15 * float(rows);
    if (rows) problems(p, m, y, rows, true);
    float label_y = y - 13, keys_h = zones_page ? 18 : 20, keys_y = label_y - 1 - keys_h;
    float map_y = top + 21, map_h = keys_y - 3 - map_y;
    auto row = instrument_row(m, pad_key, px, pw);
    p.fill(px, map_y - 2, pw, map_h + 4, colour::panel, 3);
    zone_tiles(p, m, row, map_y, map_h, c, lit, zones_page, true, 0.34f);
    std::array<juce::Colour, 128> lit_keys {};
    std::array<bool, 128> on {};
    if (zones_page && lit >= 0 && size_t(lit) < m.shapes.size()) { // the zone's keys, its root full
        const auto& z = m.shapes[size_t(lit)];
        for (int k = z.low_key; k <= z.high_key; ++k) { lit_keys[size_t(k)] = c.withAlpha(0.55f); on[size_t(k)] = true; }
        lit_keys[z.root] = c; on[z.root] = true;
    } else { lit_keys[size_t(pad_key)] = c; on[size_t(pad_key)] = true; }
    keyboard(p, row, keys_y, keys_h, colour::dim.withAlpha(0.55f), lit_keys, on);
    c_labels(p, row, label_y, zones_page ? -1 : pad_key, c);
}

// The strip every drawing page keeps on top: the zones as one band over the keys, the key a hit plays lit.
// Returns where the page's own drawing may start.
float key_strip(Paint& p, const Snapshot& s, int key) {
    const auto& m = s.sample;
    auto c = colour_of(s);
    auto row = instrument_row(m, std::clamp(60 + int(s.track().pads[s.track().last_pad % sampler_pads].pitch), 0, 127), px, pw);
    int lit = zone_at(m.shapes, key, 100);
    zone_tiles(p, m, row, top + 2, 9, c, lit, false, false, 0.25f);
    std::array<juce::Colour, 128> lit_keys {};
    std::array<bool, 128> on {};
    if (key >= 0 && key < 128) { lit_keys[size_t(key)] = c; on[size_t(key)] = true; }
    keyboard(p, row, top + 13, 12, colour::dim.withAlpha(0.55f), lit_keys, on);
    return top + 32;
}

// TUNE: two octaves (more if Semi goes further) round the key a hit plays, large; where it was (C4) outlined when
// Semi moved it, the zone it lands in lit above, and the move named under the keys.
void tune_picture(Paint& p, const Snapshot& s, float y0) {
    const auto& m = s.sample;
    const auto& pad = s.track().pads[s.track().last_pad % sampler_pads];
    auto c = colour_of(s);
    const int key = std::clamp(60 + int(pad.pitch), 0, 127);
    int lo = std::min(60, key) / 12 * 12, hi = std::max(60, key);
    if (hi - lo < 12 && lo >= 12) lo -= 12;
    hi = std::max(lo + 24, (hi + 11) / 12 * 12);
    auto row = key_row(lo, std::min(hi, 127), px, pw);
    int lit = zone_at(m.shapes, key, 100);
    zone_tiles(p, m, row, y0, 10, c, lit, false, false, 0.3f);
    std::array<juce::Colour, 128> lit_keys {};
    std::array<bool, 128> on {};
    lit_keys[size_t(key)] = c; on[size_t(key)] = true;
    const float ky = y0 + 13, kh = bottom - 22 - ky;
    keyboard(p, row, ky, kh, colour::sec.withAlpha(0.85f), lit_keys, on, key != 60 ? 60 : -1, c);
    for (int n = row.lo; n <= row.hi; ++n) // the Cs on their keys
        if (n % 12 == 0 && n != key) p.text(juce::String(note_name(n)), row.x[size_t(n)] + 3, ky + kh - 15, row.w[size_t(n)] - 4, 13, caps_text(colour::bg));
    // C4 -> the key a hit plays now (cents after it when Fine moves it)
    juce::String to = juce::String(note_name(key));
    if (pad.fine) to += " " + juce::String(pad.fine > 0 ? "+" : "") + juce::String(int(pad.fine)) + " ct";
    auto a = plain(colour::mid, type::body, Weight::SemiBold), b = plain(c, type::body, Weight::SemiBold);
    float y = bottom - 17;
    if (key == 60 && !pad.fine) { p.text("C4", px, y, pw, 14, b, juce::Justification::centred); return; }
    float aw = p.width("C4", a), bw = p.width(to, b), total = aw + 22 + bw, x = px + (pw - total) / 2;
    p.text("C4", x, y, aw + 1, 14, a);
    icon(p, Icon::ArrowRight, x + aw + 5, y + 1, 12, colour::mid, 1.4f);
    p.text(to, x + aw + 22, y, bw + 1, 14, b);
}

} // namespace

void instrument_picture(Paint& p, const Snapshot& s, StageKind kind) {
    const auto& m = s.sample;
    const auto& t = s.track();
    const auto& pad = t.pads[t.last_pad % sampler_pads];
    auto c = colour_of(s);
    const int pad_key = std::clamp(60 + int(pad.pitch), 0, 127);
    if (kind == StageKind::Trim) { // ZONES
        int lit = m.shapes.empty() ? -1 : int(std::min<size_t>(s.ui.zone, m.shapes.size() - 1));
        map_picture(p, s, bottom - 1, lit, true);
        return;
    }
    const bool envelope = kind == StageKind::AmpEnv || kind == StageKind::FiltEnv;
    const bool filter = kind == StageKind::Filter || kind == StageKind::KitFilter;
    if (!envelope && !filter && kind != StageKind::Tune) { // the map, the zone a hit plays lit; Mix with its meters
        map_picture(p, s, bottom - 1, zone_at(m.shapes, pad_key, 100), false);
        return;
    }
    float y0 = key_strip(p, s, pad_key);
    if (kind == StageKind::Tune) { tune_picture(p, s, y0); return; }
    const float y = y0 + 2, h = bottom - 4 - y, x = px + 13, w = pw - 26;
    side_meters(p, s, pad, y, h, c);
    if (envelope) {
        const bool amp = kind == StageKind::AmpEnv;
        int a = amp ? pad.attack : pad.filter_attack, d = amp ? pad.decay : pad.filter_decay, rl = amp ? pad.release : pad.filter_release;
        float sustain = (amp ? pad.sustain : pad.filter_sustain) / 100.0f;
        const bool hold = amp && d >= 100; // held while the key is: no decay
        double at = time_seconds(a) * 0.3, decay = hold ? 0.0 : time_seconds(d) * (amp ? t.params[sampler::Decay] / 100.0 : 1.0), rel = time_seconds(rl) * 0.5;
        double held = std::max(0.25, 0.35 * (at + decay + rel));
        float soft = amp ? 1.0f - 0.6f * pad.velocity_sense / 100.0f : 1.0f;
        draw_envelope(p, x, y, w, h, at + decay + held + rel, at, decay, hold ? 1.0f : sustain, rel, false, s.ui.track, soft);
        return;
    }
    float hz, q;
    unsigned shape;
    bool on = true;
    if (kind == StageKind::Filter) {
        hz = 20.0f * std::exp2(pad.cutoff / 100.0f * 9.97f); q = 0.7f + pad.resonance * 0.06f;
        shape = pad.filter == PadFilter::Hpf2 || pad.filter == PadFilter::Hpf4 ? 2 : pad.filter == PadFilter::Bpf2 || pad.filter == PadFilter::Bpf4 ? 1 : 0;
        on = pad.filter != PadFilter::Off;
    } else { // the one filter after every pad
        hz = 20.0f * std::exp2(t.params[sampler::Filter] / 100.0f * 9.97f); q = 0.7f + t.params[sampler::Resonance] * 0.06f;
        shape = unsigned(t.params[sampler::FilterType]) % 3;
    }
    if (on) draw_filter_curve(p, x, y, w, h - 15, shape, 2, hz, q, knob_shade(s.ui.track, 1), knob_shade(s.ui.track, 0));
    else {
        p.fill(x, y + h / 2, w, 1, colour::dim);
        p.text("OFF", x, y + h / 2 - 18, w, 14, caps_text(colour::mid), juce::Justification::centred);
    }
}

} // namespace melodee
