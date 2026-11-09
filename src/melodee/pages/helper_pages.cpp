// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The helpers around playing:
// TEMPO (hold Tap Tempo): K1 tempo, K2 swing for the whole project, K3 click
//   level; B1 halves, B3 doubles, B2 the click on or off; the wheel moves the
//   tempo by 0.1. Tapping works from anywhere.
// ARP (Note Repeat): hold Note Repeat for a momentary arp, tap to keep it
//   on. K1 mode, K2 rate, K3 octaves; Minus / Plus the gate. B3 Hold latches.
//   On a sampler track it is note repeat: the held pad at the rate.
// CHORD (B1 there): one pad, one chord from the scale. K1 type, K2 voicing,
//   K3 inversion. B3 Hold latches the arp the same way. Chords land in Steps
//   and Roll as up to four notes.
// CAPTURE (Shift + Sample Record): what the pads just played on this track,
//   the last bars of it laid into a pattern (the first empty one). K1 bars,
//   K2 quantise, K3 which pattern; B3 keeps, B1 drops.
#include "melodee/engine/arp.h"
#include "melodee/model/chords.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

// ---------- Tempo ----------
void tempo_draw(Paint& p, const Snapshot& s) {
    Header h = header_of(s);
    h.right.push_back(label(s.project.name));
    frame(p, s, {{"HALF", "CLICK", "DOUBLE"}, -1, colour::text}, h);
    char text[16];
    std::snprintf(text, sizeof(text), "%d.%d", s.project.tempo / 10, s.project.tempo % 10);
    float x = zone::pad + 6, y = zone::picture_y + 10;
    float w = p.text(text, x, y, 160, 40, plain(colour::text, 34, Weight::SemiBold));
    p.text("BPM", x + w + 6, y + 14, 40, 20, plain(colour::mid, 12));
    // The beat in the bar.
    unsigned beat = s.playing ? (s.ticks / ticks_per_quarter) % 4 : 99;
    float bw = (zone::width - 2 * zone::pad - 12 - 3 * 5) / 4;
    for (unsigned b = 0; b < 4; ++b) p.fill(x + b * (bw + 5), y + 52, bw, 6, b == beat ? colour::text : colour::line, 3);
    p.text("TAPS", x, y + 78, 30, 12, caps_text(colour::mid, 7));
    if (s.project.click) {
        auto style = caps_text(colour::text, 7);
        float cw = p.width("CLICK ON", style) + 9;
        p.fill(zone::width - zone::pad - 6 - cw, y + 78, cw, 12, colour::surf, 3);
        p.text("CLICK ON", zone::width - zone::pad - 6 - cw, y + 78, cw, 12, style, juce::Justification::centred);
    }
}
void tempo_soft(Context& c, int key) {
    if (key == 0) c.set_tempo(c.project.tempo / 2);
    if (key == 2) c.set_tempo(c.project.tempo * 2);
    if (key == 1) c.set_click(!c.project.click);
}
void tempo_wheel(Context& c, int delta) { c.set_tempo(int(c.project.tempo) + delta); }

// ---------- Arp ----------
juce::String gate_text(unsigned gate) { return "GATE " + juce::String(gate) + " %"; }

void arp_draw(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    bool kit = t.engine == EngineKind::Sampler;
    Header h = header_of(s);
    if (t.arp) h.left.push_back(chip(kit ? "REPEAT ON" : "ARP ON", c));
    if (t.chord && !kit) h.left.push_back(chip("CHORD ON", c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{kit ? "" : "CHORD", kit ? "REPEAT" : "ARP", t.arp_latch ? "HOLD ON" : "HOLD"}, 1, c}, h);
    float x = zone::pad, y = zone::picture_y;
    float w = p.text(arp_mode_name(ArpMode(t.arp_mode)), x, y - 2, 120, 30, plain(colour::text, 28, Weight::SemiBold));
    juce::String detail = juce::String(arp_rate_name(t.arp_rate)) + juce::String::fromUTF8(" \xc2\xb7 ")
        + (kit ? juce::String("the held pad") : juce::String(t.arp_octaves) + (t.arp_octaves == 1 ? " octave" : " octaves"));
    p.text(detail, x + w + 8, y + 6, zone::width - x - w - 14, 20, plain(c, 12, Weight::Medium));
    float py = y + 31;
    float gw = p.width(gate_text(t.arp_gate), caps_text(colour::mid, 7)) + 9;
    p.fill(x, py, gw, 12, colour::surf, 3);
    p.text(gate_text(t.arp_gate), x, py, gw, 12, caps_text(colour::mid, 7), juce::Justification::centred);
    // What it would play: the keys held now, else the chord on pad 1, sixteen steps on.
    Arp a;
    a.mode = ArpMode(t.arp_mode); a.octaves = kit ? 1 : t.arp_octaves;
    for (auto k : s.held_keys) if (k >= 0) a.press(k, 100);
    if (!a.count) {
        std::array<int, notes_per_step> keys {};
        unsigned n = chord_notes(s.project, t, 0, keys);
        for (unsigned k = 0; k < n; ++k) a.press(keys[k], 100);
    }
    std::array<int, 16> seq {};
    int lo = 127, hi = 0;
    for (auto& k : seq) { int v; k = a.next(v); lo = std::min(lo, k); hi = std::max(hi, k); }
    float gx = x, gy = py + 18, gwid = zone::width - 2 * zone::pad, gh = zone::picture_y + 145 - gy;
    p.fill(gx, gy, gwid, gh, colour::panel, 3);
    float cw = gwid / 16, rows = float(std::max(hi - lo, 1));
    for (unsigned n = 0; n < 16; ++n) {
        float ny = gy + 3 + (1 - float(seq[n] - lo) / rows) * (gh - 10);
        p.fill(gx + n * cw + 1.5f, ny, cw * t.arp_gate / 100.0f - 1, 4, c, 2);
    }
}
void arp_soft(Context& c, int key) {
    auto& t = c.track();
    if (key == 0 && t.engine != EngineKind::Sampler) c.open(PageId::Chord);
    if (key == 2) { // Hold: the keys stay after the pads let go
        t.arp_latch = !t.arp_latch;
        if (!t.arp_latch) { c.send({Command::Type::Arp, c.ui.track, 0, 0, 0}); if (t.arp) c.send({Command::Type::Arp, c.ui.track, 0, 0, 1}); }
    }
}
void arp_nudge(Context& c, int way) {
    auto& t = c.track();
    t.arp_gate = uint8_t(std::clamp(int(t.arp_gate) + way * 10, 10, 100));
    c.send({Command::Type::Arp, c.ui.track, 0, 4, t.arp_gate});
}

// ---------- Chord ----------
void chord_draw(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    Header h = header_of(s);
    if (t.chord) h.left.push_back(chip("CHORD ON", c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"CHORD", "ARP", "HOLD"}, 0, c}, h);
    unsigned pad = s.ui.chord_pad >= 0 ? unsigned(s.ui.chord_pad) : 0;
    std::array<int, notes_per_step> keys {};
    unsigned n = chord_notes(s.project, t, pad, keys);
    float x = zone::pad, y = zone::picture_y;
    float w = p.text(chord_name(s.project, t, pad), x, y - 2, 150, 32, plain(colour::text, 28, Weight::SemiBold));
    juce::String notes;
    for (unsigned k = 0; k < n; ++k) notes += (k ? " " : "") + juce::String(note_name(keys[k]));
    p.text(notes, x + w + 8, y + 6, zone::width - x - w - 14, 20, plain(c, 12, Weight::Medium));
    // Pills: the pad, the degree in the key, how many notes.
    static const char* numerals[] {"I", "II", "III", "IV", "V", "VI", "VII"};
    std::array<uint8_t, 12> degrees {};
    unsigned count = scale_degrees(s.project.scale, degrees);
    juce::String where = juce::String(count == 7 ? numerals[pad % 7] : juce::String(pad % count + 1).toRawUTF8()) + " IN "
        + juce::String(note_letter(s.project.root)) + " " + juce::String(scales()[s.project.scale % scales().size()].name).toUpperCase();
    float px = x, py = y + 31;
    for (auto text : {juce::String("PAD ") + juce::String(pad + 1), where, juce::String(n) + " NOTES"}) {
        auto style = caps_text(colour::mid, 7);
        float pw = p.width(text, style) + 9;
        p.fill(px, py, pw, 12, colour::surf, 3);
        p.text(text, px, py, pw, 12, style, juce::Justification::centred);
        px += pw + 4;
    }
    // Two octaves of keys from the chord's octave, its notes lit.
    int from = keys[0] / 12 * 12;
    float ky = py + 22, kh = zone::picture_y + 145 - ky, ww = (zone::width - 2 * zone::pad) / 14;
    auto lit = [&](int k) { for (unsigned q = 0; q < n; ++q) if (keys[q] == k) return true; return false; };
    static const int whites[] {0, 2, 4, 5, 7, 9, 11};
    for (unsigned k = 0; k < 14; ++k) {
        int key = from + 12 * int(k / 7) + whites[k % 7];
        float kx = x + k * ww;
        p.fill(kx + 0.5f, ky, ww - 1, kh, lit(key) ? c : colour::sec, 2);
        if (whites[k % 7] == 0 || lit(key)) p.text(note_name(key), kx, ky + kh - 10, ww, 9, plain(colour::bg, 6, Weight::SemiBold), juce::Justification::centred);
    }
    for (unsigned k = 0; k < 14; ++k) {
        int white = whites[k % 7];
        if (white == 4 || white == 11) continue; // no black key after E or B
        int key = from + 12 * int(k / 7) + white + 1;
        float kx = x + (k + 1) * ww - ww * 0.3f;
        p.fill(kx, ky, ww * 0.6f, kh * 0.55f, lit(key) ? c : colour::bg, 2);
    }
}
void chord_soft(Context& c, int key) {
    auto& t = c.track();
    if (key == 0) t.chord = !t.chord;    // B1 on the Chord page: chords on or off
    if (key == 1) c.open(PageId::Arp);
    if (key == 2) arp_soft(c, 2);
}

// ---------- Capture ----------
void capture_draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    Header h = header_of(s);
    h.left.push_back(chip("CAPTURE", c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"DROP", "", "KEEP"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}, h);
    unsigned bars = s.ui.capture_bars;
    float x = zone::pad, y = zone::picture_y;
    p.text("Keep the last " + juce::String(bars) + (bars == 1 ? " bar" : " bars"), x, y, 300, 22, plain(colour::text, type::title, Weight::SemiBold));
    bool empty = true;
    // The picture: four bars of what the pads played, the kept ones bright.
    double step_ms = 60000.0 / (s.project.tempo / 10.0) / 4, shown = step_ms * 64, start = double(s.now) - shown;
    float gx = x, gy = y + 36, gw = zone::width - 2 * zone::pad, gh = 68;
    p.fill(gx, gy, gw, gh, colour::panel, 3);
    for (unsigned b = 1; b < 4; ++b) p.fill(gx + gw * b / 4, gy, 0.5f, gh, colour::line);
    int lo = 127, hi = 0;
    unsigned count = std::min<unsigned>(s.history_at, unsigned(s.history.size()));
    for (unsigned n = 0; n < count; ++n) {
        const auto& e = s.history[n];
        if (e.track != s.ui.track || e.at < start) continue;
        lo = std::min<int>(lo, e.key); hi = std::max<int>(hi, e.key);
    }
    float kept_from = gx + gw * (1 - bars / 4.0f);
    for (unsigned n = 0; n < count; ++n) {
        const auto& e = s.history[n];
        if (e.track != s.ui.track || e.at < start) continue;
        empty = false;
        float nx = gx + float((e.at - start) / shown) * gw, nw = std::max(2.0f, float((e.length ? e.length : step_ms) / shown) * gw);
        float ny = gy + 3 + (hi > lo ? 1 - float(e.key - lo) / float(hi - lo) : 0.5f) * (gh - 9);
        p.fill(nx, ny, nw, 3, nx >= kept_from ? c : c.withAlpha(0.35f), 1.5f);
    }
    unsigned track_number = s.ui.track + 1;
    p.text("into pattern P" + juce::String(s.ui.capture_into + 1) + " of track " + juce::String(track_number)
               + (s.sketches[s.ui.track][s.ui.capture_into].notes ? ", replacing what is there" : ", empty until now"),
           x, y + 21, 300, 12, plain(colour::sec, 10));
    p.text("HEARD EARLIER", gx, gy + gh + 6, 120, 9, caps_text(colour::dim, 7));
    p.fill(kept_from, gy + gh + 3, gx + gw - kept_from, 2, colour::text, 1);
    p.text("KEPT", kept_from, gy + gh + 6, 60, 9, caps_text(colour::text, 7));
    if (empty) p.text("Play something on the pads first", gx, gy, gw, gh, plain(colour::dim, 11, Weight::Medium), juce::Justification::centred);
}
void capture_soft(Context& c, int key) {
    if (key == 0) c.back();
    if (key == 2) { c.capture(); c.open(PageId::Steps); }
}
} // namespace

const PageSpec tempo_page {tempo_draw, play_pad, tempo_wheel, no_push, tempo_soft, [](Context&, int) {}};
const PageSpec arp_page {arp_draw, play_pad, no_wheel, no_push, arp_soft, arp_nudge};
const PageSpec chord_page {chord_draw, play_pad, no_wheel, no_push, chord_soft, octave};
const PageSpec capture_page {capture_draw, play_pad, no_wheel, [](Context& c) { c.capture(); c.open(PageId::Steps); }, capture_soft, [](Context&, int) {}};

} // namespace melodee
