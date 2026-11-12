// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

// A multisampled instrument's zones as the screen draws them (Stage's ZONES
// and the key strip on its other pages, round 3 "instrument C"): each zone's
// keys and velocities, its root and its file, and how far each file got. No
// JUCE here, so the host tests build it.
namespace melodee {

struct ZoneShape {
    enum Flag : uint8_t { Release = 1, Switched = 2 };
    uint8_t low_key = 0, high_key = 127, low_velocity = 1, high_velocity = 127, root = 60;
    uint8_t flags = 0;
    uint8_t round = 0;  // round robin: its turn (1 up), 0 always
    uint16_t part = 0;  // its file, as LoadedSample::part_state counts them
    bool covers(int key, int velocity) const {
        return key >= low_key && key <= high_key && velocity >= low_velocity && velocity <= high_velocity;
    }
};

// What a file of the instrument has come to: still to be read, playing, or silent (missing, unreadable, no room).
enum class PartState : uint8_t { Waiting, Landed, Failed };

// The zone a press of `key` at `velocity` reaches first (release samples aside, the first turn of a round robin
// first), -1 none: the zone in hand follows the key played.
inline int zone_at(const std::vector<ZoneShape>& zones, int key, int velocity) {
    int best = -1;
    for (size_t n = 0; n < zones.size(); ++n) {
        const auto& z = zones[n];
        if ((z.flags & ZoneShape::Release) || !z.covers(key, std::clamp(velocity, 1, 127))) continue;
        if (best < 0 || (z.round && z.round < zones[size_t(best)].round)) best = int(n);
        if (z.round <= 1) break;
    }
    return best;
}

// Velocity layers: the most different velocity ranges any one key has (release samples aside), 1 at least.
inline unsigned zone_layers(const std::vector<ZoneShape>& zones) {
    unsigned most = 1;
    for (int key = 0; key < 128; ++key) {
        std::vector<uint16_t> ranges;
        for (const auto& z : zones) {
            if ((z.flags & ZoneShape::Release) || key < z.low_key || key > z.high_key) continue;
            uint16_t range = uint16_t(z.low_velocity << 8 | z.high_velocity);
            if (std::find(ranges.begin(), ranges.end(), range) == ranges.end()) ranges.push_back(range);
        }
        most = std::max(most, unsigned(ranges.size()));
    }
    return most;
}

} // namespace melodee
