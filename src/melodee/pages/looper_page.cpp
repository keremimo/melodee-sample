// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// LOOPER (Loop tapped twice; 16 Levels of type Loop): the four loops
// (docs/LOOPER.md). B1 LOOPS: a row each, its tag, its state as a word, its
// waveform and playhead, its length and source; K1 the loop, K2 its length,
// K3 its level (Shift: feedback, Quantize, After take). B2 MIX: the loop
// tracks L1-L4 as strips; K1 level, K2 pan, K3 send (Shift: the bus). B3
// MORE: the loop's sheet. Push records, closes, overdubs and plays the loop
// selected; the wheel and -/+ pick it; the pads are the loop grid
// (shell/looper_jobs.cpp). With the Looper off the page asks first.
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

juce::String boundary_word(const Snapshot& s) {
    auto q = looper::Quantize(s.project.looper.quantize);
    return q == looper::Quantize::Bar ? "next bar" : q == looper::Quantize::Beat ? "next beat" : "now";
}

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

// One loop's waveform, as the boards draw a sample: bars from the centre, the part played lit, and the playhead.
void wave(Paint& p, const Snapshot::LoopShown& l, unsigned loop, float x, float y, float w, float h, bool selected) {
    const auto& v = l.view;
    float cy = y + h / 2;
    p.fill(x, cy, w, 1, colour::line);
    if (v.state == S::Empty || v.state == S::Armed || !v.span) return;
    bool lit = v.state == S::Playing || v.state == S::Overdub || v.state == S::Recording;
    auto c = v.state == S::Recording ? colour::rec : loop_colour(loop).withAlpha(lit ? (selected ? 1.0f : 0.8f) : 0.4f);
    uint32_t span = std::max<uint32_t>(v.span, 1);
    // During a take only what is recorded shows; a loop fills its width.
    float shown = v.state == S::Recording ? std::min(1.0f, float(v.position) / float(span)) : 1.0f;
    unsigned bars = unsigned(w / 2);
    for (unsigned b = 0; b < bars; ++b) {
        float at = float(b) / float(bars);
        if (at > shown) break;
        unsigned c0 = unsigned(at * looper::peak_columns), c1 = std::max(c0 + 1, unsigned(float(b + 1) / float(bars) * looper::peak_columns));
        uint8_t peak = 0;
        for (unsigned k = c0; k < c1 && k < looper::peak_columns; ++k) peak = std::max(peak, l.peaks[k]);
        float bh = std::max(1.0f, peak / 255.0f * (h - 2));
        p.fill(x + b * 2, cy - bh / 2, 1.5f, bh, c, 0.5f);
    }
    if (v.state != S::Recording && v.length) {
        float px = x + w * float(v.position % v.length) / float(v.length);
        p.fill(px, y, 1.5f, h, colour::text.withAlpha(lit ? 1.0f : 0.4f));
    }
}

void draw_loops(Paint& p, const Snapshot& s) {
    constexpr float gap = 3, top = zone::picture_y + 2, rows_h = zone::picture_h - 4;
    constexpr float row_h = (rows_h - 3 * gap) / 4;
    constexpr float x = zone::pad, w = zone::width - 2 * zone::pad;
    constexpr float tag_w = 44, info_w = 62;
    for (unsigned n = 0; n < loop_tracks; ++n) {
        const auto& l = s.loops[n];
        const auto& v = l.view;
        float y = top + n * (row_h + gap);
        bool selected = n == s.ui.loop % loop_tracks;
        p.fill(x, y, w, row_h, selected ? colour::raised : colour::quiet, 4);
        if (selected) {
            p.g.saveState();
            p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), 3, int(row_h)));
            p.fill(x, y, 8, row_h, loop_colour(n), 4);
            p.g.restoreState();
        }
        p.text("L" + juce::String(n + 1), x + 8, y + 2, tag_w - 8, 15, plain(loop_colour(n), type::list, Weight::Bold));
        juce::String word; juce::Colour wc;
        state_word(s, l, n, word, wc);
        if (word.isNotEmpty()) p.text(word, x + 8, y + 17, tag_w - 8, 12, caps_text(wc));
        float wx = x + tag_w, ww = w - tag_w - info_w - 6;
        wave(p, l, n, wx, y + 3, ww, row_h - 6, selected);
        // Length (or the take so far) and source; a loop at another tempo says the tempo it was recorded at.
        float ix = x + w - info_w - 4;
        juce::String length = v.state == S::Recording && !v.bars ? juce::String("Free")
                            : v.ticks > 0 ? juce::String(loop_length_name(unsigned(std::lround(v.ticks / std::max(1u, s.project.bar_ticks())))))
                            : juce::String(loop_length_name(v.bars));
        if (v.ticks > 0 && std::fabs(v.ticks / s.project.bar_ticks() - std::round(v.ticks / s.project.bar_ticks())) > 0.01)
            length = juce::String(v.ticks / s.project.beat_ticks(), 1) + " beats";
        p.text(length, ix, y + 2, info_w, 15, p.fit(length, plain(selected ? colour::text : colour::sec, type::body, Weight::Medium), info_w, type::floor),
               juce::Justification::centredRight);
        juce::String below = v.stretched ? juce::String(v.tempo % 10 ? juce::String(v.tempo / 10.0, 1) : juce::String(v.tempo / 10)) + " BPM"
                                         : juce::String(loop_source_name(v.source));
        auto style = plain(v.stretched ? colour::amber : colour::mid, type::small);
        p.text(below, ix, y + 17, info_w, 12, p.fit(below, style, info_w, type::floor), juce::Justification::centredRight);
        if (s.project.loop_mix[n].mute) p.fill(x, y, w, row_h, colour::bg.withAlpha(0.5f), 4);
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

void draw_sheet(Paint& p, const Snapshot& s) {
    unsigned loop = s.ui.loop % loop_tracks;
    const auto& v = s.loops[loop].view;
    bool audio = v.length > 0;
    MenuSheet m;
    m.title = "L" + juce::String(loop + 1);
    m.detail = juce::String(looper::state_word(v.state));
    m.accent = loop_colour(loop);
    m.count = unsigned(LoopRow::Count);
    m.current = s.ui.loop_row % m.count;
    auto& r = m.rows;
    r[0] = {"Source", juce::String(loop_source_name(s.project.looper.loops[loop].source)), true, false,
            v.state == S::Recording || v.state == S::Overdub};
    bool playing = v.state == S::Playing || v.state == S::Overdub;
    r[1] = {juce::String(playing ? "Stop" : "Play") + " at " + boundary_word(s), {}, false, false, !audio};
    r[2] = {"Stop now", {}, false, false, !(playing || v.state == S::Recording)};
    r[3] = {juce::String(v.undo == looper::Undo::Redo ? "Redo" : "Undo") + " last overdub",
            juce::String(v.layers) + (v.layers == 1 ? " layer" : " layers"), false, false, v.undo == looper::Undo::None};
    bool kit = s.track().engine == EngineKind::Sampler;
    bool committable = audio && (v.state == S::Playing || v.state == S::Stopped) && kit;
    r[4] = {"Commit to pad", kit ? "T" + juce::String(s.ui.track + 1) : juce::String("a kit"), false, false, !committable};
    r[5] = {"Commit + note at bar 1", {}, false, false, !committable};
    r[6] = {"Clear loop", {}, false, true, v.state == S::Empty};
    r[7] = {"Looper off", {}, false, true, false};
    menu_sheet(p, m);
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
    if (on && s.ui.loop_sheet) keys = SoftKeys {{"BACK", "", "DO IT"}, -1, loop_colour(loop), {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}};
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
    if (s.ui.looper_view % 2 == 1) draw_mix(p, s);
    else draw_loops(p, s);
    if (s.ui.loop_sheet) draw_sheet(p, s);
}

void pad(Context& c, unsigned pad, uint8_t, bool down) { loop_grid_pad(c, pad, down); }

void pick(Context& c, int delta) {
    if (c.ui.loop_sheet) { c.ui.loop_row = uint8_t((int(c.ui.loop_row) + delta + int(LoopRow::Count)) % int(LoopRow::Count)); return; }
    c.ui.loop = uint8_t((int(c.ui.loop) + delta + int(loop_tracks)) % int(loop_tracks));
}

void push(Context& c) {
    if (!c.project.looper.on) { set_looper(c, true); return; }
    if (c.ui.loop_sheet) { loop_sheet_push(c); return; }
    looper_do(c, looper::Do::Primary, c.ui.loop);
}

void soft(Context& c, int key) {
    auto& ui = c.ui;
    if (!c.project.looper.on) { if (key == 2) set_looper(c, true); return; }
    if (ui.loop_sheet) {
        if (key == 0) ui.loop_sheet = false;
        if (key == 2) loop_sheet_push(c);
        return;
    }
    if (key == 2) { ui.loop_sheet = true; ui.loop_row = 0; return; }
    ui.looper_view = uint8_t(key);
}
} // namespace

const PageSpec looper_page {draw, pad, pick, push, soft, pick}; // Minus / Plus: the loop (the sheet's row), as the wheel

} // namespace melodee
