// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/sample_data.h"
#include <cstdint>
#include <vector>

// A sample's own beats, for Chop's Beats type (engine/onsets.cpp): its tempo
// and where each beat falls, found from the same analysis as its hits.
namespace melodee {

struct BeatGrid {
    uint16_t tempo = 0;       // tenths of a BPM; 0: none (a one shot, a sound with no steady pulse)
    std::vector<uint32_t> at; // each beat's frame (44.1 kHz), on the hit nearest it within 30 ms
    uint32_t first = 0;       // the first beat: where the beats fall, within the sample's first beat
    uint32_t pulse_from = 0, pulse_to = 0; // where the steady beat starts and ends (its first and last beat)
    bool found() const { return tempo && at.size() >= 2; }
};

// The hits, as find_hits() (sample_data.h), and the sample's beats.
unsigned find_hits(const SampleData& sample, std::array<Hit, max_hits>& out, BeatGrid& beats);

} // namespace melodee
