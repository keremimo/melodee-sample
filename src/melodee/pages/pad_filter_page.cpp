// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// FILTER (6 of 10 sampler pages, the MPC's Sample Mode Filter): the last
// pad's own filter. K1 cutoff, K2 resonance, K3 type: Off, Classic (a
// low pass that saturates as it rings, after the MPC3000), LPF2 / LPF4,
// HPF2 / HPF4, BPF2 / BPF4 (2: 12 dB an octave, 4: 24). The kit's filter
// is the Kit filter page's, after every pad.
#include "melodee/pages/sampler_common.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    const auto& pad = sampler_pad(s);
    sampler_top(p, s, sampler_pad_pill(s), false);
    auto c = colour_of(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30, h = 70;
    if (!pad.used()) return;
    float cutoff = 20.0f * std::exp2(pad.cutoff / 100.0f * 9.97f), res = pad.resonance / 100.0f;
    unsigned shape = 0, poles = 2;
    float q = 1 / std::max(0.05f, 1.414f - 1.364f * res);
    switch (pad.filter) {
    case PadFilter::Classic: q = 1 / std::max(0.03f, 2.0f - 1.97f * res); break;
    case PadFilter::Lpf4: poles = 4; break;
    case PadFilter::Hpf2: shape = 2; break;
    case PadFilter::Hpf4: shape = 2; poles = 4; break;
    case PadFilter::Bpf2: shape = 1; break;
    case PadFilter::Bpf4: shape = 1; poles = 4; break;
    default: break;
    }
    bool off = pad.filter == PadFilter::Off;
    draw_filter_curve(p, x, y, w, h, shape, poles, off ? 20000.0f : cutoff, off ? 0.7f : q, off ? colour::dim : c, knob_shade(s.ui.track, 0));
    juce::String type = off ? juce::String("FILTER OFF") : juce::String(pad_filter_name(pad.filter)).toUpperCase();
    juce::String env = pad.filter_amount ? "ENV OPENS " + juce::String(pad.filter_amount * 7 / 100.0, 1) + " OCT" : juce::String("NO ENVELOPE");
    sampler_pills(p, y + h + 16, {type, env, "THIS PAD ONLY"}, c);
}
} // namespace

const PageSpec pad_filter_page {draw, play_pad, sampler_wheel, sampler_push, sampler_soft, sampler_bank};

} // namespace melodee
