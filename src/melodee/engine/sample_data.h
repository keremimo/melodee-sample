// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// A sample in memory at 44.1 kHz, and what the screen needs about it. Read,
// converted and analysed on the service thread (it allocates and reads
// files); the audio thread only reads `left`, `right` and `frames` through a
// pointer handed over by the engine host.
namespace melodee {

struct SampleData {
    std::vector<float> left, right;
    uint32_t frames = 0;
};

// The waveform as the pages draw it: the peak of each of 512 columns, 0..255.
using Overview = std::array<uint8_t, 512>;
// Where the hits are, for chopping: a frame and how strong the rise was, 0..255.
struct Hit { uint32_t frame = 0; uint8_t strength = 0; };
constexpr unsigned max_hits = 128;

// Reads a WAV file (PCM 16, 24 or 32 bit, or 32-bit float; mono or stereo;
// any rate, converted to 44.1 kHz) of at most `max_seconds`. Returns an
// empty message on success, else what went wrong.
std::string read_wav(const std::string& path, SampleData& out, unsigned max_seconds = 60);
void make_overview(const SampleData& sample, Overview& out);
// The hits, strongest kept, sorted by frame; the first is always frame 0.
unsigned find_hits(const SampleData& sample, std::array<Hit, max_hits>& out);

} // namespace melodee
