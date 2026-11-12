// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include <array>
#include <cstdint>
#include <vector>

// Generators (roadmap P1): a Euclidean rhythm on a kit lane, and a pattern's
// variations. Plain functions over patterns; the shell writes what they make
// as one undo.
namespace melodee {

// `hits` spread as evenly as they go over `steps` (Bjorklund's pairing, as Toussaint found it in the world's rhythms),
// turned `rotate` steps later: E(3, 8) is x..x..x., E(5, 8) x.xx.xx. 64 steps at most.
std::vector<bool> euclid(unsigned hits, unsigned steps, unsigned rotate = 0);
constexpr unsigned euclid_max_steps = 64;
// The rhythm laid on lane `key` of a kit pattern, a step of Q each, as notes (they can be edited after): the lane's
// notes go, its hits come at `velocity`, and a rhythm shorter than the pattern loops on its own (the lane's length);
// kept in Pattern::euclids so the cards show it. False when the pattern is full.
bool lay_euclid(Pattern& p, uint8_t key, unsigned hits, unsigned steps, unsigned rotate, uint8_t velocity = 100);
const EuclidLane* euclid_of(const Pattern& p, unsigned key);

// A pattern's variations, made from its notes (those past its end left out), each into a new pattern: thinner (the
// weakest notes go), busier (hits beside a kit lane's hits, passing notes between a line's), humanised (each note a
// little early or late and softer or harder), reversed (played backwards, on the grid), an octave up or down (not on
// a kit, whose keys are pads). `amount` 0..1 says how far the first three go; `seed` rolls their dice.
enum class Variation : uint8_t { Thinner, Busier, Humanised, Reversed, OctaveUp, OctaveDown, Count };
const char* variation_name(Variation v);
inline bool variation_on_kit(Variation v) { return v != Variation::OctaveUp && v != Variation::OctaveDown; }
struct VaryPlace {
    bool kit = false;           // a kit: lanes of pads, not pitches
    unsigned bar = 384, beat = 96;
    unsigned root = 0;          // the project's key: its root and its scale's degrees (model/scales.h)
    std::array<uint8_t, 12> degrees {};
    unsigned degree_count = 7;
};
Pattern vary_pattern(const Pattern& p, Variation v, double amount, uint64_t seed, const VaryPlace& place);

} // namespace melodee
