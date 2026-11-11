// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// COMPRESSOR (Shift + Pad 5, parity phase 7), as on the MPC: the master
// compressor after the master chain. B1 SIDECHAIN (one track keys it and
// the rest of the mix ducks under that track), B2 COLOR (a little low end,
// wobble and saturation), B3 bypasses or engages it. K1 attack (0.1-150
// ms), K2 release (3-300 ms), K3 amount (threshold and ratio together;
// makeup follows, none with Sidechain), Shift + K3 In Boost (into the
// detector alone with Sidechain). The wheel or Minus / Plus pick the key
// track. The picture: its curve (with Sidechain, the key's level against
// what the rest loses) and what it takes off now.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

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

    float x = zone::pad + 4, y = zone::picture_y + 6, side = zone::picture_h - 16;
    p.fill(x, y, side, side, colour::panel, 4);
    float amount = c.amount / 100.0f, threshold = -amount * 30, ratio = 1 + amount * amount * 9;
    float makeup = -threshold * (1 - 1 / ratio) * 0.5f;
    auto over = [&](float in_db) { // dB taken off a signal at in_db (In Boost in front)
        float boosted = in_db + c.boost;
        return boosted > threshold ? (boosted - threshold) * (1 - 1 / ratio) : 0.0f;
    };
    // The mix: input across, output up, -40..0 dB. Sidechain: the key's level
    // across, what the rest of the mix keeps up (0 dB at the top, -24 at the foot).
    float depth = c.sidechain ? 24 : 40; // the up axis's span
    auto at = [&](float in_db) {
        if (c.sidechain) return std::clamp(-over(in_db), -depth, 0.0f);
        return std::clamp(in_db + c.boost - over(in_db) + makeup, -40.0f, 0.0f);
    };
    p.fill(x, c.sidechain ? y : y + side - 1, side, 1, colour::line);
    juce::Path curve; // one stroke, every 2 dB
    for (int db = -40; db <= 0; db += 2) {
        float px = x + (db + 40) / 40.0f * side, py = y + side - (at(float(db)) + depth) / depth * side;
        if (db == -40) curve.startNewSubPath(px, py); else curve.lineTo(px, py);
    }
    auto stroke = c.on ? (c.sidechain ? track_colour(key) : accent) : colour::mid;
    p.g.setColour(stroke);
    p.g.strokePath(curve, juce::PathStrokeType(1.5f));
    float tx = x + (threshold - c.boost + 40) / 40.0f * side;
    if (tx > x && tx < x + side) p.fill(tx, y + 4, 1, side - 8, colour::line);

    // What it takes off now, and the settings in words.
    float gx = x + side + 16, gw = zone::width - zone::pad - gx;
    p.text(c.sidechain ? "Ducking" : "Gain reduction", gx, y, gw, 14, plain(colour::sec, type::small));
    float reduce = std::round(std::clamp(s.compression, 0.0f, 20.0f) * 2) / 2; // half a dB a step: the screen redraws only on a change
    p.fill(gx, y + 18, gw, 10, colour::panel, 3);
    p.fill(gx, y + 18, gw * reduce / 20.0f, 10, c.on ? stroke : colour::mid, 3);
    p.text(juce::String(-reduce, 1) + " dB", gx, y + 30, gw, 12, caps_text(colour::mid));
    juce::String state = !c.on ? "Bypassed" : c.sidechain ? "All but track " + juce::String(key + 1) + " duck" : "Engaged on the master";
    p.text(state, gx, y + 56, gw, 14, plain(c.on ? colour::text : colour::dim, type::small, Weight::SemiBold));
    p.text(juce::String(c.sidechain ? "Key over " : "Threshold ") + juce::String(int(std::lround(threshold))) + " dB", gx, y + 74, gw, 12,
           plain(colour::sec, type::small));
    p.text(juce::String("Ratio ") + juce::String(ratio, 1) + ":1", gx, y + 88, gw, 12, plain(colour::sec, type::small));
    p.text(c.color ? "Color: tape warmth" : "Color off", gx, y + 102, gw, 12, plain(colour::sec, type::small));
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
