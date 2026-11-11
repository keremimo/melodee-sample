// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// STEP EDIT (Shift + Seq, as printed): the level under the editor, the
// selected track's pattern one step of Q at a time, as on the MPC; B1 CLOSE,
// Push, Seq or Shift + Seq again go back to the editor (shell/seq_layer.h).
// The wheel moves a step, Minus / Plus go through the step's events, a pad
// plays and picks its event on the step. K1 the pattern's length (as
// on Patterns), K2 Q, K3 the event's velocity; Shift + K1 its length,
// Shift + K2 the time signature. The fader nudges the event's timing, up
// later and down earlier (shell/fader.cpp). B3 erases the event; Erase + pad
// erases all of that pad's events once B3 says so.
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/page_stack.h"
#include "melodee/shell/step_edit.h"
#include <algorithm>
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;

// Bar.beat.tick, as the MPC counts (from 1).
juce::String where(const Snapshot& s, uint32_t ticks) {
    unsigned bar = s.project.bar_ticks(), beat = s.project.beat_ticks();
    char text[24];
    std::snprintf(text, sizeof(text), "%u.%u.%02u", ticks / bar + 1, ticks % bar / beat + 1, ticks % beat);
    return text;
}

juce::String ticks_text(int ticks) { return juce::String(ticks) + (std::abs(ticks) == 1 ? " tick" : " ticks"); }

void draw(Paint& p, const Snapshot& s) {
    const auto& pattern = s.pattern;
    auto c = colour_of(s);
    unsigned q = pattern.q(), step = unsigned(std::max<int>(s.ui.step_cursor, 0)) % pattern.steps();
    auto in = step_events(pattern, step);
    int chosen = selected_event(pattern, s.ui);
    unsigned index = chosen >= 0 ? unsigned(chosen) - unsigned(in.first) : 0;

    Header h = header_of(s);
    h.left.push_back(pill(where(s, step * q), colour::text));
    h.left.push_back(pill("Q " + juce::String(division_name(pattern.division))));
    h.left.push_back(pill(in.count ? "EVENT " + juce::String(index + 1) + " / " + juce::String(in.count) : juce::String("NO EVENTS"),
                          in.count ? colour::mid : colour::dim));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"CLOSE", "", "ERASE"}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);

    // Left: the step, and its events as a list (the selected one raised).
    float x = zone::pad, y = zone::picture_y + 2, w = 196;
    float tw = p.text("Step " + juce::String(step + 1), x, y, 120, 22, plain(colour::text, type::title, Weight::SemiBold));
    p.text("/ " + juce::String(pattern.steps()), x + tw + 5, y + 5, 60, 14, plain(colour::dim, type::small));
    constexpr float row_h = 20;
    float ly = y + 26;
    constexpr unsigned shown_rows = 5;
    if (!in.count) {
        p.fill(x, ly, w, row_h, colour::quiet, 4);
        p.text("No events on this step", x + 8, ly, w - 16, row_h, plain(colour::dim, type::small));
    }
    unsigned top = index >= shown_rows ? index + 1 - shown_rows : 0;
    for (unsigned k = top; k < in.count && k < top + shown_rows; ++k) {
        const auto& e = pattern.events[in.first + k];
        float ry = ly + (k - top) * (row_h + 2);
        bool on = k == index;
        p.fill(x, ry, w, row_h, on ? colour::raised : colour::quiet, 4);
        if (on) p.fill(x, ry + 3, 2.5f, row_h - 6, c, 1);
        auto name = key_label(s.track(), e.key);
        p.text(name, x + 8, ry, 40, row_h, plain(on ? colour::text : colour::sec, type::small, Weight::Medium));
        int offset = int(e.tick) - int(step * q);
        p.text(offset ? "+" + juce::String(offset) : juce::String("on Q"), x + 50, ry, 40, row_h, plain(offset ? c : colour::dim, type::small));
        juce::String length = juce::String(e.length) + " t";
        p.text(length, x + 92, ry, 46, row_h, plain(colour::sec, type::small));
        // Velocity as a small bar and its number.
        float vx = x + 140, vw = 30;
        p.fill(vx, ry + row_h / 2 - 1.5f, vw, 3, colour::line, 1.5f);
        p.fill(vx, ry + row_h / 2 - 1.5f, vw * e.velocity / 127.0f, 3, on ? c : colour::mid, 1.5f);
        p.text(juce::String(e.velocity), vx + vw + 2, ry, 24, row_h, plain(on ? colour::text : colour::sec, type::small), juce::Justification::centredRight);
    }
    if (in.count > shown_rows)
        p.text(juce::String(in.count - shown_rows) + " more: Minus / Plus", x, ly + shown_rows * (row_h + 2), w, 10, caps_text(colour::dim));

    // Right: the steps around this one, each event a tick where it starts and
    // a bar as long as it is held; the selected one in white.
    float gx = x + w + 10, gw = zone::width - zone::pad - gx, gy = y + 4;
    p.text("AROUND", gx, gy, gw, 9, caps_text(colour::mid));
    constexpr unsigned around = 4; // steps shown: this one in the middle
    unsigned first = step >= around / 2 ? step - around / 2 : 0;
    uint32_t from = first * q, span = around * q;
    float sy = gy + 13, sh = 64;
    p.fill(gx, sy, gw, sh, colour::panel, 3);
    for (unsigned k = 0; k <= around; ++k) p.fill(gx + gw * k / around, sy, k % around ? 0.5f : 1, sh, colour::line);
    p.fill(gx + gw * (step - first) / around, sy, gw / around, sh, c.withAlpha(0.14f));
    unsigned lane = 0;
    for (size_t n = pattern.lower(from); n < pattern.events.size() && pattern.events[n].tick < from + span && pattern.events[n].tick < pattern.length; ++n) {
        const auto& e = pattern.events[n];
        float ex = gx + gw * float(e.tick - from) / span, ew = std::max(2.0f, gw * float(e.length) / span);
        ew = std::min(ew, gx + gw - ex);
        float ey = sy + 4 + (lane++ % 6) * 10;
        bool on = int(n) == chosen;
        p.fill(ex, ey, ew, 6, on ? colour::text : c.withAlpha(0.7f), 2);
    }
    if (s.playing) {
        uint32_t at = s.position % std::max<uint32_t>(pattern.length, 1);
        if (at >= from && at < from + span) p.fill(gx + gw * float(at - from) / span, sy, 1, sh, colour::text);
    }
    if (chosen >= 0) {
        const auto& e = pattern.events[size_t(chosen)];
        auto line = plain(colour::sec, type::small, Weight::Medium);
        p.text("At " + where(s, e.tick), gx, sy + sh + 4, gw, 14, line);
        p.text("Length " + ticks_text(e.length), gx, sy + sh + 17, gw, 14, line);
        p.text("Fader: nudge", gx, sy + sh + 30, gw, 14, plain(colour::dim, type::small, Weight::Medium));
    }
}

void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    int key = pad_key(c.project, c.track(), pad);
    if (down && c.ui.erase) { // Erase + pad: all of that pad's events, once B3 says so
        if (key >= 0) ask(c, UiState::Ask::EraseKeyEvents, key);
        return;
    }
    play_pad(c, pad, velocity, down); // heard (and recorded with Seq Record), as anywhere
    if (down && key >= 0) select_key(c, key);
}

void wheel(Context& c, int delta) { step_move(c, delta); }

// Back to the editor Step Edit sits under (Steps, Roll or Motion: the track's last).
void close(Context& c) {
    auto engine = c.track().engine;
    c.open(seq_editor_page(c.ui, engine == EngineKind::Sampler || engine == EngineKind::Kit909));
}

void push(Context& c) { close(c); }

void soft(Context& c, int key) {
    if (key == 0) close(c);
    if (key == 2) erase_event(c);
}

void nudge(Context& c, int way) { event_move(c, way); }
} // namespace

const PageSpec step_edit_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
