// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/pattern.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Roadmap P5, always-on capture: every note played on any track (the pads,
// the keys, MIDI in), kept whether or not Seq Record is on, the last minutes
// of it, in a ring made once. It is the MIDI thread's: the audio thread never
// sees it, so nothing there allocates. Shift + Seq Record (Recall) lays the
// last of it into patterns after the fact: capture_window() finds the bars
// to keep, the same for every track so the patterns line up, and
// lay_capture() makes each track's.
namespace melodee {

struct PlayedNote {
    uint32_t at = 0, length = 0;  // ms when heard, and how long it was held (0: still held)
    uint32_t tick = 0, until = 0; // the transport's ticks where it was heard and let go, while it ran
    uint16_t run = 0;             // which run of the transport it was played in (0: stopped)
    uint16_t key = 0;             // as an Event plays it: key | pitch << 8
    uint8_t track = 0, velocity = 0;
};

constexpr unsigned capture_size = 8192;     // minutes of a busy hand (a sixteenth hat at 120 BPM: 17 minutes)
constexpr unsigned capture_gap_bars = 2;    // a phrase ends where no track played for this long
constexpr unsigned capture_most_bars = 64;  // the longest phrase kept
constexpr unsigned capture_auto = 0;        // Settings' Capture bars: the phrase, as long as it is

struct CaptureRing {
    std::array<PlayedNote, capture_size> notes {};
    uint32_t written = 0;
    unsigned count() const { return std::min<uint32_t>(written, capture_size); }
    const PlayedNote& newest(unsigned n) const { return notes[(written - 1 - n) % capture_size]; } // 0: the newest
    PlayedNote& newest(unsigned n) { return notes[(written - 1 - n) % capture_size]; }
    void press(unsigned track, int key, int velocity, uint32_t at, uint16_t run, uint32_t tick) {
        PlayedNote& n = notes[written % capture_size];
        n = {};
        n.track = uint8_t(track % track_count); n.key = uint16_t(key); n.velocity = uint8_t(std::clamp(velocity, 1, 127));
        n.at = at; n.run = run; n.tick = tick;
        ++written;
    }
    // The press it ends: the newest of that key on that track still held (a few hundred back at most).
    void release(unsigned track, int key, uint32_t at, uint32_t tick) {
        for (unsigned n = 0; n < std::min(count(), 512u); ++n) {
            PlayedNote& p = newest(n);
            if (p.track != track % track_count || p.key != uint16_t(key) || p.length) continue;
            p.length = std::max<uint32_t>(1, at - p.at);
            p.until = p.run ? tick : 0;
            return;
        }
    }
    unsigned played(unsigned track) const { // notes kept of a track
        unsigned n = 0;
        for (unsigned k = 0; k < count(); ++k) n += newest(k).track == track % track_count;
        return n;
    }
};

// What to keep: as Settings say (bars, 0 the phrase; and the grid, 1 tick as played), with the transport now.
struct CaptureAsk {
    uint32_t bar = 4 * ticks_per_quarter; // ticks a bar
    unsigned bars = capture_auto;
    uint32_t grid = 1;
    double tick_ms = 60000.0 / 120.0 / ticks_per_quarter; // ms a tick at the tempo now, for what was played stopped
    uint32_t now = 0;                     // ms
    uint32_t ticks = 0;                   // the transport's tick now (while it runs)
    uint16_t tracks = 0xffff;             // the tracks it looks at, a bit each
};

// The bars kept, for every track alike. A transport run's (run != 0): ticks from `start`, whole bars against its bar
// lines, from the bar the phrase began in (or so many bars back) to the bar after its newest note. Played stopped
// (run 0): ms from `start`, the phrase's first note its downbeat, at the tempo now. `length` in ticks either way.
struct CaptureWindow {
    bool any = false;
    uint16_t run = 0;
    uint32_t start = 0, length = 0;
};

inline CaptureWindow capture_window(const CaptureRing& ring, const CaptureAsk& ask) {
    CaptureWindow w;
    auto wanted = [&](const PlayedNote& p) { return ask.tracks >> (p.track % track_count) & 1; };
    unsigned newest = ring.count();
    for (unsigned n = 0; n < ring.count(); ++n) if (wanted(ring.newest(n))) { newest = n; break; }
    if (newest == ring.count()) return w;
    const PlayedNote& last = ring.newest(newest);
    uint32_t bar = std::max<uint32_t>(ask.bar, 1);
    w.any = true; w.run = last.run;
    if (last.run) { // on the transport's bar lines
        uint32_t end = (last.tick / bar + 1) * bar, first = last.tick, prev = last.tick;
        for (unsigned n = newest + 1; n < ring.count(); ++n) { // back through the phrase: no gap of two bars, the same run
            const PlayedNote& p = ring.newest(n);
            if (!wanted(p)) continue;
            if (p.run != last.run || p.tick > prev || prev - p.tick > capture_gap_bars * bar) break;
            first = prev = p.tick;
        }
        uint32_t start = ask.bars ? (end >= ask.bars * bar ? end - ask.bars * bar : 0) : first / bar * bar;
        if (!ask.bars && end - start > capture_most_bars * bar) start = end - capture_most_bars * bar;
        w.start = start; w.length = ask.bars ? ask.bars * bar : end - start;
        return w;
    }
    // Played stopped: from the phrase's first note (within the bars asked), at the tempo now.
    double bar_ms = bar * ask.tick_ms;
    uint32_t first = last.at, prev = last.at, reach = last.at + last.length;
    double earliest = ask.bars ? double(ask.now) - ask.bars * bar_ms : -1e18;
    for (unsigned n = newest + 1; n < ring.count(); ++n) {
        const PlayedNote& p = ring.newest(n);
        if (!wanted(p)) continue;
        if (p.run || p.at > prev || double(prev - p.at) > capture_gap_bars * bar_ms || double(p.at) < earliest) break;
        first = prev = p.at;
    }
    for (unsigned n = newest; n < ring.count(); ++n) { // where the phrase's notes end (a held one: now)
        const PlayedNote& p = ring.newest(n);
        if (!wanted(p) || p.run) continue;
        if (p.at < first) break;
        reach = std::max(reach, p.length ? p.at + p.length : ask.now);
    }
    unsigned bars = ask.bars ? ask.bars
                             : unsigned(std::clamp<double>(std::ceil((double(reach) - first) / bar_ms - 1e-6), 1.0, capture_most_bars));
    w.start = first; w.length = std::max(1u, bars) * bar;
    return w;
}

// Track `track`'s notes in the window as a pattern (its length the window's, Q a sixteenth), on the grid asked; false
// when it played nothing there.
inline bool lay_capture(const CaptureRing& ring, unsigned track, const CaptureWindow& w, const CaptureAsk& ask, Pattern& out) {
    out = Pattern {};
    out.division = Division::Sixteenth;
    out.length = std::max<uint32_t>(w.length, 1);
    if (!w.any) return false;
    uint32_t grid = std::max<uint32_t>(ask.grid, 1);
    bool any = false;
    for (unsigned n = ring.count(); n-- > 0;) { // oldest first
        const PlayedNote& p = ring.newest(n);
        if (p.track != track % track_count || p.run != w.run) continue;
        double at = 0, held = 0;
        if (w.run) {
            if (p.tick < w.start || p.tick >= w.start + out.length) continue;
            at = double(p.tick - w.start);
            held = p.until > p.tick ? double(p.until - p.tick) : p.length ? p.length / ask.tick_ms : double(ask.ticks > p.tick ? ask.ticks - p.tick : sixteenth_ticks);
        } else {
            if (p.at < w.start) continue;
            at = (p.at - w.start) / ask.tick_ms;
            if (at >= out.length) continue;
            held = p.length ? p.length / ask.tick_ms : (ask.now - p.at) / ask.tick_ms;
        }
        uint32_t tick = uint32_t(std::lround(at / grid)) * grid;
        if (tick >= out.length) tick = out.length - std::min(out.length, grid); // right at the end: the last place
        uint32_t length = uint32_t(std::max(1L, std::lround(std::max(held, 1.0) / grid))) * grid;
        Event e;
        e.tick = tick; e.key = uint8_t(p.key & 0xff); e.pitch = int8_t(p.key >> 8); e.velocity = p.velocity;
        e.length = uint16_t(std::min<uint32_t>({length, out.length, 65535u}));
        if (out.add(e)) any = true;
    }
    return any;
}

} // namespace melodee
