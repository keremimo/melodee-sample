// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// STEPS (Shift + Seq, Step Edit): the pads are the sixteen steps of the bar in view, pad 1
// bottom left. Tap a pad: the step on (with the last note played) or off.
// Hold a pad: that step is on the cards (length, velocity, chance) and the
// wheel changes its pitch. Minus / Plus change bar; Erase + pad clears.
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;
constexpr uint32_t tap_ms = 300;

unsigned bars_of(const Pattern& p) { return std::max(1u, (unsigned(p.length) + bar_steps - 1) / bar_steps); }

void draw(Paint& p, const Snapshot& s) {
    const auto& pattern = s.pattern;
    auto c = colour_of(s);
    unsigned bars = bars_of(pattern), bar = std::min<unsigned>(s.ui.steps_bar, bars - 1), first = bar * bar_steps;
    Header h = header_of(s);
    h.left.push_back(pill("BAR " + juce::String(bar + 1) + " / " + juce::String(bars)));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"STEPS", "ROLL", "MOTION"}, 0, c}, h);

    int cursor = s.ui.step_cursor;
    const auto& cur = pattern.steps[size_t(std::max(cursor, 0)) % max_steps];
    float x0 = zone::pad, top = zone::picture_y;
    float tw = p.text("Step " + juce::String(cursor + 1), x0, top, 140, 22, plain(colour::text, type::title, Weight::SemiBold));
    if (const Note* n = cur.first()) p.text(note_name(n->key), x0 + tw + 6, top + 1, 60, 22, plain(c, 12, Weight::Medium));

    // A small roll of the bar: eight rows of the scale around its notes.
    std::array<uint8_t, 12> degrees {};
    unsigned count = scale_degrees(s.project.scale, degrees);
    auto row_key = [&](int index) { // scale note `index` above the root of octave 0
        int octave = index >= 0 ? index / int(count) : -((-index + int(count) - 1) / int(count));
        return 12 * octave + s.project.root + degrees[size_t(index - octave * int(count))];
    };
    auto index_of = [&](int key) {
        int best = 0;
        for (int i = 0; i < 80; ++i) if (row_key(i) <= key) best = i;
        return best;
    };
    int lo = 1000, hi = -1;
    for (unsigned st = first; st < first + bar_steps && st < pattern.length; ++st)
        for (auto& n : pattern.steps[st].notes)
            if (n.velocity) { lo = std::min(lo, index_of(n.key)); hi = std::max(hi, index_of(n.key)); }
    if (hi < 0) lo = hi = index_of(s.track().step_note());
    int top_row = std::max(hi, (lo + hi) / 2 + 4);
    constexpr float cw = 14.5f, rh = 8, rx = zone::pad, ry = zone::picture_y + 25, rw = 232;
    p.fill(rx, ry, rw, 8 * rh, colour::panel, 3);
    for (unsigned st = 1; st < bar_steps; ++st)
        p.fill(rx + st * cw, ry, st % 4 ? 0.5f : 1, 8 * rh, st % 4 ? colour::bg : colour::line);
    bool held = s.ui.held_step >= 0;
    int highlight = held ? s.ui.held_step : cursor;
    if (highlight >= int(first) && highlight < int(first + bar_steps))
        p.fill(rx + (highlight - first) * cw, ry, cw, 8 * rh, c.withAlpha(0.14f));
    for (unsigned st = first; st < first + bar_steps && st < pattern.length; ++st) {
        const auto& step = pattern.steps[st];
        for (auto& n : step.notes) {
            if (!n.velocity) continue;
            int row = top_row - index_of(n.key);
            if (row < 0 || row > 7) continue;
            float nx = rx + (st - first) * cw + 1, ny = ry + row * rh + 1.5f;
            float nw = std::min<float>(step.length, float(first + bar_steps - st)) * cw - 2;
            p.fill(nx, ny, nw, rh - 3, c, 2);
            if (int(st) == highlight) p.ring(nx - 1, ny - 1, nw + 2, rh - 1, colour::text, 2.5f);
        }
    }
    // Velocity bars and the step row, the playing step in white.
    float vy = ry + 8 * rh + 4;
    int playing_step = s.playing ? int(s.position % pattern.length) : -1;
    for (unsigned k = 0; k < bar_steps; ++k) {
        unsigned st = first + k;
        float x = rx + 1 + k * cw;
        bool inside = st < pattern.length;
        const Note* n = inside ? pattern.steps[st].first() : nullptr;
        bool head = int(st) == playing_step || int(st) == highlight;
        auto col = head ? colour::text : c;
        if (n) {
            float bh = n->velocity / 127.0f * 18;
            p.fill(x + 2, vy + 18 - bh, cw - 6, bh, col, 1.5f);
        }
        p.fill(x, vy + 21, cw - 2, 4, n || int(st) == playing_step ? col : colour::line, 2);
    }
    float fx = x0, fy = vy + 30;
    for (auto flag : {std::pair<const char*, int>{"ACCENT", step_accent}, {"SLIDE", step_slide}, {"TIE", step_tie}}) {
        bool on = cur.flags & flag.second;
        auto style = caps_text(on ? c : colour::dim);
        float w = p.width(flag.first, style) + 9;
        p.fill(fx, fy, w, 12.5f, colour::surf, 3);
        p.text(flag.first, fx, fy, w, 12.5f, style, juce::Justification::centred);
        fx += w + 4;
    }

    // Right column: the pads as steps, the bars and the pattern.
    float x = rx + rw + 7, w = zone::width - zone::pad - x, y = zone::picture_y + 3;
    p.text("PADS", x, y, w, 9, caps_text(colour::mid, 7));
    float gy = y + 13, gh = 70, gc = (w - 6) / 4, gr = (gh - 6) / 4;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned st = first + pad, row = 3 - pad / 4, column = pad % 4;
        bool on = st < pattern.length && !pattern.steps[st].empty();
        bool white = int(st) == highlight && held;
        float cx = x + column * (gc + 2), cy = gy + row * (gr + 2);
        p.fill(cx, cy, gc, gr, white ? colour::text : on ? c : colour::surf, 3);
        p.text(juce::String(pad + 1), cx, cy, gc, gr, plain(on || white ? colour::bg : colour::dim, 6.5f, Weight::SemiBold),
               juce::Justification::centred);
    }
    float by = gy + gh + 8;
    p.text("BAR " + juce::String(bar + 1) + " / " + juce::String(bars), x, by, w, 9, caps_text(colour::mid, 7));
    float bw = (w - 2 * std::min(bars, 4u) + 2) / std::min(bars, 4u);
    for (unsigned b = 0; b < std::min(bars, 4u); ++b)
        p.fill(x + b * (bw + 2), by + 12, bw, 4, b == bar % 4 ? c : colour::line, 2);
    p.text("PATTERN", x, by + 22, w, 9, caps_text(colour::mid, 7));
    p.text("P" + juce::String(s.track().pattern + 1), x, by + 31, w, 15, plain(c, 13, Weight::Medium));
}

void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    auto& pattern = c.pattern();
    unsigned st = unsigned(c.ui.steps_bar) * bar_steps + pad % 16;
    if (st >= pattern.length) return;
    if (down) {
        if (c.ui.erase) { c.edit_pattern(c.ui.track, c.track().pattern, "Erase step", [&] { c.set_step(c.ui.track, st, Step {}); }); return; }
        c.ui.step_cursor = int16_t(st);
        c.ui.held_step = int16_t(st);
        c.ui.held_since = c.now;
        c.ui.held_edited = false;
        c.ui.held_created = pattern.steps[st].empty();
        if (c.ui.held_created) {
            Step step;
            step.notes[0] = {c.track().step_note(), uint8_t(std::max<int>(velocity, 64))};
            c.set_step(c.ui.track, st, step);
        }
        return;
    }
    if (c.ui.held_step != int(st)) return;
    bool tap = c.now - c.ui.held_since < tap_ms;
    if (!c.ui.held_created && !c.ui.held_edited && tap) c.set_step(c.ui.track, st, Step {});
    c.ui.held_step = -1;
}

void wheel(Context& c, int delta) {
    auto& pattern = c.pattern();
    if (c.ui.held_step >= 0) {
        // The held step's notes move together, one semitone a detent.
        Step step = pattern.steps[size_t(c.ui.held_step)];
        for (auto& n : step.notes) if (n.velocity) n.key = uint8_t(std::clamp(n.key + delta, 0, 127));
        if (const Note* n = step.first()) {
            auto& t = c.track();
            t.last_note = n->key;
            if (t.engine == EngineKind::Sampler && n->key >= sampler_first_note && n->key < sampler_first_note + sampler_pads)
                t.last_pad = uint8_t(n->key - sampler_first_note); // on a kit the note is a pad
        }
        c.set_step(c.ui.track, unsigned(c.ui.held_step), step);
        c.ui.held_edited = true;
        return;
    }
    c.ui.step_cursor = int16_t(std::clamp<int>(c.ui.step_cursor + delta, 0, pattern.length - 1));
    c.ui.steps_bar = uint8_t(c.ui.step_cursor / bar_steps);
}

void nudge(Context& c, int way) {
    unsigned bars = bars_of(c.pattern());
    c.ui.steps_bar = uint8_t(std::clamp<int>(c.ui.steps_bar + way, 0, int(bars) - 1));
    c.ui.step_cursor = int16_t(c.ui.steps_bar * bar_steps + c.ui.step_cursor % bar_steps);
}

void soft(Context& c, int key) {
    static const PageId pages[] {PageId::Steps, PageId::Roll, PageId::Motion};
    c.open(pages[key % 3]);
}
} // namespace

const PageSpec steps_page {draw, pad, wheel, no_push, soft, nudge};

} // namespace melodee
