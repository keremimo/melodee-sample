// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MIX (2 of 10 sampler pages, the MPC's Sample Mode Mix): the last pad's
// volume (K1, in dB: -inf, -74 to +6) with its meter on the left, its pan
// (K3) on the right, and Shift + K2 the kit's volume, the same scale.
#include "melodee/engine/sampler.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/ui/waveform.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    sampler_top(p, s, sampler_pad_pill(s), false);
    const auto& pad = sampler_pad(s);
    auto c = colour_of(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30, h = 80;
    if (!pad.used()) { p.text("No sample on this pad", x, y + 20, w, 20, plain(colour::dim, type::small, Weight::Medium), juce::Justification::centred); return; }
    // The volume meter: -74 dB at the foot, +6 at the top, 0 dB marked.
    auto meter_y = [&](int level) {
        if (level <= 0) return y + h;
        return y + h - (volume_db(level) - level_floor_db) / (level_top_db - level_floor_db) * h;
    };
    p.fill(x, y, 6, h, colour::surf, 2);
    float top = meter_y(pad.level);
    p.fill(x, top, 6, y + h - top, knob_shade(s.ui.track, 0), 2);
    p.fill(x - 2, meter_y(100), 10, 1, colour::text);
    // The region, its height following the volume.
    if (s.sample.frames) {
        uint32_t end = pad.end ? pad.end : s.sample.frames;
        float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
        float g = std::min(1.0f, volume_gain(pad.level) * volume_gain(s.track().params[sampler::Level]) * std::pow(10.0f, pad.gain / 2000.0f));
        float wh = std::max(4.0f, h * g);
        ui::WaveHead head; // the pad as it is heard: a head and its wake
        bool heard = pad_head(s, s.track().last_pad % sampler_pads, pad, s.sample.frames, head);
        waveform(p, x + 14, y + (h - wh) / 2, w - 42, wh, s.sample.overview, from, to, from, to, c, &head, heard ? 1 : 0);
        if (heard) draw_heads(p, x + 14, y + (h - wh) / 2, w - 42, wh, from, to, &head, 1);
    }
    // The pan meter: a line with the centre marked.
    float px = x + w - 20, pw = 20, py = y + h / 2;
    p.fill(px, py, pw, 1, colour::line);
    p.fill(px + pw / 2, py - 4, 1, 9, colour::dim);
    p.fill(px + pw * (pad.pan + 50) / 100.0f - 1.5f, py - 6, 3, 13, knob_shade(s.ui.track, 2), 1.5f);
    auto db = [](int level) { auto t = decibels(level); return juce::String(t) + (level > 0 ? " DB" : ""); };
    juce::String pan = pad.pan == 0 ? juce::String("CENTRE") : (pad.pan < 0 ? "L" : "R") + juce::String(std::abs(pad.pan));
    sampler_pills(p, y + h + 8, {"VOLUME " + db(pad.level), "KIT " + db(s.track().params[sampler::Level]), "PAN " + pan}, c);
}
} // namespace

const PageSpec pad_mix_page {draw, play_pad, sampler_wheel, sampler_push, sampler_soft, sampler_bank};

} // namespace melodee
