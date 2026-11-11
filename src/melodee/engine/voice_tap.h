// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

// What a sampler's voices show the screen: once per audio block, after the
// render, the engine host takes up to eight taps of the track in view (the
// newest voices first, Chop's audition and the Samples preview among them)
// and the places of up to sixteen of its cloud's grains, and publishes them
// through a TapBoard. The screen reads them without a lock, and works out
// where the ear is from the block's frame stamp (heard_at). No allocation,
// no locks: the audio thread only copies words.
namespace melodee {

namespace tap {
enum Flag : uint8_t {
    Loop = 1,       // goes round from loop_at
    Wrapped = 2,    // went round in the block it was taken after
    Cloud = 4,      // a cloud: `at` is where its grains come from, `step` how fast Scan moves it
    Releasing = 8,  // let go (or a gate's note off): its envelope is falling
    Stretch = 16,   // a stretched voice: `at` is its place, `step` how fast the place moves
    Waiting = 32,   // Offset: not started yet
    Choked = 64,    // cut by a mute group, a mono pad or a new audition: gone in a few ms
};
}

struct VoiceTap {
    uint32_t serial = 0;    // the voice's own number (it grows with each start): follows one voice from block to block
    uint32_t at = 0;        // the frame in the file it stands at (its place, when stretched)
    uint32_t from = 0, to = 0, loop_at = 0; // its region in the file, and where a loop goes round from
    uint32_t age = 0;       // output frames it has sounded
    float step = 0;         // frames of the file per output frame, signed: reversed < 0
    float level = 0;        // its envelope 0..1, with a choke's and the region end's fades
    int16_t pad = -1;       // the pad (sampler_pads: the audition voice, Chop's or the Samples preview); -1 none
    uint16_t slot = 0xffff; // the sample
    uint8_t flags = 0;      // tap::Flag
    uint8_t spare[3] {};
};

// One grain of a cloud (mock 6): where it started reading, where it reads
// now, and how far through its window it is, in output frames.
struct GrainTap {
    uint32_t serial = 0;    // the grain's own number (it grows with each grain): follows one grain from block to block
    uint32_t from = 0;      // the frame in the file it started at
    uint32_t at = 0;        // the frame it reads now
    uint32_t age = 0;       // output frames it has sounded
    uint32_t length = 0;    // its window, in output frames
    float step = 0;         // frames of the file per output frame
    int16_t pad = -1;       // the pad whose cloud spawned it
    uint16_t slot = 0xffff; // the sample
};

struct TrackTaps {
    static constexpr unsigned voices = 8, grains = 16;
    uint64_t stamp = 0;        // the device frame the taps stand at: the end of the block they were taken after
    uint8_t track = 255;       // 255: none
    uint8_t count = 0;         // taps in `voice`, newest first
    uint8_t grain_count = 0;
    uint8_t spare = 0;
    uint32_t spare2 = 0;
    std::array<VoiceTap, voices> voice {};
    std::array<GrainTap, grains> grain {}; // a cloud's newest grains, `grain_count` of them, newest first
};
static_assert(std::is_trivially_copyable_v<TrackTaps> && sizeof(TrackTaps) % 4 == 0, "copied as words");

// One writer (the audio thread), any readers. A sequence lock over atomic
// words: the writer never waits, a reader tries again while a write is under way.
class TapBoard {
public:
    TapBoard() { publish(TrackTaps {}); } // nothing yet: track 255
    void publish(const TrackTaps& taps) {
        uint32_t words[word_count];
        std::memcpy(words, &taps, sizeof(TrackTaps));
        uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (unsigned n = 0; n < word_count; ++n) slot[n].store(words[n], std::memory_order_relaxed);
        seq.store(s + 2, std::memory_order_release);
    }
    // False when every try met a write (it leaves `out` as it was).
    bool read(TrackTaps& out) const {
        uint32_t words[word_count];
        for (unsigned tries = 0; tries < 16; ++tries) {
            uint32_t before = seq.load(std::memory_order_acquire);
            if (before & 1) continue;
            for (unsigned n = 0; n < word_count; ++n) words[n] = slot[n].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) != before) continue;
            std::memcpy(&out, words, sizeof(TrackTaps));
            return true;
        }
        return false;
    }

private:
    static constexpr unsigned word_count = sizeof(TrackTaps) / 4;
    std::atomic<uint32_t> seq {0};
    std::array<std::atomic<uint32_t>, word_count> slot {};
};

// Where the ear is in a voice's file `frames` output frames after its tap was
// taken (negative: before it, as the output latency makes it: what is heard
// now was rendered that long ago). A loop is followed round, a one shot held
// at its region's edges. `begun` is false while the voice was not yet
// sounding then; `pass` counts the times a loop went round (0: the first).
struct HeardPlace {
    double at = 0;
    bool begun = true;
    uint32_t pass = 0;
};
inline HeardPlace heard_at(const VoiceTap& t, double frames) {
    HeardPlace h;
    const double lo = t.from, hi = t.to;
    if (t.flags & tap::Waiting || double(t.age) + frames < 0) { // not heard yet: at its start
        h.begun = false;
        h.at = t.step < 0 && !(t.flags & tap::Cloud) ? hi - 1 : lo;
        return h;
    }
    double at = double(t.at) + double(t.step) * frames;
    if (t.flags & tap::Cloud) { // round inside the region
        double span = std::max(1.0, hi - lo);
        h.at = lo + std::fmod(std::fmod(at - lo, span) + span, span);
        return h;
    }
    if (!(t.flags & tap::Loop)) { h.at = std::clamp(at, lo, std::max(lo, hi - 1)); return h; }
    // A loop: forwards it plays from `from` to the end once, then round [loop_at, to); reversed from the end down to
    // loop_at, and round. Its passes come from how far it has gone (age, at its speed); the tap's own pass says
    // whether a place before the loop point is the first pass or the lap before.
    const double speed = std::fabs(double(t.step));
    const double round_lo = std::clamp(double(t.loop_at), lo, std::max(lo, hi - 1)), span = std::max(1.0, hi - round_lo);
    const double first = t.step < 0 ? span : std::max(1.0, hi - 1 - lo);
    auto pass_of = [&](double travelled) { return travelled >= first ? uint32_t((travelled - first) / span) + 1 : 0u; };
    const uint32_t tap_pass = pass_of(double(t.age) * speed);
    h.pass = pass_of((double(t.age) + frames) * speed);
    if (t.step >= 0) {
        if (at >= hi) at = round_lo + std::fmod(at - round_lo, span);
        else if (at < round_lo && tap_pass) at = hi - std::fmod(round_lo - at, span);
        h.at = std::max(at, lo);
    } else {
        if (at < round_lo) at = hi - std::fmod(round_lo - at, span);
        else if (at >= hi) at = round_lo + std::fmod(at - hi, span);
        h.at = at;
    }
    return h;
}

// Where a grain is heard `frames` output frames after its tap was taken
// (negative: before it, the output latency): `begun` false while it is not
// heard yet; `over` once its window has closed, `after` output frames ago.
// `at` stops where the grain stopped reading; `window` is its sine window
// heard now (0 before and after).
struct HeardGrain {
    bool begun = false, over = false;
    double at = 0;
    float window = 0;
    double after = 0;
};
inline HeardGrain heard_grain(const GrainTap& g, double frames) {
    HeardGrain h;
    const double age = double(g.age) + frames, length = std::max(1.0, double(g.length));
    h.at = g.from;
    if (age < 0) return h;
    h.begun = true;
    h.over = age >= length;
    h.at = double(g.from) + double(g.step) * std::min(age, length);
    if (h.over) h.after = age - length;
    else h.window = float(std::sin(3.14159265358979 * age / length));
    return h;
}

} // namespace melodee
