// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// COMPRESSOR (Shift + Pad 5, parity phase 7), as on the MPC: the master
// compressor after the master chain. B1 SIDECHAIN (one track keys it and
// the rest of the mix ducks under that track), B2 COLOR (a little low end,
// wobble and saturation), B3 bypasses or engages it. K1 attack (0.1-150
// ms), K2 release (3-300 ms), K3 amount (threshold and ratio together;
// makeup follows, none with Sidechain), Shift + K3 In Boost (into the
// detector alone with Sidechain). The wheel or Minus / Plus pick the key
// track. The picture: its curve with a dot where the level sits now (with
// Sidechain, the key's level across against what the rest keeps), and
// beside it the last 2.6 s: what the detector heard rising from the foot
// (the key in its track's colour), the threshold dashed, and what was
// taken off hanging from the top, so a ducking mix reads as the pump it is.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;
constexpr float floor_db = 40;  // the level scales: -40 .. 0 dB
constexpr float duck_db = 24;   // what is taken off: 0 .. 24 dB down from the top

void draw(Paint& p, const Snapshot& s) {
    const auto& c = s.project.compressor;
    auto accent = colour::text;
    unsigned key = c.key % track_count;
    Header h = header_of(s);
    h.left.push_back(pill("COMPRESSOR"));
    h.left.push_back(pill(c.on ? "ON" : "BYPASS", c.on ? accent : colour::mid));
    if (c.color) h.left.push_back(pill("COLOR", accent));
    if (c.sidechain) h.right.push_back(tag_of(s, key)); // the key track
    else h.right.push_back(label("MASTER", colour::mid));
    int lit = c.sidechain ? 0 : c.color ? 1 : -1; // one bar: Sidechain first (Color has its pill)
    frame(p, s, {{"SIDECHAIN", "COLOR", c.on ? "BYPASS" : "ENGAGE"}, lit, accent}, h);

    auto mix = c.on ? accent : colour::mid;                                      // the mix and what it keeps
    auto heard = c.sidechain ? track_colour(key) : colour::sec;                  // what the detector hears
    if (!c.on) heard = colour::mid;
    float amount = c.amount / 100.0f, threshold = -amount * 30, ratio = 1 + amount * amount * 9;
    float makeup = -threshold * (1 - 1 / ratio) * 0.5f;
    auto over = [&](float in_db) { // dB taken off a signal at in_db (In Boost in front)
        float boosted = in_db + c.boost;
        return boosted > threshold ? (boosted - threshold) * (1 - 1 / ratio) : 0.0f;
    };

    // The curve: input across, output up (-40..0 dB). Sidechain: the key across,
    // what the rest keeps up (0 dB at the top, -24 at the foot).
    float x = zone::pad + 4, y = zone::picture_y + 6, side = zone::picture_h - 16;
    p.fill(x, y, side, side, colour::panel, 4);
    float depth = c.sidechain ? duck_db : floor_db;
    auto at = [&](float in_db) {
        if (c.sidechain) return std::clamp(-over(in_db), -depth, 0.0f);
        return std::clamp(in_db + c.boost - over(in_db) + makeup, -depth, 0.0f);
    };
    auto curve_y = [&](float in_db) { return y + side - (at(in_db) + depth) / depth * side; };
    p.fill(x, c.sidechain ? y : y + side - 1, side, 1, colour::line);
    float tx = x + (threshold - c.boost + floor_db) / floor_db * side;
    if (tx > x && tx < x + side) p.fill(tx, y + 4, 1, side - 8, colour::line);
    juce::Path curve; // one stroke, every 2 dB
    for (int db = -40; db <= 0; db += 2) {
        float px = x + (db + floor_db) / floor_db * side;
        if (db == -40) curve.startNewSubPath(px, curve_y(float(db))); else curve.lineTo(px, curve_y(float(db)));
    }
    p.g.setColour(mix);
    p.g.strokePath(curve, juce::PathStrokeType(1.5f));
    if (c.sidechain) p.text("KEY", x + 6, y + side - 18, 40, 14, caps_text(heard));
    // Where the level sits now: on the curve, at what the detector heard last.
    float now_db = s.comp_trail.back().level - c.boost; // the curve's input has In Boost in front
    if (c.on && now_db > -floor_db) {
        float dx = x + std::min(now_db + floor_db, floor_db) / floor_db * side, dy = curve_y(std::min(now_db, 0.0f));
        p.g.setColour(colour::panel); p.g.fillEllipse(dx - 5, dy - 5, 10, 10);
        p.g.setColour(heard); p.g.fillEllipse(dx - 3.5f, dy - 3.5f, 7, 7);
    }

    // The last 2.6 s, newest on the right.
    float gx = x + side + 12, gw = zone::width - zone::pad - gx, gh = side - 32;
    p.fill(gx, y, gw, gh, colour::panel, 4);
    float cw = gw / float(comp_columns);
    auto level_y = [&](float db) { return y + gh - std::clamp((db + floor_db) / floor_db, 0.0f, 1.0f) * (gh - 2); };
    auto duck_y = [&](float db) { return y + std::clamp(db / duck_db, 0.0f, 1.0f) * (gh - 2); };
    juce::Path level, taken;
    level.startNewSubPath(gx, y + gh);
    taken.startNewSubPath(gx, y);
    for (unsigned n = 0; n < comp_columns; ++n) {
        const auto& col = s.comp_trail[n];
        float cx = gx + (n + 0.5f) * cw;
        level.lineTo(cx, level_y(col.level));
        taken.lineTo(cx, duck_y(col.reduce));
    }
    level.lineTo(gx + gw, y + gh); level.closeSubPath();
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(gx), int(y), int(gw), int(gh)));
    p.g.setColour(heard.withAlpha(c.sidechain ? 0.55f : 0.3f)); // the key stands out; the mix itself sits back
    p.g.fillPath(level);
    juce::Path hanging = taken;
    hanging.lineTo(gx + gw, y); hanging.closeSubPath();
    p.g.setColour(mix.withAlpha(0.14f));
    p.g.fillPath(hanging);
    p.g.setColour(mix);
    p.g.strokePath(taken, juce::PathStrokeType(1.5f));
    p.g.restoreState();
    float ty = level_y(threshold);
    for (float dx = gx + 2; dx < gx + gw - 2; dx += 6) p.fill(dx, ty, 3, 1, colour::line); // the threshold

    // What it takes off: the most of the last third of a second, half a dB a step.
    float most = 0;
    for (unsigned n = comp_columns - 10; n < comp_columns; ++n) most = std::max(most, s.comp_trail[n].reduce);
    most = c.on ? std::round(std::min(most, 40.0f) * 2) / 2 : 0;
    float ry = y + gh + 6, rh = side - gh - 6;
    p.text(c.sidechain ? "DUCKING" : "REDUCTION", gx, ry, gw, rh, caps_text(colour::mid), juce::Justification::centredLeft);
    auto unit_style = plain(colour::sec, type::small);
    float uw = p.width("dB", unit_style);
    p.text("dB", gx + gw - uw, ry + 3, uw, rh, unit_style, juce::Justification::centredRight);
    p.text(most > 0 ? "-" + juce::String(most, 1) : "0.0", gx, ry, gw - uw - 4, rh,
           plain(c.on ? colour::text : colour::dim, type::title, Weight::SemiBold), juce::Justification::centredRight);
}

void soft(Context& c, int key) {
    const auto& comp = c.project.compressor;
    if (key == 0) {
        c.set_compressor(6, !comp.sidechain);
        c.notice(comp.sidechain ? "Sidechain: track " + std::to_string(comp.key + 1) + " keys it" : "Sidechain off");
    }
    if (key == 1) { c.set_compressor(1, !comp.color); c.notice(comp.color ? "Color on" : "Color off"); }
    if (key == 2) { c.set_compressor(0, !comp.on); c.notice(comp.on ? "Compressor engaged" : "Compressor bypassed"); }
}

// The wheel or Minus / Plus: the key track, while Sidechain is on.
void pick_key(Context& c, int way) {
    const auto& comp = c.project.compressor;
    if (!comp.sidechain || !way) return;
    c.set_compressor(7, std::clamp<int>(comp.key + way, 0, int(track_count) - 1));
}
} // namespace

const PageSpec compressor_page {draw, play_pad, pick_key, no_push, soft, pick_key};

} // namespace melodee
