// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "dsp/frame_ring.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// The base's SincResampler (dsp/frame_ring.h) for a ratio that never changes,
// as the plug-in rate bridge (plugin/rate_bridge.h) has: the same kernel, band,
// lag and output, with each output position's weights worked out once at
// reset() instead of for every frame. 44.1 to 48 kHz is 147 input frames to
// 160 output frames, so an output frame falls on one of 160 places between
// two input frames; each place's taps, already divided by their sum, are a
// row of the table, and a frame is then one row against the last input
// frames, four taps at a time. reset() allocates (not on the audio thread);
// feed() does not. A ratio whose table would be too big plays through a
// SincResampler instead.
namespace melodee {

class PolyphaseResampler {
public:
    static constexpr unsigned max_half = ::SincResampler<2>::max_half;
    static constexpr size_t max_table = size_t(1) << 17; // weights: 512 KB

    void reset(unsigned input_rate, unsigned output_rate) {
        slow.reset(input_rate, output_rate);
        table.clear();
        fallback = true;
        if (!input_rate || !output_rate) return;
        const unsigned g = std::gcd(input_rate, output_rate);
        step = input_rate / g;  // the phase moves this many 1/places an output frame
        places = output_rate / g;
        // As SincResampler::reset(): the band follows the lower rate, the kernel stretches converting down.
        const double base = double(input_rate) / output_rate;
        const double band = std::max(std::min(1.0, 1.0 / base), double(SincKernel::zero_crossings) / max_half);
        half = std::min(unsigned(std::ceil(SincKernel::zero_crossings / band)), max_half);
        const unsigned taps = 2 * half;
        width = (taps + 3) & ~3u; // whole groups of four; the extra taps (the oldest frames) weigh nothing
        if (size_t(places) * width > max_table) return;
        fallback = false;
        const SincKernel::Point* kernel = SincKernel::table();
        const double scale = band * SincKernel::steps;
        table.assign(size_t(places) * width, 0.0f);
        std::vector<float> row(taps);
        for (unsigned p = 0; p < places; ++p) {
            const double phase = double(p) / places;
            const double first = (half - 1 + phase) * scale;
            float sum = 0;
            for (unsigned j = 0; j < taps; ++j) {
                double at = std::abs(first - j * scale);
                unsigned point = unsigned(at);
                row[j] = kernel[point].value + float(at - point) * kernel[point].slope;
                sum += row[j];
            }
            float* out = table.data() + size_t(p) * width + (width - taps);
            for (unsigned j = 0; j < taps; ++j) out[j] = row[j] / sum;
        }
        place = 0; newest = 0; pending = half; primed = false;
    }

    // As SincResampler::feed(): `emit(left, right)` for each converted frame.
    template <class Emit> void feed(float left, float right, Emit&& emit) {
        if (fallback) { slow.feed(left, right, emit); return; }
        if (!primed) {
            std::fill(hl.begin(), hl.end(), left);
            std::fill(hr.begin(), hr.end(), right);
            newest = 0; primed = true;
            return;
        }
        newest = (newest + 1) & (history - 1);
        hl[newest] = hl[newest + history] = left;
        hr[newest] = hr[newest + history] = right;
        if (pending > 1) { --pending; return; }
        pending = 0;
        const float* wl = hl.data() + (newest + history + 1 - width);
        const float* wr = hr.data() + (newest + history + 1 - width);
        while (place < places) {
            const float* w = table.data() + size_t(place) * width;
            float a, b;
            dot(w, wl, wr, a, b);
            emit(a, b);
            place += step;
        }
        place -= places;
    }

private:
    void dot(const float* w, const float* l, const float* r, float& a, float& b) const {
#if defined(__ARM_NEON)
        float32x4_t sl = vdupq_n_f32(0), sr = vdupq_n_f32(0);
        for (unsigned j = 0; j < width; j += 4) {
            const float32x4_t k = vld1q_f32(w + j);
            sl = vfmaq_f32(sl, k, vld1q_f32(l + j));
            sr = vfmaq_f32(sr, k, vld1q_f32(r + j));
        }
        a = vaddvq_f32(sl);
        b = vaddvq_f32(sr);
#else
        float s[8] {};
        for (unsigned j = 0; j < width; j += 4)
            for (unsigned k = 0; k < 4; ++k) { s[k] += w[j + k] * l[j + k]; s[4 + k] += w[j + k] * r[j + k]; }
        a = (s[0] + s[1]) + (s[2] + s[3]);
        b = (s[4] + s[5]) + (s[6] + s[7]);
#endif
    }

    // Each channel's last input frames twice over, so the last `width` always lie in a row.
    static constexpr unsigned history = 2 * max_half;
    std::array<float, 2 * history> hl {}, hr {};
    std::vector<float> table;  // places rows of width weights
    unsigned step = 1, places = 1, place = 0, half = 0, width = 0, newest = 0, pending = 0;
    bool primed = false, fallback = true;
    ::SincResampler<2> slow;   // a ratio too fine for a table
};

} // namespace melodee
