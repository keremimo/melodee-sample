// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/waveform.h"
#include <algorithm>
#include <cmath>

namespace melodee::ui {

void waveform(Paint& p, float x, float y, float w, float h, const std::array<uint8_t, 512>& overview,
              float from, float to, float keep_from, float keep_to, juce::Colour colour, float bar) {
    waveform(p, x, y, w, h, overview, from, to, keep_from, keep_to, colour, nullptr, 0, bar);
}

void waveform(Paint& p, float x, float y, float w, float h, const std::array<uint8_t, 512>& overview,
              float from, float to, float keep_from, float keep_to, juce::Colour colour,
              const WaveHead* heads, unsigned count, float bar) {
    waveform(p, x, y, w, h, overview, from, to, keep_from, keep_to, colour, heads, count, WaveDetail {}, bar);
}

void waveform(Paint& p, float x, float y, float w, float h, const std::array<uint8_t, 512>& overview,
              float from, float to, float keep_from, float keep_to, juce::Colour colour,
              const WaveHead* heads, unsigned count, const WaveDetail& detail, float bar) {
    const bool fine = detail.peaks && detail.to > detail.from;
    float mid = y + h / 2, step = bar * 2;
    unsigned columns = unsigned(w / step);
    auto x_of = [&](float at) { return x + (at - from) / std::max(1e-6f, to - from) * w; };
    // The wakes' faint ground, under the bars.
    for (unsigned n = 0; n < count; ++n) {
        const auto& head = heads[n];
        if (head.wake <= 0 || !head.ground) continue;
        float a = std::clamp(x_of(std::min(head.at, head.wake_from)), x, x + w);
        float b = std::clamp(x_of(std::max(head.at, head.wake_from)), x, x + w);
        if (b > a) p.fill(a, y, b - a, h, colour.withAlpha(0.13f * head.wake));
    }
    const juce::Colour lit = colour.interpolatedWith(colour::text, 0.45f);
    for (unsigned c = 0; c < columns; ++c) {
        float at = from + (to - from) * (c + 0.5f) / columns; // where in the sample, 0..1
        float c0 = from + (to - from) * c / columns, c1 = from + (to - from) * (c + 1) / columns;
        float slack = (detail.to - detail.from) * 0.002f; // the same edges, worked out in floats twice
        const auto& peaks = fine && c0 >= detail.from - slack && c1 <= detail.to + slack ? *detail.peaks : overview;
        float p0 = &peaks == &overview ? 0.0f : detail.from, p1 = &peaks == &overview ? 1.0f : detail.to;
        unsigned a = unsigned(std::clamp((c0 - p0) / (p1 - p0), 0.0f, 1.0f) * 511);
        unsigned b = unsigned(std::clamp((c1 - p0) / (p1 - p0), 0.0f, 1.0f) * 511);
        uint8_t peak = 0;
        for (unsigned k = a; k <= std::max(a, b); ++k) peak = std::max(peak, peaks[k]);
        float half = std::max(0.5f, peak / 255.0f * h / 2);
        bool kept = at >= keep_from && at <= keep_to;
        juce::Colour shade = kept ? colour : colour.withAlpha(0.3f);
        if (kept) {
            float bx = x + c * step + bar / 2;
            float near = 0, wake = 0;
            for (unsigned n = 0; n < count; ++n) {
                const auto& head = heads[n];
                if (head.strength > 0.05f && std::fabs(bx - x_of(head.at)) < 2.2f) near = std::max(near, head.strength);
                if (head.wake > 0 && at >= std::min(head.at, head.wake_from) && at <= std::max(head.at, head.wake_from))
                    wake = std::max(wake, head.wake);
            }
            if (near > 0) shade = colour::text.withAlpha(std::max(0.6f, near));
            else if (wake > 0) shade = colour.interpolatedWith(lit, wake);
        }
        p.fill(x + c * step, mid - half, bar, half * 2, shade);
    }
}

void draw_heads(Paint& p, float x, float y, float w, float h, float from, float to, const WaveHead* heads, unsigned count, bool cap) {
    for (unsigned n = 0; n < count; ++n) {
        const auto& head = heads[n];
        if (head.strength <= 0.01f || head.at < from || head.at > to) continue;
        float hx = x + (head.at - from) / std::max(1e-6f, to - from) * w;
        auto c = colour::text.withAlpha(std::clamp(head.strength, 0.0f, 1.0f));
        p.fill(hx - 0.75f, y, 1.5f, h, c);
        if (cap) {
            juce::Path tip;
            tip.addTriangle(hx - 3.5f, y - 4, hx + 3.5f, y - 4, hx, y);
            p.g.setColour(c);
            p.g.fillPath(tip);
        }
    }
}

} // namespace melodee::ui
