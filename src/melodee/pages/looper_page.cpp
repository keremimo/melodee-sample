// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// LOOPER (LOOPER on Tracks; Loop and 16 Levels pressed together; 16 Levels of type Loop): the four loops
// (docs/LOOPER.md). B1 LOOPS (round 2, looper B): four rings, each loop's
// waveform bent round, a hand sweeping where it plays, its state and length
// under it; K1 the loop, K2 its length,
// K3 its level (Shift: feedback, Quantize, After take). B2 MIX: the loop
// tracks L1-L4 as strips; K1 level, K2 pan, K3 send (Shift: the bus). B3
// MORE: the loop's sheet (shell/loop_sheet.h). Push records, closes, overdubs
// and plays the loop selected; the wheel and -/+ pick it; the pads are the
// loop grid (shell/looper_jobs.cpp). With the Looper off the page asks first.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/ui/widgets.h"
#include "melodee/engine/engine_host.h"
#include <cmath>

namespace melodee {
namespace {
using namespace ui;
using S = Looper::State;

// The state as a word and its colour; a change waiting blinks.
void state_word(const Snapshot& s, const Snapshot::LoopShown& l, unsigned loop, juce::String& word, juce::Colour& c) {
    const auto& v = l.view;
    bool blink = (s.now / 250) % 2;
    word = looper::state_word(v.state);
    c = loop_colour(loop);
    switch (v.state) {
    case S::Empty: word = ""; c = colour::dim; break; // an empty loop says nothing
    case S::Armed: c = blink ? colour::rec : colour::dim; break;
    case S::Recording: c = colour::rec; break;
    case S::Overdub: c = colour::amber; break;
    case S::Stopped: c = colour::mid; break;
    case S::Playing: break;
    }
    if (v.fitting) { word = "FIT"; c = blink ? colour::amber : colour::dim; }
    else if (v.pending == Looper::Pending::Play || v.pending == Looper::Pending::Stop || v.pending == Looper::Pending::Close)
        c = blink ? c : colour::dim;
}

// Its length: the bars it spans (Free while a first take runs), or beats where it is no whole bar.
juce::String length_text(const Snapshot& s, const Looper::View& v) {
    if (v.state == S::Recording && !v.bars) return "Free";
    if (v.ticks > 0) {
        double bars = v.ticks / std::max(1u, s.project.bar_ticks());
        if (std::fabs(bars - std::round(bars)) > 0.01) return juce::String(v.ticks / s.project.beat_ticks(), 1) + " beats";
        return loop_length_name(unsigned(std::lround(bars)));
    }
    return loop_length_name(v.bars);
}

// Round 2 (looper B, "four rings"): a loop drawn as a loop, its waveform bent round a ring from twelve o'clock,
// a hand sweeping where it plays (in sync, the hands point the same way). Red while recording (only what is
// recorded so far), amber overdubbing, its colour playing, grey stopped, a broken amber ring while armed. Its tag
// in the middle on `ground`, whatever lies behind it.
void draw_ring(Paint& p, const Snapshot& s, unsigned n, float cx, float cy, juce::Colour ground, bool selected) {
    constexpr float r = 28, two_pi = 6.2831853f;
    constexpr unsigned ticks = 72;
    const auto& l = s.loops[n];
    const auto& v = l.view;
    bool lit = v.state == S::Playing || v.state == S::Overdub || v.state == S::Recording;
    auto c = v.state == S::Recording ? colour::rec : v.state == S::Overdub ? colour::amber
           : v.state == S::Stopped ? colour::mid.withAlpha(0.6f) : loop_colour(n);
    // The ring, broken while armed.
    juce::Path ring;
    ring.addCentredArc(cx, cy, r, r, 0, 0, two_pi, true);
    if (v.state == S::Armed) {
        juce::Path dashed;
        const float dashes[] {3, 4};
        juce::PathStrokeType(1.5f).createDashedStroke(dashed, ring, dashes, 2);
        p.g.setColour((s.now / 250) % 2 ? colour::amber : colour::dim);
        p.g.fillPath(dashed);
    } else {
        p.g.setColour(colour::line);
        p.g.strokePath(ring, juce::PathStrokeType(1.5f));
    }
    // The waveform, a tick a step round from twelve o'clock, as tall as its loudest column.
    if (v.state != S::Empty && v.state != S::Armed && v.span) {
        float shown = v.state == S::Recording ? std::min(1.0f, float(v.position) / float(std::max<uint32_t>(v.span, 1))) : 1.0f;
        juce::Path wave;
        for (unsigned k = 0; k < ticks; ++k) {
            float at = float(k) / ticks;
            if (at > shown) break;
            unsigned c0 = k * looper::peak_columns / ticks, c1 = std::max(c0 + 1, (k + 1) * looper::peak_columns / ticks);
            uint8_t peak = 0;
            for (unsigned q = c0; q < c1 && q < looper::peak_columns; ++q) peak = std::max(peak, l.peaks[q]);
            float len = 2 + peak / 255.0f * 14, a = at * two_pi - two_pi / 4;
            wave.startNewSubPath(cx + (r - len / 2) * std::cos(a), cy + (r - len / 2) * std::sin(a));
            wave.lineTo(cx + (r + len / 2) * std::cos(a), cy + (r + len / 2) * std::sin(a));
        }
        p.g.setColour(c.withMultipliedAlpha(lit && !selected ? 0.85f : 1.0f));
        p.g.strokePath(wave, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    // The hand: where it plays (a take: how far it has come).
    if (lit && v.length + v.span) {
        uint32_t whole = v.state == S::Recording ? std::max<uint32_t>(v.span, 1) : std::max<uint32_t>(v.length, 1);
        float a = float(v.position % whole) / float(whole) * two_pi - two_pi / 4;
        juce::Path hand;
        hand.startNewSubPath(cx, cy);
        hand.lineTo(cx + 37 * std::cos(a), cy + 37 * std::sin(a));
        p.g.setColour(colour::text);
        p.g.strokePath(hand, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    p.circle(cx, cy, 13, ground);
    bool quiet = v.state == S::Empty || v.state == S::Stopped;
    p.text("L" + juce::String(n + 1), cx - 13, cy - 8, 26, 16, plain(quiet ? colour::dim : loop_colour(n), type::body, Weight::Bold),
           juce::Justification::centred);
}

// Under a ring from `y`: its state as a word in its colour, and its length (amber at another tempo, with the tempo).
void draw_ring_words(Paint& p, const Snapshot& s, unsigned n, float cx, float y, bool selected) {
    const auto& v = s.loops[n].view;
    juce::String word; juce::Colour wc;
    state_word(s, s.loops[n], n, word, wc);
    if (word.isNotEmpty()) p.text(word, cx - 37, y, 74, 13, caps_text(wc), juce::Justification::centred);
    if (v.state != S::Empty) {
        juce::String below = v.stretched ? juce::String(v.tempo % 10 ? juce::String(v.tempo / 10.0, 1) : juce::String(v.tempo / 10)) + " BPM"
                                         : length_text(s, v);
        auto style = plain(v.stretched ? colour::amber : selected ? colour::sec : colour::mid, type::small);
        p.text(below, cx - 37, y + 15, 74, 13, p.fit(below, style, 70, type::floor), juce::Justification::centred);
    }
}

void draw_loops(Paint& p, const Snapshot& s) {
    constexpr float cy = zone::picture_y + 60;
    for (unsigned n = 0; n < loop_tracks; ++n) {
        float cx = zone::pad + 40 + n * 76;
        bool selected = n == s.ui.loop % loop_tracks;
        if (selected) p.fill(cx - 37, zone::picture_y + 4, 74, zone::picture_h - 8, colour::raised, 4);
        draw_ring(p, s, n, cx, cy, selected ? colour::raised : colour::bg, selected);
        draw_ring_words(p, s, n, cx, zone::picture_y + 106, selected);
        if (s.project.loop_mix[n].mute) p.fill(cx - 37, zone::picture_y + 4, 74, zone::picture_h - 8, colour::bg.withAlpha(0.5f), 4);
    }
}

void draw_mix(Paint& p, const Snapshot& s) {
    constexpr float gap = 6, top = zone::picture_y + 2, sh = zone::picture_h - 4;
    constexpr float w = (zone::width - 2 * zone::pad - 3 * gap) / 4;
    for (unsigned n = 0; n < loop_tracks; ++n) {
        const auto& m = s.project.loop_mix[n];
        float x = zone::pad + n * (w + gap), cx = x + w / 2;
        bool selected = n == s.ui.loop % loop_tracks, sound = s.loops[n].view.state != S::Empty;
        auto c = loop_colour(n);
        p.fill(x, top, w, sh, selected ? colour::raised : colour::quiet, 4);
        if (selected) p.fill(cx - 15, top + 4, 30, 18, c, 3);
        p.text("L" + juce::String(n + 1), x, top + 4, w, 18, plain(selected ? colour::bg : c, type::list, Weight::Bold), juce::Justification::centred);
        float fy = top + 28, fh = 70, fx = cx - 6;
        p.fill(fx, fy, 6, fh, colour::line, 3);
        float level = m.level / 127.0f;
        if (level > 0) p.fill(fx, fy + fh * (1 - level), 6, fh * level, c, 3);
        p.fill(fx + 9, fy, 2, fh, colour::line, 1);
        float meter = s.loops[n].meter;
        float shown = meter > 0 ? std::clamp((20 * std::log10(meter) + 48) / 48, 0.0f, 1.0f) : 0;
        if (shown > 0 && !m.mute) p.fill(fx + 9, fy + fh * (1 - shown), 2, fh * shown, colour::sec, 1);
        float py = fy + fh + 8;
        p.fill(cx - 15, py, 30, 2, colour::line, 1);
        p.fill(cx - 15 + 30 * (m.pan + 50) / 100.0f - 1, py - 2, 2, 6, colour::sec, 1);
        juce::String send = juce::String("SEND ") + char('A' + m.bus % bus_count) + " " + juce::String(m.send);
        p.text(send, x, py + 6, w, 13, p.fit(send, caps_text(m.send ? colour::sec : colour::dim), w - 4, type::floor), juce::Justification::centred);
        juce::String word = m.mute ? "MUTE" : m.solo ? "SOLO" : !sound ? "EMPTY" : "";
        auto wc = m.mute ? colour::rec : m.solo ? colour::text : colour::dim;
        if (word.isNotEmpty()) p.text(word, x, top + sh - 16, w, 13, caps_text(wc), juce::Justification::centred);
        if (m.mute) p.fill(x, top, w, sh, colour::bg.withAlpha(0.5f), 4);
    }
}

// The loop's sheet (B3 MORE, shell/loop_sheet.h): what each of B1-B3 does for the loop selected, Shift held the
// second row. `key` is the soft key's label, `word` and `sub` its panel's; `can`: it does something now.
struct SheetKey {
    Icon icon = Icon::None;
    juce::String key, word, sub;
    bool can = false, danger = false;
};

std::array<SheetKey, 3> sheet_keys(const Snapshot& s) {
    unsigned loop = s.ui.loop % loop_tracks;
    const auto& v = s.loops[loop].view;
    const auto& t = s.track();
    int pad = commit_pad(t);
    auto can = [&](LoopJob job) { return loop_job_can(job, v, pad >= 0); };
    bool going = loop_going(v);
    auto key = [](Icon i, juce::String k, juce::String word, juce::String sub, bool able) {
        SheetKey out; out.icon = i; out.key = std::move(k); out.word = std::move(word); out.sub = std::move(sub); out.can = able; return out;
    };
    std::array<SheetKey, 3> keys;
    if (s.ui.shift) { // the second row: CLEAR (asks), + NOTE, and the stop or play at once
        keys[0] = key(Icon::Trash, "CLEAR", "CLEAR", "L" + juce::String(loop + 1), can(LoopJob::Clear));
        keys[0].danger = true;
        keys[1] = key(Icon::Note, "+ NOTE", "+ NOTE", "AT BAR 1", can(LoopJob::CommitNote));
        keys[2] = key(going ? Icon::Stop : Icon::Play, going ? "STOP NOW" : "PLAY NOW", going ? "STOP" : "PLAY", "AT ONCE", can(LoopJob::Now));
        return keys;
    }
    char label[4] {};
    if (pad >= 0) pad_label(unsigned(pad), label);
    keys[0] = key(Icon::Back, "BACK", {}, {}, true);
    keys[1] = key(Icon::Pads, "COMMIT", "COMMIT", t.engine != EngineKind::Sampler ? juce::String("PICK A KIT") : pad < 0 ? juce::String("KIT FULL")
                                                                                     : "TO " + juce::String(label), can(LoopJob::Commit));
    auto q = looper::Quantize(s.project.looper.quantize);
    juce::String when = q == looper::Quantize::Bar ? "NEXT BAR" : q == looper::Quantize::Beat ? "NEXT BEAT" : "AT ONCE";
    bool queued = v.pending == Looper::Pending::Play || v.pending == Looper::Pending::Stop; // pressed again, it cancels
    if (queued) keys[2] = key(Icon::Cross, "CANCEL", "CANCEL", v.pending == Looper::Pending::Stop ? "THE STOP" : "THE PLAY", can(LoopJob::Toggle));
    else keys[2] = key(going ? Icon::Stop : Icon::Play, going ? "STOP" : "PLAY", going ? "STOP" : "PLAY", when, can(LoopJob::Toggle));
    return keys;
}

// One screen, three panels as the keys sit over them: under B1 the loop itself, alive (its ring, state and length);
// under B2 COMMIT and the pad it lands on; under B3 PLAY or STOP and when. Shift held, the second row: CLEAR,
// + NOTE, STOP or PLAY at once. A job that would do nothing is dim. The pots are the cards: the loop's source,
// length and level (Shift: feedback, Quantize, After take); the wheel picks the loop.
void draw_sheet(Paint& p, const Snapshot& s, const std::array<SheetKey, 3>& keys) {
    unsigned loop = s.ui.loop % loop_tracks;
    std::array<KeyTile, 3> tiles;
    for (size_t n = s.ui.shift ? 0 : 1; n < tiles.size(); ++n) {
        const auto& k = keys[n];
        tiles[n] = word_sub_tile(k.icon, k.word, k.sub, n == 2);
        tiles[n].colour = loop_colour(loop);
        tiles[n].off = !k.can;
        tiles[n].danger = k.danger;
    }
    key_tiles(p, tiles);
    if (s.ui.shift) return;
    constexpr float x = 6, y = zone::picture_y + 12, w = zone::width / 3.0f - 12, h = 116; // as key_tiles lays its panels
    float cx = x + w / 2;
    p.fill(x, y, w, h, colour::panel, 6);
    draw_ring(p, s, loop, cx, y + 46, colour::panel, true);
    draw_ring_words(p, s, loop, cx, y + 86, true);
    if (s.project.loop_mix[loop].mute) p.fill(x, y, w, h, colour::bg.withAlpha(0.5f), 6);
}

void draw(Paint& p, const Snapshot& s) {
    unsigned loop = s.ui.loop % loop_tracks;
    const auto& v = s.loops[loop].view;
    Header h = header_of(s);
    bool on = s.project.looper.on;
    if (on) {
        h.left.push_back(pill(Icon::Magnet, juce::String(looper::quantize_name(s.project.looper.quantize)).toUpperCase()));
        h.right.push_back(chip("L" + juce::String(loop + 1), loop_colour(loop)));
        if (v.layers) h.right.push_back(label(juce::String(v.layers) + (v.layers == 1 ? " LAYER" : " LAYERS")));
    }
    bool making = on && (!s.looper_on || !v.ready); // the memory is being made
    if (!on || making) h.left.push_back(pill(Icon::Loop, making ? "LOADING" : "LOOPER OFF", making ? colour::amber : colour::mid));
    SoftKeys keys {{"LOOPS", "MIX", "MORE"}, int(s.ui.looper_view % 2), loop_colour(loop)};
    if (!on) { keys = SoftKeys {{"", "", "TURN ON"}}; keys.glyphs[2] = Icon::Power; }
    if (making) keys = SoftKeys {{"", "", ""}};
    bool sheet = on && !making && s.ui.loop_sheet;
    std::array<SheetKey, 3> sheet_row;
    if (sheet) { // the sheet's keys: BACK, COMMIT, PLAY or STOP (Shift: CLEAR, + NOTE, at once); dim where they do nothing
        sheet_row = sheet_keys(s);
        keys = SoftKeys {{sheet_row[0].key, sheet_row[1].key, sheet_row[2].key}, !s.ui.shift && sheet_row[2].can ? 2 : -1, loop_colour(loop),
                         {s.ui.shift ? KeyIcon::None : KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
        for (size_t n = 0; n < 3; ++n) {
            if (n || s.ui.shift) keys.glyphs[n] = sheet_row[n].icon;
            if (!sheet_row[n].can) keys.tints[n] = colour::dim;
            else if (sheet_row[n].danger) keys.tints[n] = colour::rec;
        }
    }
    frame(p, s, keys, h);
    if (!on || making) { // L1-L4 as ghost lanes in their colours, the power glyph of B3 over them
        for (unsigned l = 0; l < loop_tracks; ++l) {
            float y = zone::picture_y + 8 + l * 33;
            p.text("L" + juce::String(l + 1), zone::pad + 4, y + 6, 30, 20, plain(loop_colour(l).withAlpha(0.5f), type::list, Weight::SemiBold));
            for (float x = 40; x < zone::width - zone::pad; x += 7) p.fill(x, y + 15, 2, 1.5f, loop_colour(l).withAlpha(0.35f));
        }
        float cx = 177, cy = zone::picture_y + 72;
        p.circle(cx, cy, 25, colour::bg);
        p.g.setColour(making ? colour::amber : colour::mid);
        p.g.drawEllipse(cx - 25, cy - 25, 50, 50, 1.5f);
        icon(p, making ? Icon::Loop : Icon::Power, cx - 12, cy - 12, 24, making ? colour::amber : colour::sec, 1.6f);
        p.text(juce::String(loop_tracks) + juce::String::fromUTF8(" \xc3\x97 45 MB"), zone::pad, zone::picture_y + 128, zone::width - 2 * zone::pad, 14,
               caps_text(colour::dim), juce::Justification::centredRight);
        return;
    }
    if (sheet) draw_sheet(p, s, sheet_row);
    else if (s.ui.looper_view % 2 == 1) draw_mix(p, s);
    else draw_loops(p, s);
}

void pad(Context& c, unsigned pad, uint8_t, bool down) { loop_grid_pad(c, pad, down); }

// The wheel and -/+: the loop (the sheet follows it).
void pick(Context& c, int delta) { c.ui.loop = uint8_t((int(c.ui.loop) + delta + int(loop_tracks)) % int(loop_tracks)); }

// Push, sheet or not: record, close, overdub, play the loop selected.
void push(Context& c) {
    if (!c.project.looper.on) { set_looper(c, true); return; }
    looper_do(c, looper::Do::Primary, c.ui.loop);
}

void soft(Context& c, int key) {
    auto& ui = c.ui;
    if (!c.project.looper.on) { if (key == 2) set_looper(c, true); return; }
    if (ui.loop_sheet) { loop_sheet_key(c, unsigned(key)); return; } // BACK, COMMIT, PLAY / STOP; Shift: CLEAR, + NOTE, at once
    if (key == 2) { ui.loop_sheet = true; return; }
    ui.looper_view = uint8_t(key);
}
} // namespace

const PageSpec looper_page {draw, pad, pick, push, soft, pick}; // Minus / Plus: the loop, as the wheel

} // namespace melodee
