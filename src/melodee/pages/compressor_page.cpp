// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// COMPRESSOR (Shift + Pad 5, parity phase 7), as on the MPC: the master
// compressor after the master chain. B1 COLOR (a little low end, wobble
// and saturation), B3 bypasses or engages it. K1 attack (0.1-150 ms), K2
// release (3-300 ms), K3 amount (threshold and ratio together; makeup
// follows), Shift + K3 In Boost. The picture: its curve and what it takes
// off now.
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
    Header h = header_of(s);
    h.left.push_back(pill("COMPRESSOR"));
    h.left.push_back(pill(c.on ? "ON" : "BYPASS", c.on ? accent : colour::mid));
    if (c.color) h.left.push_back(pill("COLOR", accent));
    h.right.push_back(label("MASTER", colour::mid));
    frame(p, s, {{"COLOR", "", c.on ? "BYPASS" : "ENGAGE"}, c.color ? 0 : -1, accent}, h);

    // The curve: input across, output up, -40..0 dB.
    float x = zone::pad + 4, y = zone::picture_y + 6, side = zone::picture_h - 16;
    p.fill(x, y, side, side, colour::panel, 4);
    float amount = c.amount / 100.0f, threshold = -amount * 30, ratio = 1 + amount * amount * 9;
    float makeup = -threshold * (1 - 1 / ratio) * 0.5f;
    auto at = [&](float in_db) {
        float boosted = in_db + c.boost;
        float out = boosted > threshold ? threshold + (boosted - threshold) / ratio : boosted;
        return std::clamp(out + makeup, -40.0f, 0.0f);
    };
    p.fill(x, y + side - 1, side, 1, colour::line);
    juce::Path curve; // one stroke, every 2 dB
    for (int db = -40; db <= 0; db += 2) {
        float px = x + (db + 40) / 40.0f * side, py = y + side - (at(float(db)) + 40) / 40.0f * side;
        if (db == -40) curve.startNewSubPath(px, py); else curve.lineTo(px, py);
    }
    p.g.setColour(c.on ? accent : colour::mid);
    p.g.strokePath(curve, juce::PathStrokeType(1.5f));
    float tx = x + (threshold - c.boost + 40) / 40.0f * side;
    if (tx > x && tx < x + side) p.fill(tx, y + 4, 1, side - 8, colour::line);

    // What it takes off now, and the settings in words.
    float gx = x + side + 16, gw = zone::width - zone::pad - gx;
    p.text("Gain reduction", gx, y, gw, 14, plain(colour::sec, 10));
    float reduce = std::round(std::clamp(s.compression, 0.0f, 20.0f) * 2) / 2; // half a dB a step: the screen redraws only on a change
    p.fill(gx, y + 18, gw, 10, colour::panel, 3);
    p.fill(gx, y + 18, gw * reduce / 20.0f, 10, c.on ? accent : colour::mid, 3);
    p.text(juce::String(-reduce, 1) + " dB", gx, y + 30, gw, 12, caps_text(colour::mid, 7));
    p.text(c.on ? "Engaged on the master" : "Bypassed", gx, y + 56, gw, 14, plain(c.on ? colour::text : colour::dim, 10, Weight::SemiBold));
    p.text(juce::String("Threshold ") + juce::String(int(std::lround(threshold))) + " dB", gx, y + 74, gw, 12, plain(colour::sec, 9));
    p.text(juce::String("Ratio ") + juce::String(ratio, 1) + ":1", gx, y + 88, gw, 12, plain(colour::sec, 9));
    p.text(c.color ? "Color: tape warmth" : "Color off", gx, y + 102, gw, 12, plain(colour::sec, 9));
}

void soft(Context& c, int key) {
    if (key == 0) { c.set_compressor(1, !c.project.compressor.color); c.notice(c.project.compressor.color ? "Color on" : "Color off"); }
    if (key == 2) { c.set_compressor(0, !c.project.compressor.on); c.notice(c.project.compressor.on ? "Compressor engaged" : "Compressor bypassed"); }
}
} // namespace

const PageSpec compressor_page {draw, play_pad, no_wheel, no_push, soft, [](Context&, int) {}};

} // namespace melodee
