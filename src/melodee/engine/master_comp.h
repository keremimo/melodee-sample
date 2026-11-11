// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/compressor.h"
#include <atomic>

// The master compressor's DSP, written for sample-base (audio thread only):
// a stereo-linked feed-forward compressor whose AMOUNT lowers the threshold
// and raises the ratio together, with automatic makeup, and Color: a low
// band added back in parallel, a slow wobble in a short delay and a soft
// saturation, as a tape machine colours. With Sidechain the same detector
// listens to the key track instead, and duck() turns the rest of the mix
// down before the master chain; process() then leaves only Color.
namespace melodee {

class MasterCompressor {
public:
    void set(const MasterComp& settings);
    void process(float* left, float* right, unsigned frames);
    // Sidechain: the key's level (In Boost into the detector only) ducks left / right, no makeup.
    void duck(const float* key_left, const float* key_right, float* left, float* right, unsigned frames);
    bool keyed() const { return s.on && s.sidechain; }
    unsigned key() const { return s.key; }
    unsigned key_pad() const { return s.key_pad; } // MasterComp::whole_kit: the whole track
    float reduction_db() const { return shown.load(std::memory_order_relaxed); } // for the page's meter
    // For the page's picture (screen thread): the loudest the detector heard (dB, In Boost in) and the
    // most it took off since it last asked; both start again.
    void take_activity(float& level_db, float& reduce_db);
private:
    float envelope(float level);     // the envelope, attack up and release down
    float target(float reduce) const; // the gain for that many dB off (makeup only on the mix itself)
    void colour(float& l, float& r);
    void note(float env_most, float reduce_most); // a block's loudest and most taken off, for take_activity()
    MasterComp s;
    float attack = 0.99f, release = 0.999f, threshold = 0, ratio = 1, makeup = 1, boost = 1;
    float env = 0, low_l = 0, low_r = 0, wobble = 0, gain = 1, step = 0;
    static constexpr unsigned line = 512;
    float delay_l[line] {}, delay_r[line] {};
    unsigned at = 0;
    std::atomic<float> shown {0};
    std::atomic<float> heard_most {-90}, took_most {0}; // since the screen last asked
};

} // namespace melodee
