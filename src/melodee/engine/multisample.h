// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/sampler.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// A multisampled instrument's zones: what sits on a kit pad in place of one
// region of one sample, as melodee-ios's SAMPLER zones. A press plays every
// zone its key and velocity reach, in round robin; the pad's own controls
// stay over all of them, each zone's filter and envelope being offsets from
// the pad's.
namespace melodee {

constexpr unsigned max_zones = 512;
// Streaming's unit: a window a side, and what stays in memory of each zone's
// start for a key to strike with no wait on the card (a third of a second).
constexpr uint32_t stream_chunk = 16384;
constexpr size_t stream_chunk_bytes = stream_chunk * sizeof(float); // 64 KB: whole pages, aligned in the laid out file
enum class ZoneLoop : uint8_t { Off, On, UntilReleased };

struct SampleZone {
    uint16_t part = 0;                // which of the instrument's audio files
    uint32_t start = 0, end = 0;      // frames of it, set as it lands; end 0: to the end
    uint8_t low_key = 0, high_key = 127, root = 60;
    uint8_t low_velocity = 1, high_velocity = 127;
    int16_t tune = 0;                 // cents over its root
    int16_t keytrack = 100;           // cents a key moves it (0: not at all)
    float gain = 0;                   // dB
    float pan = 0;                    // -1..1
    ZoneLoop loop = ZoneLoop::Off;
    bool one_shot = false;
    uint32_t loop_start = 0, loop_end = 0; // frames; loop_end past the last in the loop
    uint32_t sequence = 1, sequence_length = 0; // 1 up, of so many; 0: always
    float low_random = 0, high_random = 1;
    uint32_t group = 0, off_by = 0;   // a press of a group cuts what is off by it; 0 none
    // Its own filter (cutoff 0: the pad's alone): Hz, dB of resonance, cents at
    // full velocity and a key from its centre.
    PadFilter filter = PadFilter::Off;
    float cutoff = 0, resonance = 0;
    int16_t cutoff_velocity = 0, cutoff_key = 0;
    uint8_t cutoff_center = 60;
    // Its own envelope: each stage off the pad's, in the pad's 0..100 steps.
    int16_t attack = 0, decay = 0, sustain = 0, release = 0;
};

// One audio file of an instrument: its frames, mapped from the card's cache
// (or, in tests, held). `frames` stays 0 until it has landed, its zones' start,
// end and loop written before it, so the audio thread may read them after.
struct ZonePart {
    ZonePart() = default;
    ZonePart(const ZonePart&) = delete;
    ZonePart& operator=(const ZonePart&) = delete;
    ~ZonePart(); // the mapping goes, and its pins with it (engine/stream_pager.cpp)
    const float* left = nullptr;
    const float* right = nullptr;     // == left: one side
    std::atomic<uint32_t> frames {0};
    std::vector<uint16_t> pins;       // a count a chunk; the landing thread's, then the pager's
    void* mapping = nullptr;
    size_t mapping_bytes = 0;
};

struct Multisample {
    std::vector<SampleZone> zones;
    std::unique_ptr<ZonePart[]> parts;
    unsigned part_count = 0;
    std::vector<uint32_t> turns;      // the presses each zone has been reached by; audio thread only
};

} // namespace melodee
