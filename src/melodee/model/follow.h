// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include <cstdint>

// Roadmap P5: what a pattern does once it has played a while (its follow action, a clip following on by itself),
// as melodee-ios's Stage clips do. A first action, and a second on the odds the first leaves; after so many bars of
// the time signature, or plays of itself at its track's speed. The audio thread brings what follows on the tick it is
// due (EngineHost::follow), while the patterns play on their own (not in the song, and not on a chained track: a
// chain is that track's order, see docs/ROADMAP.md, P5's decisions).
namespace melodee {

enum class FollowAction : uint8_t { None, Next, Previous, Any, Again, Stop, Count };

struct PatternFollow {
    uint8_t action = 0;  // FollowAction: the first
    uint8_t other = 0;   // and the second, on the odds the first leaves (None: the pattern plays on)
    uint8_t chance = 100; // the first's odds in percent, 0..100
    uint8_t count = 1;   // after so many, 1..64
    bool plays = false;  // of its own plays, else bars
    bool on() const { return action || other; }
    bool operator==(const PatternFollow& o) const {
        return action == o.action && other == o.other && chance == o.chance && count == o.count && plays == o.plays;
    }
    bool operator!=(const PatternFollow& o) const { return !(*this == o); }
};
constexpr unsigned follow_most = 64; // count's top

inline const char* follow_name(unsigned action) {
    static const char* const names[] {"OFF", "NEXT", "PREV", "ANY", "AGAIN", "STOP"};
    return names[action % unsigned(FollowAction::Count)];
}

// Dice that roll the same way from the same seed (SplitMix64), as iOS's, so what a follow action chose can be tested.
struct FollowDice {
    uint64_t state = 0x5eed;
    uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    unsigned below(unsigned n) { return n ? unsigned(next() % n) : 0; }
};

// The action this time: the first on its odds, else the second (None: neither, the pattern plays on).
inline unsigned follow_roll(const PatternFollow& f, FollowDice& dice) {
    if (f.chance >= 100 || dice.below(100) < f.chance) return f.action;
    return f.other;
}

// What comes, and after how many stretches of `after`: where a roll brings neither action the pattern plays on
// another stretch and the dice roll again. False when nothing ever can, or nothing came in 64 rolls.
inline bool follow_plan(const PatternFollow& f, FollowDice& dice, unsigned& action, unsigned& stretches) {
    if (!f.on() || (!f.chance && !f.other) || (f.chance >= 100 && !f.action)) return false;
    for (stretches = 1; stretches <= follow_most; ++stretches)
        if ((action = follow_roll(f, dice))) return true;
    return false;
}

// Where an action goes from pattern `from` among the track's patterns with notes (`filled`, a bit each): another
// pattern, the same (Again), or -1: the track stops. Next and Previous go round; Any is one of the others at random,
// the same when it is the only one. Nothing filled: it stops.
inline int follow_target(unsigned action, unsigned from, uint32_t filled, FollowDice& dice) {
    filled &= (1u << pattern_count) - 1;
    if (!filled || action == unsigned(FollowAction::Stop)) return -1;
    from %= pattern_count;
    switch (FollowAction(action)) {
    case FollowAction::Next:
        for (unsigned k = 1; k <= pattern_count; ++k) if (filled >> ((from + k) % pattern_count) & 1) return int((from + k) % pattern_count);
        break;
    case FollowAction::Previous:
        for (unsigned k = 1; k <= pattern_count; ++k) if (filled >> ((from + pattern_count - k) % pattern_count) & 1) return int((from + pattern_count - k) % pattern_count);
        break;
    case FollowAction::Any: {
        uint32_t others = filled & ~(1u << from);
        if (!others) return int(from);
        unsigned count = 0;
        for (unsigned p = 0; p < pattern_count; ++p) count += others >> p & 1;
        unsigned pick = dice.below(count);
        for (unsigned p = 0; p < pattern_count; ++p)
            if (others >> p & 1 && !pick--) return int(p);
        break;
    }
    default: break;
    }
    return int(from); // Again
}

// The ticks of the transport a pattern plays before its follow action comes: whole bars, or its own plays (`length`
// as it plays, its track's speed in it).
inline uint32_t follow_after(const PatternFollow& f, uint32_t length, uint32_t bar) {
    uint32_t count = f.count ? f.count : 1;
    return count * (f.plays ? (length ? length : 1) : (bar ? bar : 1));
}

} // namespace melodee
