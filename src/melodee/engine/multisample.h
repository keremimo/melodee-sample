// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/sampler.h"
#include <cstdint>

// A multisampled instrument's zones: what sits on a kit pad in place of one
// region of one sample, as melodee-ios's SAMPLER zones. A press plays every
// zone its key and velocity reach, in round robin; the pad's own controls
// stay over all of them, each zone's filter and envelope being offsets from
// the pad's.
namespace melodee {

constexpr unsigned max_zones = 512;
enum class ZoneLoop : uint8_t { Off, On, UntilReleased };

struct SampleZone {
    uint16_t part = 0;                // which of the instrument's audio files
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

} // namespace melodee
