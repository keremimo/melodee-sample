// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/sampler.h"
#include <atomic>
#include <cmath>
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
    // Roadmap P3. A release sample (SFZ trigger=release): it plays when its key is let go, at the press's
    // velocity, `rt_decay` dB quieter a second the key was held, and is not played by the press itself.
    bool on_release = false;
    float rt_decay = 0;
    // Crossfades (xfin_* / xfout_*): over these keys and velocities it fades in, then out; a pair equal:
    // none. `power`: equal power (the format's default), else straight gain.
    uint8_t xfin_lokey = 0, xfin_hikey = 0, xfout_lokey = 127, xfout_hikey = 127;
    uint8_t xfin_lovel = 0, xfin_hivel = 0, xfout_lovel = 127, xfout_hivel = 127;
    bool key_power = true, vel_power = true;
    // A keyswitch's (sw_last): it plays while that key was the last of the instrument's keyswitches pressed; -1 always.
    int16_t switch_key = -1;
};

// How loud a zone's crossfades leave it at `key` and `velocity`, 0..1.
inline float zone_crossfade(const SampleZone& z, int key, int velocity) {
    auto ramp = [](int at, int lo, int hi, bool rising) { // 0..1 across lo..hi; outside it what lies beyond
        if (hi <= lo) return 1.0f;
        if (at <= lo) return rising ? 0.0f : 1.0f;
        if (at >= hi) return rising ? 1.0f : 0.0f;
        float x = float(at - lo) / float(hi - lo);
        return rising ? x : 1.0f - x;
    };
    auto curve = [](float x, bool power) { return power ? std::sqrt(x) : x; };
    float k = ramp(key, z.xfin_lokey, z.xfin_hikey, true) * ramp(key, z.xfout_lokey, z.xfout_hikey, false);
    float v = ramp(velocity, z.xfin_lovel, z.xfin_hivel, true) * ramp(velocity, z.xfout_lovel, z.xfout_hivel, false);
    return curve(k, z.key_power) * curve(v, z.vel_power);
}

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
    uint32_t total = 0;               // its length, known before it is published
    std::atomic<uint32_t> frames {0}; // its length once landed: what the audio thread goes by
    std::vector<uint16_t> pins;       // a count a chunk; the landing thread's, then the pager's
    void* mapping = nullptr;
    size_t mapping_bytes = 0;
};

struct Multisample {
    std::vector<SampleZone> zones;
    std::unique_ptr<ZonePart[]> parts;
    unsigned part_count = 0;
    std::vector<uint32_t> turns;      // the presses each zone has been reached by; audio thread only
    // Keyswitches (roadmap P3, SFZ sw_lokey / sw_hikey / sw_default): a press in low..high picks the zones
    // whose switch_key it is and sounds nothing of itself; -1 none. `switched`: the last pressed, audio thread only.
    int16_t switch_low = -1, switch_high = -1, switch_default = -1, switched = -1;
    bool has_release = false;         // a zone plays on a key let go
};

} // namespace melodee
