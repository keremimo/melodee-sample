// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/waveform.h"
#include <algorithm>

namespace melodee::ui {

void waveform(Paint& p, float x, float y, float w, float h, const std::array<uint8_t, 512>& overview,
              float from, float to, float keep_from, float keep_to, juce::Colour colour, float bar) {
    float mid = y + h / 2, step = bar * 2;
    unsigned columns = unsigned(w / step);
    for (unsigned c = 0; c < columns; ++c) {
        float at = from + (to - from) * (c + 0.5f) / columns; // where in the sample, 0..1
        unsigned a = unsigned(std::clamp(from + (to - from) * c / columns, 0.0f, 1.0f) * 511);
        unsigned b = unsigned(std::clamp(from + (to - from) * (c + 1) / columns, 0.0f, 1.0f) * 511);
        uint8_t peak = 0;
        for (unsigned k = a; k <= std::max(a, b); ++k) peak = std::max(peak, overview[k]);
        float half = std::max(0.5f, peak / 255.0f * h / 2);
        bool kept = at >= keep_from && at <= keep_to;
        p.fill(x + c * step, mid - half, bar, half * 2, kept ? colour : colour.withAlpha(0.3f));
    }
}

} // namespace melodee::ui
