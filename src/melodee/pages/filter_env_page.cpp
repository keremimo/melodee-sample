// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// FILTER ENV (7 of 10 sampler pages, the MPC's Filt Env): how the last
// pad's filter opens on each hit. K1 attack, K2 decay (One shot) or
// release (Note On), K3 depth (up to seven octaves over the cutoff);
// Shift + K2 decay from the start or the end; row 2 sustain and the other
// of decay and release.
#include "melodee/pages/sampler_common.h"
#include "melodee/ui/waveform.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    const auto& pad = sampler_pad(s);
    sampler_top(p, s, sampler_pad_pill(s), true);
    auto c = colour_of(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30, h = 78;
    if (!pad.used()) return;
    double length = 1.0;
    if (s.sample.frames) {
        uint32_t end = pad.end ? pad.end : s.sample.frames;
        length = (end - pad.start) / 44100.0;
        float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
        waveform(p, x, y, w, h, s.sample.overview, from, to, from, to, c.withAlpha(0.25f));
    }
    // The cutoff's own height, and the envelope on top of it scaled by the depth.
    float base = pad.cutoff / 100.0f, reach = std::min(1.0f - base, pad.filter_amount / 100.0f * 7.0f / 9.97f);
    float ey = y + h * (1 - base - reach), eh = std::max(4.0f, h * reach);
    p.fill(x, y + h * (1 - base), w, 1, colour::dim);
    bool gate = pad.play == PadPlay::Gate, from_end = !gate && !pad.decay_from_start;
    draw_envelope(p, x, ey, w, eh + 2, length, time_seconds(pad.filter_attack) * 0.3, time_seconds(pad.filter_decay), pad.filter_sustain / 100.0f,
                  time_seconds(pad.filter_release) * 0.5, from_end, s.ui.track);
    juce::String how = gate ? juce::String("NOTE ON: RELEASE") : from_end ? juce::String("DECAY FROM END") : juce::String("DECAY FROM START");
    juce::String type = pad.filter == PadFilter::Off ? juce::String("FILTER OFF") : juce::String(pad_filter_name(pad.filter)).toUpperCase();
    sampler_pills(p, y + h + 6, {how, "DEPTH " + juce::String(pad.filter_amount) + " %", type}, c);
}
} // namespace

const PageSpec filter_env_page {draw, play_pad, sampler_rows_wheel, no_push, sampler_soft, sampler_bank};

} // namespace melodee
