// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/model/generators.h"
#include <algorithm>
#include <cmath>

namespace melodee {

std::vector<bool> euclid(unsigned hits, unsigned steps, unsigned rotate) {
    steps = std::clamp(steps, 1u, euclid_max_steps);
    hits = std::min(hits, steps);
    // Bjorklund: the hits' groups take the rests' one at a time, until one group (or none) of what is left is over.
    std::vector<bool> out(steps, hits == steps);
    if (hits == 0 || hits == steps) return out;
    std::vector<std::vector<bool>> a(hits, std::vector<bool> {true}), b(steps - hits, std::vector<bool> {false});
    do { // at least once, as Bjorklund's
        size_t m = std::min(a.size(), b.size());
        std::vector<std::vector<bool>> paired, left;
        for (size_t n = 0; n < m; ++n) { auto g = a[n]; g.insert(g.end(), b[n].begin(), b[n].end()); paired.push_back(g); }
        if (a.size() > m) left.assign(a.begin() + long(m), a.end());
        else left.assign(b.begin() + long(m), b.end());
        a = paired; b = left;
    } while (b.size() > 1);
    std::vector<bool> spread;
    for (const auto& g : a) spread.insert(spread.end(), g.begin(), g.end());
    for (const auto& g : b) spread.insert(spread.end(), g.begin(), g.end());
    for (unsigned n = 0; n < steps; ++n) out[(n + rotate) % steps] = spread[n]; // a hit on step n lands on n + rotate
    return out;
}

bool lay_euclid(Pattern& p, uint8_t key, unsigned hits, unsigned steps, unsigned rotate, uint8_t velocity) {
    steps = std::clamp(steps, 1u, euclid_max_steps);
    hits = std::min(hits, steps);
    rotate %= steps;
    p.events.erase(std::remove_if(p.events.begin(), p.events.end(), [&](const Event& e) { return e.key == key; }), p.events.end());
    unsigned q = p.q();
    uint32_t span = std::min<uint32_t>(steps * q, p.length);
    p.set_lane_length(key, span < p.length ? span : 0); // shorter than the pattern: the lane loops on its own
    auto rhythm = euclid(hits, steps, rotate);
    bool fits = true;
    for (unsigned s = 0; s < steps && s * q < p.length; ++s) {
        if (!rhythm[s]) continue;
        Event e;
        e.tick = s * q; e.length = uint16_t(std::min<unsigned>(q, 0xffff)); e.key = key; e.velocity = velocity;
        fits &= p.add(e);
    }
    p.euclids.erase(std::remove_if(p.euclids.begin(), p.euclids.end(), [&](const EuclidLane& l) { return l.key == key; }), p.euclids.end());
    EuclidLane lane;
    lane.key = key; lane.hits = uint8_t(hits); lane.steps = uint8_t(steps); lane.rotate = uint8_t(rotate);
    p.euclids.insert(std::lower_bound(p.euclids.begin(), p.euclids.end(), lane, [](const EuclidLane& x, const EuclidLane& y) { return x.key < y.key; }), lane);
    return fits;
}

const EuclidLane* euclid_of(const Pattern& p, unsigned key) {
    for (const auto& l : p.euclids) if (l.key == key) return &l;
    return nullptr;
}

const char* variation_name(Variation v) {
    static const char* names[] {"Thinner", "Busier", "Humanised", "Reversed", "Octave up", "Octave down"};
    return names[unsigned(v) % unsigned(Variation::Count)];
}

namespace {
// Dice from a seed (splitmix64): the same variation again from the same seed.
struct Dice {
    uint64_t state;
    uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double unit() { return double(next() >> 11) / double(1ull << 53); } // 0..1
    bool chance(double p) { return unit() < p; }
    double nudge() { return unit() + unit() - 1.0; } // -1..1, mostly small
};

// Where a tick falls in the bar, strongest first: the bar's first, a beat, an eighth, a sixteenth, between.
int weight(uint32_t tick, const VaryPlace& place) {
    if (tick % place.bar == 0) return 4;
    if (tick % place.beat == 0) return 3;
    if (tick % (ticks_per_quarter / 2) == 0) return 2;
    if (tick % sixteenth_ticks == 0) return 1;
    return 0;
}

// The key's place in the project's scale, and back: degree n of the scale above C-1 (octaves of degree_count).
int degree_of(int key, const VaryPlace& place) {
    int count = int(std::max(place.degree_count, 1u)), best = 0;
    for (int d = 0; d < count * 11; ++d) {
        int k = (d / count) * 12 + int(place.root) + place.degrees[size_t(d % count)];
        if (k <= key) best = d;
    }
    return best;
}
int key_of(int degree, const VaryPlace& place) {
    int count = int(std::max(place.degree_count, 1u));
    degree = std::max(degree, 0);
    return std::clamp((degree / count) * 12 + int(place.root) + place.degrees[size_t(degree % count)], 0, 127);
}
} // namespace

Pattern vary_pattern(const Pattern& p, Variation v, double amount, uint64_t seed, const VaryPlace& place) {
    Dice dice {seed};
    amount = std::clamp(amount, 0.0, 1.0);
    Pattern out = p;
    std::vector<Event> notes;
    for (const auto& e : p.events) if (e.tick < p.length) notes.push_back(e);
    unsigned q = std::max(p.q(), 1u);
    out.events.clear();
    switch (v) {
    case Variation::Thinner: { // the weakest go (where they fall in the bar, then how hard), one at least staying
        if (notes.size() > 1) {
            size_t going = std::min(size_t(std::lround(amount * double(notes.size()))), notes.size() - 1);
            std::vector<std::pair<double, size_t>> ranked;
            for (size_t n = 0; n < notes.size(); ++n)
                ranked.push_back({weight(notes[n].tick, place) + notes[n].velocity / 128.0 + dice.unit() * 0.001, n});
            std::sort(ranked.begin(), ranked.end());
            std::vector<bool> gone(notes.size(), false);
            for (size_t n = 0; n < going; ++n) gone[ranked[n].second] = true;
            std::vector<Event> kept;
            for (size_t n = 0; n < notes.size(); ++n) if (!gone[n]) kept.push_back(notes[n]);
            notes = kept;
        }
        break;
    }
    case Variation::Busier: {
        std::vector<Event> added;
        if (place.kit) { // a softer hit on an empty step beside a hit of its lane
            std::vector<uint8_t> keys;
            for (const auto& e : notes) if (std::find(keys.begin(), keys.end(), e.key) == keys.end()) keys.push_back(e.key);
            for (uint8_t key : keys) {
                unsigned total = 0, count = 0;
                std::vector<bool> hit(p.steps(), false);
                for (const auto& e : notes) if (e.key == key) { hit[e.tick / q % p.steps()] = true; total += e.velocity; ++count; }
                uint8_t soft = uint8_t(std::max(1u, total / std::max(count, 1u) / 2));
                unsigned steps = p.steps();
                for (unsigned s = 0; s < steps; ++s) {
                    if (hit[s] || !(hit[(s + steps - 1) % steps] || hit[(s + 1) % steps]) || !dice.chance(amount)) continue;
                    Event e;
                    e.tick = s * q; e.length = uint16_t(q); e.key = key; e.velocity = soft;
                    added.push_back(e);
                }
            }
        } else if (!notes.empty()) { // a passing note halfway into a gap of two steps or more of the line (its top notes)
            std::vector<Event> line;
            for (const auto& e : notes)
                if (line.empty() || line.back().tick != e.tick) line.push_back(e);
                else if (e.key > line.back().key) line.back() = e;
            for (size_t n = 0; n < line.size(); ++n) {
                const auto& a = line[n];
                uint32_t next = n + 1 < line.size() ? line[n + 1].tick : line[0].tick + p.length;
                if (next - a.tick < 2 * q || !dice.chance(amount)) continue;
                uint32_t at = a.tick + std::max<uint32_t>((next - a.tick) / 2 / q * q, q);
                if (at >= next) continue;
                int from = degree_of(a.key, place), to = degree_of((n + 1 < line.size() ? line[n + 1] : line[0]).key, place);
                int degree = std::abs(to - from) >= 2 ? from + (to - from) / 2 : from + (dice.chance(0.5) ? 1 : -1);
                Event e;
                e.tick = at % p.length; e.length = uint16_t(std::min<uint32_t>(q, next - at));
                e.key = uint8_t(key_of(degree, place)); e.velocity = uint8_t(std::max(1, a.velocity * 4 / 5));
                added.push_back(e);
            }
        }
        notes.insert(notes.end(), added.begin(), added.end());
        break;
    }
    case Variation::Humanised: // a little early or late (a quarter of a step at the whole amount), softer or harder (20)
        for (auto& e : notes) {
            int shift = int(std::lround(dice.nudge() * q / 4.0 * amount));
            e.tick = uint32_t(std::clamp<int64_t>(int64_t(e.tick) + shift, 0, int64_t(p.length) - 1));
            e.velocity = uint8_t(std::clamp(int(std::lround(e.velocity + dice.nudge() * 20.0 * amount)), 1, 127));
        }
        break;
    case Variation::Reversed: // each note starts where its end was (up to the grid), mirrored in the pattern; slides go
        for (auto& e : notes) {
            uint32_t end = std::min<uint32_t>(e.tick + std::max<uint16_t>(e.length, 1), p.length);
            uint32_t at = p.length - end;
            at = std::min<uint32_t>((at + q - 1) / q * q, p.length - 1);
            e.tick = at;
            e.flags &= uint8_t(~step_slide);
        }
        break;
    case Variation::OctaveUp: case Variation::OctaveDown:
        if (!place.kit)
            for (auto& e : notes) {
                int key = e.key + (v == Variation::OctaveUp ? 12 : -12);
                if (key >= 0 && key <= 127) e.key = uint8_t(key);
            }
        break;
    default: break;
    }
    for (const auto& e : notes) out.add(e);
    return out;
}

} // namespace melodee
