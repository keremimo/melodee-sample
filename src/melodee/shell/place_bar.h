// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/snapshot.h"
#include <algorithm>
#include <array>
#include <cstdint>

// The place bar (Settings: Place bar, Top or Bottom): a 2 px strip on every
// screen that shows, without text, where the music is. Its segments are the
// bars of the pattern playing on the track in view (grouped so there are
// never more than 16), the Song's sections between its markers while the
// Song plays, or the bars of the selected loop on the Looper page. The fill
// is how far it has played; its tone says what the transport does: playing,
// recording (or overdubbing), waiting (Count-In, Seq Record or a loop armed,
// a loop queued for its bar) or stopped, where it holds the place Shift +
// Play carries on from. pages/place_bar.cpp draws it.
namespace melodee {

struct PlaceBar {
    enum class Tone : uint8_t { Playing, Recording, Waiting, Stopped };
    static constexpr unsigned most = 16;
    Tone tone = Tone::Stopped;
    std::array<uint32_t, most> segments {}; // lengths, ticks (Song: ticks of its bars)
    unsigned count = 1;
    uint32_t position = 0; // ticks from the start of the first segment
    uint32_t notch = 0;    // ticks between the notches inside the segment it is in (0: none)
    uint32_t beat = 96;    // ticks a beat: the leading edge ticks on each
    bool held = false;     // not moving: no fill pulse, a still edge
    unsigned blocks = 0, blocks_left = 0; // Count-In: a block a beat, the ones still to come
    uint32_t total() const { uint32_t t = 0; for (unsigned n = 0; n < count; ++n) t += segments[n]; return t; }
};

// Splits `length` ticks into segments of `unit` ticks (the last one shorter),
// doubling the unit until there are no more than PlaceBar::most of them. The
// notch is the unit it started from when the segments hold more than one,
// else `smaller`.
inline void place_segments(PlaceBar& bar, uint32_t length, uint32_t unit, uint32_t smaller) {
    length = std::max<uint32_t>(length, 1); unit = std::max<uint32_t>(unit, 1);
    uint32_t start = unit;
    while ((length + unit - 1) / unit > PlaceBar::most) unit *= 2;
    bar.count = 0;
    for (uint32_t at = 0; at < length && bar.count < PlaceBar::most; at += unit) bar.segments[bar.count++] = std::min(unit, length - at);
    bar.notch = unit > start ? start : smaller;
}

inline PlaceBar place_bar_of(const Snapshot& s) {
    PlaceBar bar;
    const auto& project = s.project;
    uint32_t bar_ticks = std::max(project.bar_ticks(), 1u), beat = std::max(project.beat_ticks(), 1u);
    bar.beat = beat;
    // Count-In: a block a beat across the whole strip, emptying as they pass.
    if (s.count_in) {
        bar.tone = PlaceBar::Tone::Waiting;
        bar.blocks = std::max(1u, unsigned(project.time_signature().beats));
        bar.blocks_left = s.count_in > bar.blocks ? 0 : bar.blocks - s.count_in + 1;
        return bar;
    }
    // The Looper page: the loop selected, once it holds (or is taking) something.
    if (s.ui.page == PageId::Looper) {
        const auto& v = s.loops[s.ui.loop % loop_tracks].view;
        using looper::State; using looper::Pending;
        if (v.state != State::Empty) {
            bool taking = v.state == State::Recording && v.ticks <= 0;
            if (taking) { // its first take: one segment, as far as the waveform spans
                bar.count = 1; bar.segments[0] = std::max<uint32_t>(v.span, 1);
                bar.position = std::min(v.position, bar.segments[0]); bar.notch = 0;
            } else {
                uint32_t ticks = uint32_t(std::max(v.ticks, 1.0));
                place_segments(bar, ticks, bar_ticks, beat);
                bar.position = v.length ? uint32_t(double(v.position) / v.length * ticks) : 0;
            }
            if (v.state == State::Recording || v.state == State::Overdub) bar.tone = PlaceBar::Tone::Recording;
            else if (v.state == State::Armed || v.pending == Pending::Start || v.pending == Pending::Play) bar.tone = PlaceBar::Tone::Waiting;
            else if (v.state == State::Playing) bar.tone = PlaceBar::Tone::Playing;
            else bar.tone = PlaceBar::Tone::Stopped;
            bar.held = bar.tone == PlaceBar::Tone::Stopped || v.state == State::Armed;
            if (taking && bar.tone == PlaceBar::Tone::Recording) bar.held = false;
            return bar;
        }
    }
    // The Song while it plays (or, stopped on the Song page, the bar it stopped in): its sections between markers.
    bool song = s.song_playing || (!s.playing && s.ui.page == PageId::Song);
    if (song) {
        unsigned bars = std::max(project.song.length(), 1u);
        std::array<unsigned, PlaceBar::most + 1> cuts {};
        unsigned n = 0;
        cuts[n++] = 0;
        std::array<unsigned, song_markers> marks {};
        unsigned marked = 0;
        for (unsigned m = 0; m < project.song.marker_count && m < song_markers; ++m)
            if (project.song.markers[m].bar > 0 && project.song.markers[m].bar < bars) marks[marked++] = project.song.markers[m].bar;
        std::sort(marks.begin(), marks.begin() + marked);
        for (unsigned m = 0; m < marked && n < PlaceBar::most; ++m) if (marks[m] != cuts[n - 1]) cuts[n++] = marks[m];
        if (n > 1) {
            bar.count = n;
            for (unsigned k = 0; k < n; ++k) bar.segments[k] = ((k + 1 < n ? cuts[k + 1] : bars) - cuts[k]) * bar_ticks;
            bar.notch = bar_ticks;
        } else place_segments(bar, bars * bar_ticks, bar_ticks, beat);
        unsigned at = std::min(s.song_bar, bars - 1);
        bar.position = at * bar_ticks + (s.playing ? s.ticks % bar_ticks : 0);
    } else {
        // The pattern playing on the track in view; stopped, where Shift + Play carries on from.
        uint32_t length = std::max<uint32_t>(s.pattern.length, 1);
        place_segments(bar, length, bar_ticks, beat);
        bar.position = (s.playing ? s.position : s.ui.locate) % length;
    }
    if (!s.playing) {
        bar.held = true;
        bar.tone = s.ui.rec_armed ? PlaceBar::Tone::Waiting : PlaceBar::Tone::Stopped;
    } else bar.tone = s.ui.recording ? PlaceBar::Tone::Recording : PlaceBar::Tone::Playing;
    return bar;
}

} // namespace melodee
