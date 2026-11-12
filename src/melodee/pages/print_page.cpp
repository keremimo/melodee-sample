// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PRINT (freeze plan F2c, shell/print_sheet.cpp): Sample Record on a plug-in
// or MIDI track, B3 PRINT on a kit's Record. The pads are the track's
// patterns of the bank in view, laid out as the pads sit: lit ones get
// printed (a snowflake in the corner), a tap leaves one out, B2 ALL. K1
// TAP (Post FX, Pre FX), K2 TAIL (Loop, Ring, Cut); K3 stays empty. Beside
// the pads: how many patterns, what the prints take on the card, and what
// freezing saves of a block. B3 FREEZE prints them: then each pad shows its
// print's progress, the column the speed and the time left; B1 calls it off
// and nothing changes. Minus / Plus: the bank in view. A MIDI track's prints
// need its audio back in real time: not yet.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <cmath>

namespace melodee {
using namespace ui;

namespace {
constexpr float cell = 31, gap = 3, grid = 4 * cell + 3 * gap;

// A label in capitals over a value with a unit beside it.
void block(Paint& p, float x, float y, float w, const juce::String& caps, const juce::String& value, const juce::String& unit,
           juce::Colour value_colour) {
    p.text(caps, x, y, w, 12, caps_text(colour::mid));
    float vw = p.text(value, x, y + 13, w, 24, plain(value_colour, type::value, Weight::SemiBold));
    if (unit.isNotEmpty()) p.text(unit, x + vw + 5, y + 21, w - vw - 5, 14, p.fit(unit, plain(colour::sec, type::body), w - vw - 5, type::floor));
}

juce::String clock(float seconds) {
    int s = int(std::lround(std::max(0.0f, seconds)));
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

// A pattern's print on the card, bytes: a pass at the device's rate in 32-bit float stereo; a Ring tail counted a second.
double print_bytes(const Snapshot& s, unsigned t, unsigned p, PrintTail tail) {
    double ticks = s.sketches[t][p].length, bpm = std::max(1u, s.tempo) / 10.0;
    double seconds = ticks / ticks_per_quarter * 60.0 / bpm + (tail == PrintTail::Ring ? 1.0 : 0.0);
    return seconds * s.audio_rate * 8.0;
}

void draw(Paint& p, const Snapshot& s) {
    const auto& sheet = s.ui.print_sheet;
    const unsigned t = sheet.track % track_count;
    const auto& track = s.project.tracks[t];
    const auto c = track_colour(t);
    const bool midi = track.engine == EngineKind::Midi, printing = sheet.printing;
    const auto& job = s.print;
    const bool ours = job.busy && job.track == t && job.freeze; // the print engine works on this sheet's track
    unsigned picked = 0, notes = 0;
    for (unsigned n = 0; n < pattern_count; ++n) { notes += s.sketches[t][n].notes; picked += s.sketches[t][n].notes && sheet.picked(n); }

    Header h = header_of(s);
    if (printing) h.left.push_back(pill(Icon::Snow, ours ? "PRINTING " + juce::String(job.through + (job.pattern >= 0)) + "/" + juce::String(job.count)
                                                        : juce::String("PRINTING"), colour::ice));
    else h.left.push_back(chip(Icon::Snow, "PRINT", colour::ice));
    h.left.push_back(bank_pill(s));
    h.right.push_back(tag_of(s, t));
    SoftKeys keys {{"CANCEL", printing ? "" : "ALL", printing ? "" : "FREEZE"}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    if (!printing) {
        keys.glyphs[2] = Icon::Snow;
        keys.tints[2] = midi || !picked ? colour::dim : colour::ice;
        if (s.ui.shift) { keys.labels = {"", "", "DEEP FREEZE"}; keys.icons = {}; keys.tints[2] = colour::dim; } // F2d: not yet
    }
    frame(p, s, keys, h);

    // The patterns as the pads sit: pads 13-16 along the top, 1-4 along the bottom.
    float gx = zone::pad, gy = zone::picture_y + (zone::picture_h - grid) / 2;
    unsigned live = s.playing ? s.playing_patterns[t] % pattern_count : track.pattern % pattern_count;
    for (unsigned k = 0; k < pattern_pads; ++k) {
        unsigned n = s.ui.pad_pattern(k), row = 3 - k / 4, column = k % 4;
        float x = gx + column * (cell + gap), y = gy + row * (cell + gap);
        bool has = s.sketches[t][n].notes, lit = has && sheet.picked(n);
        juce::Colour text = colour::dim;
        bool snow = false;
        if (printing && lit && ours && job.is_done(n)) { p.fill(x, y, cell, cell, c.withAlpha(0.8f), 5); text = colour::bg; snow = true; }
        else if (printing && lit && ours && job.is_failed(n)) { p.fill(x, y, cell, cell, colour::rec.withAlpha(0.22f), 5); p.ring(x, y, cell, cell, colour::rec, 5, 1.5f); text = colour::rec; }
        else if (printing && lit && ours && job.pattern == int(n)) { // its progress, a ring filling
            p.fill(x, y, cell, cell, c.withAlpha(0.18f), 5);
            float cx = x + cell / 2, cy = y + cell / 2, r = cell / 2 - 3.5f;
            juce::Path back, done;
            back.addCentredArc(cx, cy, r, r, 0, 0, juce::MathConstants<float>::twoPi, true);
            p.g.setColour(colour::line);
            p.g.strokePath(back, juce::PathStrokeType(2));
            done.addCentredArc(cx, cy, r, r, 0, 0, juce::MathConstants<float>::twoPi * std::clamp(job.fraction, 0.02f, 1.0f), true);
            p.g.setColour(colour::ice);
            p.g.strokePath(done, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            text = colour::text;
        } else if (lit && !printing) { p.fill(x, y, cell, cell, c, 5); text = colour::bg; snow = true; }
        else if (printing && lit) { p.fill(x, y, cell, cell, c.withAlpha(0.35f), 5); text = colour::text; } // waits its turn
        else if (has && !printing) { p.fill(x, y, cell, cell, c.withAlpha(0.22f), 5); text = c; }
        else p.fill(x, y, cell, cell, colour::quiet, 5);
        if (!printing && lit && n == live) p.ring(x, y, cell, cell, colour::text, 5, 1.5f); // the one playing
        p.text(juce::String(n + 1), x, y, cell, cell, plain(text, type::body, Weight::SemiBold), juce::Justification::centred);
        if (snow) icon(p, Icon::Snow, x + cell - 11.5f, y + 2.5f, 9, colour::bg, 1.4f);
    }

    // Beside them: what it costs and saves, or how the prints go.
    float x = gx + grid + 14, w = zone::width - zone::pad - x, y = zone::picture_y + 6;
    if (printing) {
        if (ours && job.times_real > 0) {
            block(p, x, y, w, "SPEED", juce::String::fromUTF8("\xc3\x97") + juce::String(job.times_real, 1), "real time", colour::ice);
            block(p, x, y + 44, w, "LEFT", clock(job.seconds_left), "", colour::text);
        } else {
            block(p, x, y, w, "SPEED", juce::String::fromUTF8("\xe2\x80\x94"), "", colour::dim);
            block(p, x, y + 44, w, "LEFT", juce::String::fromUTF8("\xe2\x80\x94"), "", colour::dim);
        }
        juce::String now = ours && job.pattern >= 0 ? "P" + juce::String(job.pattern + 1) : juce::String(ours ? "MAPPING" : "WAITING");
        p.text(now, x, y + 92, w, 12, caps_text(colour::mid));
        float by = y + 108, bw = w;
        p.fill(x, by, bw, 6, colour::quiet, 3);
        float done = ours && job.count ? (float(job.through) + (job.pattern >= 0 ? job.fraction : 0)) / float(job.count) : 0;
        if (done > 0) p.fill(x, by, bw * std::clamp(done, 0.0f, 1.0f), 6, colour::ice, 3);
    } else {
        block(p, x, y, w, "PATTERNS", juce::String(picked), "of " + juce::String(notes), colour::text);
        if (midi) {
            block(p, x, y + 44, w, "MIDI TRACKS", "Later", "", colour::amber);
        } else {
            double bytes = 0;
            for (unsigned n = 0; n < pattern_count; ++n) if (s.sketches[t][n].notes && sheet.picked(n)) bytes += print_bytes(s, t, n, sheet.tail);
            double mb = bytes / (1024.0 * 1024.0);
            block(p, x, y + 44, w, "CARD", mb >= 10 ? juce::String(int(std::lround(mb))) : juce::String(mb, 1), "MB", colour::text);
            float load = s.machine.track_load[t];
            bool frozen = s.freeze[t].frozen();
            float saved = frozen ? (s.live_load[t] > 0 ? std::max(0.0f, s.live_load[t] - load) : -1) : load;
            block(p, x, y + 88, w, "CPU", saved < 0 ? juce::String::fromUTF8("\xe2\x80\x94")
                                                    : juce::String::fromUTF8("\xe2\x88\x92") + juce::String(int(std::lround(saved * 100))),
                  saved < 0 ? juce::String() : juce::String("% of a block"), colour::ice);
        }
    }
    knob_strip(p, cards_of(s)); // K1 TAP, K2 TAIL (shell/bindings.cpp)
}

void pad(Context& c, unsigned pad, uint8_t, bool down) { if (down) c.print_sheet_pad(pad); }

void soft(Context& c, int key) {
    if (c.ui.shift) { if (key == 2 && !c.ui.print_sheet.printing) c.notice("Deep freeze: later"); return; } // F2d
    if (key == 0) c.print_sheet_cancel();
    if (key == 1) c.print_sheet_all();
    if (key == 2) c.print_sheet_go();
}

void push(Context& c) { c.print_sheet_go(); }

// Minus / Plus: the bank of patterns in view.
void nudge(Context& c, int way) {
    if (c.ui.print_sheet.printing) return;
    int bank = std::clamp(int(c.ui.pattern_bank) + way, 0, int(pattern_banks) - 1);
    c.ui.pattern_bank = uint8_t(bank);
    c.notice(std::string("Bank ") + char('A' + bank));
}
} // namespace

const PageSpec print_page {draw, pad, no_wheel, push, soft, nudge};

} // namespace melodee
