// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cmath>
#include <cstdint>

// The master compressor (Shift + Pad 5, parity phase 7), as on the MPC:
// after the master chain, before main volume. Attack, release and amount
// are 0..100 on their cards; amount is threshold and ratio together, and
// makeup follows from them. Saved with the project; off in older files.
// Sidechain (B1): one track keys it instead of the mix, whichever track is
// selected. That track's sound (after its chain, muted or not) drives the
// detector, and everything else (the other tracks, the Looper, the buses'
// returns, Knob FX) ducks under it before the master chain, with no makeup;
// the key itself passes untouched. On a kit the key can be one pad (the
// kick): it leaves the kit after its own chain (skipping the kit filter and
// the track's chain, as Knob FX's pads do) and the rest of the kit ducks.
namespace melodee {

struct MasterComp {
    bool on = false;      // B3: engaged (off: bypassed)
    bool color = false;   // B2: a parallel low boost, a little wobble and saturation
    uint8_t attack = 30;  // 0.1 .. 150 ms
    uint8_t release = 45; // 3 .. 300 ms
    uint8_t amount = 50;  // 0 .. 100 %
    uint8_t boost = 0;    // Shift + K3, In Boost: 0 .. 24 dB into it (the key alone, with Sidechain)
    bool sidechain = false; // B1: keyed by `key` (ducking the rest) instead of the mix itself
    uint8_t key = 0;        // the key track, 0 .. 15
    uint8_t key_pad = whole_kit; // on a kit: the key pad, 0 .. 127, or the whole track
    static constexpr uint8_t whole_kit = 255;
};

inline float comp_attack_ms(int v) { return 0.1f * std::pow(1500.0f, std::fmin(std::fmax(v, 0), 100) / 100.0f); }
inline float comp_release_ms(int v) { return 3.0f * std::pow(100.0f, std::fmin(std::fmax(v, 0), 100) / 100.0f); }

} // namespace melodee
