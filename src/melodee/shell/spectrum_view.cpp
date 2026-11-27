// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/spectrum_view.h"
#include "melodee/engine/fft.h"
#include <algorithm>

namespace melodee {

SpectrumView::SpectrumView() : frames(spectrum_points), window(spectrum_points), bins(spectrum_points), twiddle(spectrum_points / 2) {
    constexpr float pi = 3.14159265358979f;
    for (unsigned n = 0; n < spectrum_points; ++n) window[n] = 0.5f - 0.5f * std::cos(2 * pi * float(n) / float(spectrum_points)); // Hann
    for (unsigned k = 0; k < spectrum_points / 2; ++k) twiddle[k] = std::polar(1.0f, -2 * pi * float(k) / float(spectrum_points));
    clear();
}

void SpectrumView::clear() {
    db.fill(spectrum_floor_db);
    seen = false;
}

void SpectrumView::update(const ScopeRing& ring, float rate, uint32_t now) {
    const float since = seen ? float(now - last_at) / 1000.0f : 0.0f;
    const float fall = spectrum_fall_db_per_s * std::min(since, 0.5f);
    last_at = now;
    seen = true;
    const uint32_t written = ring.written();
    const bool fresh = written != last_written;
    last_written = written;
    std::array<float, spectrum_bands> heard;
    heard.fill(spectrum_floor_db);
    if (fresh && rate > 0) {
        ring.latest(frames.data(), spectrum_points);
        for (unsigned n = 0; n < spectrum_points; ++n) bins[n] = {frames[n] * window[n], 0.0f};
        fft(bins, twiddle);
        // A full-scale sine through a Hann window peaks at N/4 in its bin: that is 0 dB.
        const float scale = 4.0f / float(spectrum_points), hz_per_bin = rate / float(spectrum_points);
        const unsigned top = spectrum_points / 2 - 1;
        for (unsigned b = 0; b < spectrum_bands; ++b) {
            const float lo = spectrum_band_hz(float(b)), hi = spectrum_band_hz(float(b + 1));
            unsigned k0 = unsigned(std::lround(lo / hz_per_bin)), k1 = unsigned(std::lround(hi / hz_per_bin));
            k0 = std::clamp(k0, 1u, top); k1 = std::clamp(std::max(k1, k0), 1u, top); // a band narrower than a bin: its nearest
            float most = 0;
            for (unsigned k = k0; k <= k1; ++k) most = std::max(most, std::abs(bins[k]));
            const float level = most * scale;
            heard[b] = level > 1e-6f ? std::max(spectrum_floor_db, 20 * std::log10(level)) : spectrum_floor_db;
        }
    }
    for (unsigned b = 0; b < spectrum_bands; ++b) db[b] = std::max(heard[b], std::max(spectrum_floor_db, db[b] - fall));
}

} // namespace melodee
