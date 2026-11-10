// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

// The arpeggiator (and note repeat, which is the same with one key held):
// the keys held, played in turn at a rate, across octaves. It counts its own
// clock ticks (24 a quarter), so it plays whether the song runs or not.
// Audio thread only; no allocation.
namespace melodee {

enum class ArpMode : uint8_t { Up, Down, UpDown, Random, Order, Count };
inline const char* arp_mode_name(ArpMode m) {
    static const char* names[] {"Up", "Down", "Up-down", "Random", "Order"};
    return names[unsigned(m) % unsigned(ArpMode::Count)];
}
// Rates in ticks, slowest first, each straight rate followed by its triplet:
// 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T. Note Repeat's B1-B3 pick
// 1/8, 1/16, 1/32 and, with Shift, their triplets (MELODEE_PARITY.md).
constexpr unsigned arp_rates = 8;
inline unsigned arp_rate_ticks(unsigned rate) { static const unsigned t[] {24, 16, 12, 8, 6, 4, 3, 2}; return t[rate % arp_rates]; }
inline const char* arp_rate_name(unsigned rate) {
    static const char* n[] {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"};
    return n[rate % arp_rates];
}
inline bool arp_rate_triplet(unsigned rate) { return rate % 2 == 1; }
constexpr unsigned arp_default_rate = 4; // 1/16
// Swing on the repeats, as the project's swing moves the sequencer's off
// sixteenths: every second note of a straight rate comes late by this many
// ticks (fractional), so a pair of notes splits swing : 100 - swing. Triplets
// stay even. `step` counts the rate's notes from the bar.
inline double arp_swing_ticks(unsigned rate, unsigned step, unsigned swing) {
    if (arp_rate_triplet(rate) || step % 2 == 0 || swing <= 50) return 0;
    return arp_rate_ticks(rate) * (double(swing) - 50) / 50;
}

struct Arp {
    bool on = false;
    ArpMode mode = ArpMode::Up;
    unsigned rate = arp_default_rate, octaves = 1, gate = 60; // gate: percent of a step
    int top = 127;                             // the highest key: a kit's run past 127 (model/sampler.h)
    std::array<int, 16> held {}, order {};     // held sorted by pitch; order as pressed
    std::array<int, 16> velocity {};
    unsigned count = 0, position = 0;
    int sounding = -1;
    uint32_t seed = 7;

    void press(int key, int vel) {
        for (unsigned n = 0; n < count; ++n) if (order[n] == key) return;
        if (count == held.size()) return;
        order[count] = key; velocity[count] = vel; held[count] = key; ++count;
        std::sort(held.begin(), held.begin() + count);
    }
    void release(int key) {
        unsigned w = 0;
        for (unsigned n = 0; n < count; ++n) if (order[n] != key) { order[w] = order[n]; velocity[w] = velocity[n]; ++w; }
        count = w;
        for (unsigned n = 0; n < count; ++n) held[n] = order[n];
        std::sort(held.begin(), held.begin() + count);
    }
    void clear() { count = 0; position = 0; }
    // The next key to play, or -1 with nothing held.
    int next(int& vel) {
        if (!count) return -1;
        unsigned span = count * std::max(1u, octaves), at = 0;
        switch (mode) {
        case ArpMode::Down: at = span - 1 - position % span; break;
        case ArpMode::UpDown: { unsigned cycle = span > 1 ? 2 * span - 2 : 1, p = position % cycle; at = p < span ? p : cycle - p; break; }
        case ArpMode::Random: seed = seed * 1664525u + 1013904223u; at = (seed >> 8) % span; break;
        default: at = position % span; break;
        }
        ++position;
        unsigned note = at % count, octave = at / count;
        int key = (mode == ArpMode::Order ? order[note] : held[note]) + 12 * int(octave);
        vel = velocity[0];
        for (unsigned n = 0; n < count; ++n) if (order[n] == (mode == ArpMode::Order ? order[note] : held[note])) vel = velocity[n];
        return std::min(key, top);
    }
};

} // namespace melodee
