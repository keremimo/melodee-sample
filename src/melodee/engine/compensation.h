// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

// Roadmap P4, plug-in delay compensation: the delay lines that hold back the
// paths that are early, so every path meets the others where they sum. Made
// once, at the engine host's start: a change of latency only moves where a
// line reads (eased across one block), never allocates. Audio thread (and the
// render workers, each line on one job only).
namespace melodee {

class CompensationLines {
public:
    static constexpr unsigned most = 16384; // frames a line can hold back: 170 ms at 96 kHz, 371 at 44.1
    static constexpr unsigned mask = most - 1;
    // The lines: one a track, a loop track and a bus; Knob FX's return; the direct mix into the master, the
    // Sidechain's key track as it joins after the ducking and as the detector hears it; a kit's pads into Knob FX
    // and the Sidechain's key pad (both leave their track before its chain).
    enum : unsigned { track0 = 0, loop0 = 16, bus0 = 20, knob = 24, direct = 25, aside = 26, key = 27, knob_pads = 28, key_pad = 29, count = 30 };

    CompensationLines() : memory(new float[size_t(count) * 2 * most]()) {}

    // Line `n` on `frames` frames of left / right, in place, `delay` frames late. A delay that changed moves across
    // this block, the old reading fading into the new; a line not run in the block before starts from silence.
    // `block` counts the host's blocks (a line knows it was skipped).
    void run(unsigned n, float* left, float* right, unsigned frames, unsigned delay, uint64_t block) {
        if (n >= count || !frames) return;
        auto& line = lines[n];
        float* l = memory.get() + size_t(n) * 2 * most;
        float* r = l + most;
        if (line.block + 1 != block) { // idle since: what it holds is old
            std::memset(l, 0, sizeof(float) * most); std::memset(r, 0, sizeof(float) * most);
            line.delay = delay;
        }
        line.block = block;
        delay = std::min(delay, most - frames);
        for (unsigned f = 0; f < frames; ++f) { l[(line.at + f) & mask] = left[f]; r[(line.at + f) & mask] = right[f]; }
        unsigned from = line.delay;
        line.delay = delay;
        if (delay == 0 && from == 0) { line.at += frames; return; }
        float step = from == delay ? 0.0f : 1.0f / float(frames);
        for (unsigned f = 0; f < frames; ++f) {
            unsigned now = (line.at + f - delay) & mask;
            float a = l[now], b = r[now];
            if (step != 0) {
                unsigned was = (line.at + f - from) & mask;
                float w = step * float(f + 1);
                a = l[was] + (a - l[was]) * w; b = r[was] + (b - r[was]) * w;
            }
            left[f] = a; right[f] = b;
        }
        line.at += frames;
    }
    unsigned delay(unsigned n) const { return n < count ? lines[n].delay : 0; }

private:
    struct Line { unsigned at = 0, delay = 0; uint64_t block = ~uint64_t(0) - 1; };
    std::unique_ptr<float[]> memory;
    std::array<Line, count> lines {};
};

} // namespace melodee
