// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MOD (roadmap P4; MOTION again on Motion): the project's modulators and
// macros (model/modulators.h). B1 MOD, B2 MACRO; B3 ADD puts the lane last
// turned anywhere (a pot on Stage, Edit, a chain, Tracks' strip) on the one
// in hand. Modulators: K1 which (1-8), K2 its rate (a follower's rise), K3
// the target in hand's depth; Shift + K1 its kind (Off, LFO, Follow), Shift +
// K2 an LFO's shape or a follower's source, Shift + K3 an LFO's clock or a
// follower's fall. Macros: K1 which, K2 its value (every target with it), K3
// the target in hand's high end, Shift + K3 its low. The wheel picks the
// target in hand, Push takes it off, Minus / Plus the one in hand. The
// picture: a modulator's wave (or its source's level) with where it is now,
// and its targets as rows, each its track's chip, its lane and its depth; a
// macro's value as an arc and its targets, each its lane and its range.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/mod_jobs.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

juce::String lane_text(const Snapshot& s, unsigned track, unsigned param, unsigned pad) {
    return juce::String(lane_name(s.project.tracks[track % track_count], motion_lane(param, pad), &s.project));
}

// A target row: the track's chip, the lane's name, and on the right what the row says of it.
void target_row(Paint& p, const Snapshot&, float x, float y, float w, float h, bool in_hand, unsigned track, const juce::String& lane,
                const juce::String& right, juce::Colour c) {
    if (in_hand) { p.fill(x, y, w, h, colour::raised, 3); p.fill(x, y, 2.5f, h, c, 1); }
    p.fill(x + 6, y + (h - 14) / 2, 18, 14, track_colour(track), 3);
    p.text(juce::String(track + 1), x + 6, y + (h - 14) / 2, 18, 14, plain(colour::bg, type::small, Weight::SemiBold), juce::Justification::centred);
    auto style = plain(in_hand ? colour::text : colour::sec, type::body, in_hand ? Weight::Medium : Weight::Regular);
    float rw = p.width(right, plain(colour::mid, type::small, Weight::Medium)) + 4;
    p.text(lane, x + 30, y, w - 36 - rw, h, p.fit(lane, style, w - 36 - rw, type::floor));
    p.text(right, x + w - rw - 4, y, rw, h, plain(in_hand ? colour::text : colour::mid, type::small, Weight::Medium), juce::Justification::centredRight);
}

void draw_mods(Paint& p, const Snapshot& s, juce::Colour c) {
    const auto& m = s.project.modulators[s.ui.mod_index % modulator_count];
    constexpr float x = zone::pad, y = zone::picture_y + 4, h = zone::picture_h - 8;
    constexpr float lw = 120, rx = x + lw + 8, rw = zone::width - zone::pad - rx;
    // Left: the wave over one cycle (an LFO) or the source's level (a follower), where it is now.
    p.fill(x, y, lw, h, colour::quiet, 4);
    float value = s.mod_values[s.ui.mod_index % modulator_count];
    if (m.kind == ModKind::Lfo) {
        juce::Path wave;
        float wx = x + 8, ww = lw - 16, cy = y + h / 2 - 6, amp = h / 2 - 22;
        for (unsigned k = 0; k <= 64; ++k) {
            double phase = k / 64.0;
            float v = lfo_value(m.shape, phase, -0.4f, 0.6f);
            float px = wx + ww * float(phase), py = cy - v * amp;
            if (!k) wave.startNewSubPath(px, py); else wave.lineTo(px, py);
        }
        p.g.setColour(c);
        p.g.strokePath(wave, juce::PathStrokeType(1.8f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
        p.fill(wx, cy - value * amp - 1, ww, 2, colour::text.withAlpha(0.5f));
        juce::String rate = m.sync ? juce::String(lfo_division_name(m.division)) : juce::String(lfo_hertz(m.rate), 2) + " Hz";
        p.text(rate, x + 8, y + h - 20, lw - 16, 16, plain(colour::sec, type::small, Weight::Medium));
    } else if (m.kind == ModKind::Follower) {
        unsigned src = m.source % (track_count + bus_count);
        juce::String name = src < track_count ? juce::String(src + 1) : juce::String::charToString(juce::juce_wchar('A' + src - track_count));
        icon(p, Icon::Route, x + 8, y + 8, 16, colour::sec, 1.4f);
        p.text(name, x + 28, y + 6, 60, 20, plain(src < track_count ? track_colour(src) : colour::text, type::list, Weight::SemiBold));
        float bh = h - 44;
        p.fill(x + lw / 2 - 8, y + 30, 16, bh, colour::line, 3);
        p.fill(x + lw / 2 - 8, y + 30 + bh * (1 - std::clamp(value, 0.0f, 1.0f)), 16, bh * std::clamp(value, 0.0f, 1.0f), c, 3);
    } else {
        icon(p, Icon::Modulation, x + lw / 2 - 14, y + h / 2 - 20, 28, colour::dim, 1.4f);
    }
    // Right: its targets, one a row; the empty ones a quiet dash.
    float row_h = (h - 4) / float(modulator_targets);
    for (unsigned i = 0; i < modulator_targets; ++i) {
        const auto& g = m.targets[i];
        float ry = y + 2 + i * row_h;
        bool hand = i == s.ui.mod_target % modulator_targets;
        if (!g.used()) {
            if (hand) p.fill(rx, ry, rw, row_h - 3, colour::surf, 3);
            p.fill(rx + 8, ry + row_h / 2 - 1.5f, 14, 1.5f, colour::dim);
            continue;
        }
        juce::String depth = (g.depth > 0 ? "+" : g.depth < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(std::abs(g.depth)) + "%";
        target_row(p, s, rx, ry, rw, row_h - 3, hand, g.track, lane_text(s, g.track, g.param, g.pad), depth, c);
    }
}

void draw_macros(Paint& p, const Snapshot& s, juce::Colour c) {
    const auto& m = s.project.macros[s.ui.macro_index % macro_count];
    constexpr float x = zone::pad, y = zone::picture_y + 4, h = zone::picture_h - 8;
    constexpr float lw = 100, rx = x + lw + 8, rw = zone::width - zone::pad - rx;
    p.fill(x, y, lw, h, colour::quiet, 4);
    // Its value as an arc, the number in it.
    float cx = x + lw / 2, cy = y + h / 2 + 4, radius = 36;
    constexpr float from = -2.4f, span = 4.8f;
    juce::Path track_arc, value_arc;
    track_arc.addCentredArc(cx, cy, radius, radius, 0, from, from + span, true);
    value_arc.addCentredArc(cx, cy, radius, radius, 0, from, from + span * m.value / 127.0f, true);
    p.g.setColour(colour::line);
    p.g.strokePath(track_arc, juce::PathStrokeType(5, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    p.g.setColour(c);
    p.g.strokePath(value_arc, juce::PathStrokeType(5, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    p.text(juce::String(m.value), cx - 30, cy - 12, 60, 24, plain(colour::text, type::value, Weight::SemiBold), juce::Justification::centred);
    // Its targets, four rows at a time around the one in hand.
    constexpr unsigned shown = 4;
    unsigned hand = s.ui.macro_target % macro_targets, first = std::min(hand / shown * shown, macro_targets - shown);
    float row_h = (h - 4) / float(shown);
    for (unsigned k = 0; k < shown; ++k) {
        unsigned i = first + k;
        const auto& g = m.targets[i];
        float ry = y + 2 + k * row_h;
        bool in_hand = i == hand;
        if (!g.used()) {
            if (in_hand) p.fill(rx, ry, rw, row_h - 3, colour::surf, 3);
            p.fill(rx + 8, ry + row_h / 2 - 1.5f, 14, 1.5f, colour::dim);
            continue;
        }
        juce::String range = juce::String(g.low) + juce::String::fromUTF8(" \xe2\x86\x92 ") + juce::String(g.high);
        target_row(p, s, rx, ry, rw, row_h - 3, in_hand, g.track, lane_text(s, g.track, g.param, g.pad), range, c);
    }
    if (macro_targets > shown) page_dots(p, zone::width - zone::pad, y + h - 8, int(macro_targets / shown), int(first / shown), c, true);
}

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    bool macros = s.ui.mod_view == 1;
    Header h = header_of(s);
    if (macros) h.left.push_back(pill("MACRO " + juce::String(s.ui.macro_index % macro_count + 1)));
    else {
        const auto& m = s.project.modulators[s.ui.mod_index % modulator_count];
        h.left.push_back(pill("MOD " + juce::String(s.ui.mod_index % modulator_count + 1)));
        h.left.push_back(pill(mod_kind_name(m.kind), m.kind == ModKind::Off ? colour::dim : c));
    }
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{"MOD", "MACRO", "ADD"}, macros ? 1 : 0, c, {KeyIcon::None, KeyIcon::None, KeyIcon::None}};
    keys.glyphs = {Icon::Modulation, Icon::Sliders, Icon::Plus};
    frame(p, s, keys, h);
    if (macros) draw_macros(p, s, c);
    else draw_mods(p, s, c);
}

void wheel(Context& c, int delta) { // the target in hand
    auto& ui = c.ui;
    if (ui.mod_view == 1) ui.macro_target = uint8_t(std::clamp(int(ui.macro_target) + delta, 0, int(macro_targets) - 1));
    else ui.mod_target = uint8_t(std::clamp(int(ui.mod_target) + delta, 0, int(modulator_targets) - 1));
}

void push(Context& c) { mod_remove(c); }

void soft(Context& c, int key) {
    if (key == 0) c.ui.mod_view = 0;
    else if (key == 1) c.ui.mod_view = 1;
    else mod_add(c);
}

void nudge(Context& c, int way) { // the modulator or macro in hand
    auto& ui = c.ui;
    if (ui.mod_view == 1) { ui.macro_index = uint8_t(std::clamp(int(ui.macro_index) + way, 0, int(macro_count) - 1)); ui.macro_target = 0; }
    else { ui.mod_index = uint8_t(std::clamp(int(ui.mod_index) + way, 0, int(modulator_count) - 1)); ui.mod_target = 0; }
}
} // namespace

const PageSpec mod_page {draw, play_pad, wheel, push, soft, nudge};

} // namespace melodee
