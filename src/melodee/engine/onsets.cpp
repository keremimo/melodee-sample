// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/sample_data.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

// Hits for Chop (find_hits): where a new sound starts, found from the change
// in the spectrum rather than in the overall level, so a chord change at the
// same loudness or a hi-hat over a held pad counts too. The SuperFlux method
// (Böck and Widmer, 2013): a log spectrum in bands a semitone wide, 100 frames
// a second; each band's rise over the frame before, that frame first widened
// by one band either side so a vibrato's slide is not counted as a rise. A hit
// is a peak of the summed rises, the highest within 30 ms and above the
// average around it. Its cut is then moved back to the quiet point just before
// the attack, in 1 ms steps, so the slice holds the whole attack; Zero Snap
// (shell/chop.h) puts it on a crossing after that.
namespace melodee {
namespace {

using Complex = std::complex<float>;
constexpr unsigned fft_size = 2048, hop = 441; // 46 ms windows, 100 frames a second (at 44.1 kHz)
constexpr unsigned fine = 44;                  // the attack search's steps: 1 ms
constexpr unsigned spacing = 3;                // frames: hits at least 30 ms apart
constexpr float delta = 1.0f;                  // how far a peak stands above the average around it

// In-place radix-2 FFT of a.size() (a power of two) points; twiddle[k] = e^(-2πik/n), k < n/2.
void fft(std::vector<Complex>& a, const std::vector<Complex>& twiddle) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t half = len / 2, step = n / len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < half; ++k) {
                Complex u = a[i + k], v = a[i + k + half] * twiddle[k * step];
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
    }
}

// Semitone bands from 30 Hz to 16 kHz as triangles over the FFT's bins; low
// bands that would share a bin merge, so each band has a bin of its own.
struct Band { unsigned from, peak, to; };
std::vector<Band> make_bands() {
    std::vector<unsigned> centres;
    for (double f = 30; f <= 16000; f *= std::pow(2.0, 1.0 / 12)) {
        unsigned bin = unsigned(std::lround(f * fft_size / 44100));
        if (centres.empty() || bin > centres.back()) centres.push_back(bin);
    }
    std::vector<Band> bands;
    for (size_t n = 1; n + 1 < centres.size(); ++n) bands.push_back({centres[n - 1], centres[n], centres[n + 1]});
    return bands;
}

float band_sum(const std::vector<float>& magnitude, const Band& b) {
    float sum = 0;
    for (unsigned k = b.from; k < b.to; ++k)
        sum += magnitude[k] * (k < b.peak ? float(k - b.from) / float(b.peak - b.from) : float(b.to - k) / float(b.to - b.peak));
    return sum;
}

} // namespace

unsigned find_hits(const SampleData& s, std::array<Hit, max_hits>& out) {
    const uint32_t frames = std::min<uint32_t>(s.frames, uint32_t(std::min(s.left.size(), s.right.size())));
    std::vector<Hit> found;
    found.push_back({0, 255});
    if (frames > fine * 4) {
        std::vector<float> mono(frames); // the two sides summed
        for (uint32_t n = 0; n < frames; ++n) mono[n] = s.left[n] + s.right[n];

        // The bands of frames centred hop apart, two frames an FFT: one as the real part, one as the
        // imaginary, parted again by the spectrum's symmetry.
        // Frames whose window runs past the end are left out: the sound cut off there would count as a hit.
        const unsigned count = frames > fft_size / 2 ? (frames - fft_size / 2) / hop + 1 : 0;
        std::vector<Complex> twiddle(fft_size / 2), buffer(fft_size);
        for (unsigned k = 0; k < fft_size / 2; ++k) twiddle[k] = std::polar(1.0f, float(-2 * M_PI * k / fft_size));
        std::vector<float> window(fft_size);
        for (unsigned n = 0; n < fft_size; ++n) window[n] = float(0.5 - 0.5 * std::cos(2 * M_PI * n / fft_size));
        const auto bands = make_bands();
        const size_t width = bands.size();
        std::vector<float> magnitude[2], sums(size_t(count) * width), flux(count, 0.0f);
        magnitude[0].resize(fft_size / 2 + 1);
        magnitude[1].resize(fft_size / 2 + 1);
        auto sample = [&](unsigned frame, unsigned n) {
            int64_t at = int64_t(frame) * hop - fft_size / 2 + n;
            return at >= 0 ? mono[size_t(at)] * window[n] : 0.0f;
        };
        for (unsigned frame = 0; frame < count; frame += 2) {
            for (unsigned n = 0; n < fft_size; ++n) buffer[n] = {sample(frame, n), frame + 1 < count ? sample(frame + 1, n) : 0.0f};
            fft(buffer, twiddle);
            for (unsigned k = 0; k <= fft_size / 2; ++k) {
                Complex z = buffer[k], mirror = std::conj(buffer[(fft_size - k) % fft_size]);
                magnitude[0][k] = std::abs(z + mirror) / 2;
                magnitude[1][k] = std::abs(z - mirror) / 2;
            }
            for (unsigned side = 0; side < 2 && frame + side < count; ++side)
                for (size_t b = 0; b < width; ++b) sums[(frame + side) * width + b] = band_sum(magnitude[side], bands[b]);
        }

        // The log spectrum, 0 at 60 dB under the sample's loudest band, so a quiet sample finds the hits a loud one
        // does and the faint leakage of a clean tone (it changes with the tone's phase) is not heard as a rise. The
        // flux: each band's rise over the frame before, that frame widened by one band either side.
        const float floor = std::max(1e-9f, sums.empty() ? 0.0f : *std::max_element(sums.begin(), sums.end()) * 1e-3f);
        for (float& v : sums) v = std::log10(1.0f + v / floor);
        for (unsigned t = 1; t < count; ++t) {
            const float *level = &sums[t * width], *before = &sums[(t - 1) * width];
            float rise = 0;
            for (size_t b = 0; b < width; ++b) {
                float widened = std::max({before[b], before[b ? b - 1 : b], before[b + 1 < width ? b + 1 : b]});
                rise += std::max(0.0f, level[b] - widened);
            }
            flux[t] = rise;
        }

        // Peaks: the highest within 30 ms either side, above the average from 100 ms before to 70 ms after. Strength
        // is the height above that average, so a busy texture's bumps stay weak beside a clear hit.
        float loudest = 0;
        std::vector<unsigned> peaks;
        std::vector<float> height; // how far each stands above the average: its strength, against the highest
        for (unsigned t = 1; t < count; ++t) {
            unsigned lo = t > spacing ? t - spacing : 0, hi = std::min(count - 1, t + spacing);
            if (flux[t] <= 0 || *std::max_element(flux.begin() + lo, flux.begin() + hi + 1) > flux[t]) continue;
            if (t > lo && flux[t - 1] == flux[t]) continue; // a flat top counts once
            unsigned from = t > 10 ? t - 10 : 0, to = std::min(count - 1, t + 7);
            float average = 0;
            for (unsigned u = from; u <= to; ++u) average += flux[u];
            average /= float(to - from + 1);
            if (flux[t] < average + delta) continue;
            peaks.push_back(t);
            height.push_back(flux[t] - average);
            loudest = std::max(loudest, flux[t] - average);
        }

        // Backtrack: in 1 ms steps of the sound's edges (its first difference, so a hat over a held pad shows), the
        // steepest rise from 10 ms before the frame's centre to 25 ms after (a frame peaks as a sharp attack
        // enters its window, about 10 ms early). It counts when the attack (the loudest edge in the 5 ms from it)
        // is twice the loudest edge from 30 ms to 8 ms before; the cut then goes back down the rise until the edges
        // stop falling or reach the background (their middle level there), at most 20 ms, and lands where the rise
        // starts. A peak with no clear rise (a chord change at the same loudness, a beating chord) keeps the
        // frame's centre: a cut a little early keeps the attack, a late one loses it.
        const uint32_t steps = frames / fine;
        std::vector<float> edge(steps);
        for (uint32_t j = 0; j < steps; ++j) {
            float sum = 0;
            for (uint32_t n = j * fine; n < (j + 1) * fine; ++n) {
                float d = n ? mono[n] - mono[n - 1] : mono[n];
                sum += d * d;
            }
            edge[j] = std::sqrt(sum / fine);
        }
        std::vector<float> around;
        for (size_t p = 0; p < peaks.size(); ++p) {
            const unsigned t = peaks[p];
            uint32_t centre = t * hop, cut = centre;
            int64_t lo = std::max<int64_t>(1, int64_t(centre / fine) - 10), hi = std::min<int64_t>(int64_t(steps) - 1, centre / fine + 25);
            int64_t steepest = -1;
            float most = 0;
            for (int64_t j = lo; j <= hi; ++j)
                if (edge[size_t(j)] - edge[size_t(j - 1)] > most) { most = edge[size_t(j)] - edge[size_t(j - 1)]; steepest = j; }
            if (steepest > 0) {
                around.assign(edge.begin() + std::max<int64_t>(0, steepest - 30), edge.begin() + std::max<int64_t>(0, steepest - 8));
                float background = 0, loudest_before = 0, attack = 0;
                if (!around.empty()) {
                    loudest_before = *std::max_element(around.begin(), around.end());
                    std::nth_element(around.begin(), around.begin() + around.size() / 2, around.end());
                    background = around[around.size() / 2];
                }
                for (int64_t j = steepest; j <= std::min<int64_t>(int64_t(steps) - 1, steepest + 5); ++j) attack = std::max(attack, edge[size_t(j)]);
                if (attack >= 2 * loudest_before) {
                    int64_t j = steepest - 1, floor = std::max<int64_t>(0, steepest - 21);
                    while (j > floor && edge[size_t(j - 1)] < edge[size_t(j)] && edge[size_t(j)] > background) --j;
                    cut = uint32_t(j + 1) * fine;
                }
            }
            if (cut <= found.back().frame + spacing * hop) continue; // too near the hit before (or the start)
            found.push_back({cut, uint8_t(std::max(1L, std::lround(255 * height[p] / loudest)))});
        }
    }
    if (found.size() > out.size()) { // keep the strongest, then put them back in time order
        std::sort(found.begin() + 1, found.end(), [](const Hit& a, const Hit& b) { return a.strength > b.strength; });
        found.resize(out.size());
        std::sort(found.begin(), found.end(), [](const Hit& a, const Hit& b) { return a.frame < b.frame; });
    }
    std::copy(found.begin(), found.end(), out.begin());
    return unsigned(found.size());
}

} // namespace melodee
