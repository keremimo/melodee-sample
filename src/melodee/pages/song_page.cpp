// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SONG (Shift + Pad 12): an arrange view of our own. B1 Export (parity
// phase 6): B2 an audio mixdown, B3 the song as a new pattern. One lane per track,
// time left to right; a clip is a pattern placed on a lane, in the track's
// colour with its notes in small, notches at each repeat. Markers name the
// parts, the brace is the loop, the strip at the bottom is the whole song.
// The wheel moves the cursor by the bar, pads pick the lane. Every clip job
// is on a button of its own, no menu: B3 places the pattern on K2 for the
// length on K3 (on a clip, duplicates it after itself), B2 mutes the clip,
// Shift + B2 its motion, Shift + B3 gives it its own pattern, Push opens its
// notes (on empty time, places as B3 does), Chop splits it at the cursor,
// Erase deletes it, Note Repeat repeats it to the next marker, Loop loops
// its bars. K1 zoom. Minus / Plus jump between markers (or clip edges while
// there are none). Shift + Push adds a marker at the cursor, or names it.
// Play here plays the song from the cursor.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/bindings.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;
// Six lanes at a time, tall enough for the clips' names; the window follows
// the selected track.
constexpr unsigned lanes = 6;
constexpr float x0 = 34, x1 = zone::width - zone::pad, lane_top = zone::picture_y + 29, lane_h = 15, lane_gap = 1;

unsigned zoom_of(const UiState& ui) { return song_zooms[ui.song_zoom % (sizeof(song_zooms) / sizeof(song_zooms[0]))]; }
// The first bar in view: the remembered one, moved just enough to show the cursor.
unsigned view_of(const UiState& ui) {
    unsigned zoom = zoom_of(ui), view = ui.song_view;
    if (ui.song_cursor < view) view = ui.song_cursor;
    if (ui.song_cursor >= view + zoom) view = ui.song_cursor - zoom + 1;
    return view;
}
juce::String clock_of(unsigned bar, unsigned tempo_tenths, unsigned bar_ticks) {
    unsigned seconds = unsigned(bar * (double(bar_ticks) / ticks_per_quarter) * 600.0 / tempo_tenths + 0.5);
    char text[16];
    std::snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
    return text;
}
juce::String dash() { return juce::String::fromUTF8("\xe2\x80\x93"); }

void draw_clip(Paint& p, const Snapshot& s, const Clip& clip, float y, unsigned view, float bw, bool selected) {
    float cx = x0 + (float(clip.bar) - float(view)) * bw, cw = clip.bars * bw;
    float left = std::max(cx, x0), right = std::min(cx + cw, x1);
    if (right <= left + 1) return;
    auto c = track_colour(clip.track);
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(left), int(y), int(right - left + 1), int(lane_h)));
    p.fill(cx + 0.5f, y, cw - 1, lane_h, selected ? c : c.withAlpha(clip.muted ? 0.12f : 0.38f), 2.5f);
    const auto& sketch = s.sketches[clip.track][clip.pattern];
    // Its notes in small: the pattern repeated across the clip.
    // In sixteenths: the sketch keeps the first note of each.
    unsigned bar = s.project.bar_ticks(), length = std::max<unsigned>((sketch.length + sixteenth_ticks - 1) / sixteenth_ticks, 1);
    float step_w = bw * sixteenth_ticks / bar;
    int lo = 127, hi = 0;
    for (unsigned k = 0; k < std::min<unsigned>(length, 64); ++k)
        if (sketch.keys[k]) { lo = std::min<int>(lo, sketch.keys[k]); hi = std::max<int>(hi, sketch.keys[k]); }
    unsigned steps = unsigned(clip.bars * bar / sixteenth_ticks);
    auto note = selected ? colour::bg : c;
    if (hi >= lo && step_w >= 0.6f && !clip.muted)
        for (unsigned k = 0; k < steps; ++k) {
            unsigned at = k % length;
            if (at >= 64 || !sketch.keys[at]) continue;
            float nx = cx + k * step_w, ny = y + lane_h - 3 - (hi > lo ? float(sketch.keys[at] - lo) / (hi - lo) : 0.5f) * (lane_h - 7);
            if (nx > right) break;
            p.fill(nx, ny, std::max(1.0f, step_w * 0.8f), 1, note);
        }
    // A notch at each repeat of the pattern.
    float repeat = length * step_w;
    if (repeat >= 4)
        for (float rx = cx + repeat; rx < cx + cw - 1; rx += repeat) p.fill(rx, y, 0.5f, lane_h, colour::bg);
    p.text("P" + juce::String(clip.pattern + 1), cx + 3, y, 24, lane_h, plain(selected ? colour::bg : c, type::small, Weight::Bold));
    if (selected) p.ring(cx + 0.5f, y, cw - 1, lane_h, colour::text, 2.5f);
    p.g.restoreState();
}

// B1-B3 follow what is under the cursor; with Shift, the clip's second jobs.
SoftKeys keys_of(const Snapshot& s, const Clip* clip, juce::Colour c) {
    if (s.ui.shift) return {{"", clip ? "MOTION" : "", clip ? "OWN PATTERN" : ""}, -1, c};
    if (clip) return {{"EXPORT", clip->muted ? "UNMUTE" : "MUTE", "DUPLICATE"}, -1, c};
    return {{"EXPORT", "", s.project.tracks[s.ui.track].used() ? "PLACE" : ""}, -1, c};
}

void draw(Paint& p, const Snapshot& s) {
    const auto& song = s.project.song;
    auto c = colour_of(s);
    unsigned zoom = zoom_of(s.ui), view = view_of(s.ui), cursor = s.ui.song_cursor;
    float bw = (x1 - x0) / zoom;
    int selected_clip = song.clip_at(s.ui.track, cursor);
    Header h = header_of(s);
    h.left.push_back(pill("BAR " + juce::String(cursor + 1) + juce::String::fromUTF8(" \xc2\xb7 ") + clock_of(cursor, s.project.tempo, s.project.bar_ticks())));
    if (zoom == song_zooms[0]) h.left.push_back(pill("SNAP 1 BAR"));
    // Following MIDI clock or MTC in, its chip takes the loop's room (the ruler's brace still shows the loop).
    else if (song.looping() && !s.following) h.left.push_back(pill("LOOP " + juce::String(song.loop_start + 1) + dash() + juce::String(song.loop_end), colour::text));
    h.right.push_back(tag_of(s, s.ui.track));

    if (s.ui.song_export) {
        // Export (B1), as on the MPC: B2 an audio mixdown, B3 a new pattern; a tile under each says what it makes.
        SoftKeys keys {{"CANCEL", "MIXDOWN", "PATTERN"}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
        keys.glyphs = {Icon::None, Icon::Wav, Icon::Pattern};
        frame(p, s, keys, h);
        p.dim_zone(zone::picture_y, zone::picture_h, 0.86f);
        p.dim_zone(zone::knobs_y, zone::knobs_h);
        unsigned bars = song.length(), seconds = unsigned(bars * (double(s.project.bar_ticks()) / ticks_per_quarter) * 600.0 / std::max(1u, s.tempo) + 0.5);
        if (!bars) { // nothing to export yet
            icon(p, Icon::Pattern, zone::width / 2.0f - 14, zone::picture_y + 44, 28, colour::amber, 1.4f);
            p.text("EMPTY SONG", 0, zone::picture_y + 80, zone::width, 14, caps_text(colour::amber), juce::Justification::centred);
            return;
        }
        unsigned free = pattern_count; // the first pattern number no track uses
        for (unsigned n = 0; n < pattern_count && free == pattern_count; ++n) {
            bool used = false;
            for (unsigned t = 0; t < track_count; ++t) used = used || s.sketches[t][n].notes;
            if (!used) free = n;
        }
        juce::String length = juce::String(seconds / 60) + ":" + juce::String(seconds % 60).paddedLeft('0', 2);
        key_tiles(p, {KeyTile {}, value_tile(Icon::Wav, length, juce::String::fromUTF8("WAV \xc2\xb7 24-BIT"), true),
                      value_tile(Icon::Pattern, free < pattern_count ? "P" + juce::String(free + 1) : juce::String("FULL"),
                                 juce::String(bars) + (bars == 1 ? " BAR" : " BARS"))});
        return;
    }
    frame(p, s, keys_of(s, selected_clip >= 0 ? &song.clips[size_t(selected_clip)] : nullptr, c), h);

    // Markers, the ruler and the loop brace.
    float label_every = zoom <= 8 ? 1 : zoom / 4.0f;
    for (unsigned m = 0; m < song.marker_count; ++m) {
        const auto& marker = song.markers[m];
        if (marker.bar < view || marker.bar >= view + zoom) continue;
        float mx = x0 + (marker.bar - view) * bw;
        p.text(marker.name, mx + 2, zone::picture_y, 80, 13, caps_text(colour::mid));
        p.fill(mx, zone::picture_y, 0.5f, lane_top - zone::picture_y + lanes * (lane_h + lane_gap), colour::line);
    }
    for (unsigned b = view; b < view + zoom; ++b)
        if (unsigned(b - view) % unsigned(label_every) == 0)
            p.text(juce::String(b + 1), x0 + (b - view) * bw + 2, zone::picture_y + 13, 30, 13, plain(colour::mid, type::small, Weight::SemiBold));
    if (song.looping()) {
        float lx = x0 + (float(song.loop_start) - float(view)) * bw, rx = x0 + (float(song.loop_end) - float(view)) * bw;
        lx = std::max(lx, x0); rx = std::min(rx, x1);
        if (rx > lx) {
            p.fill(lx, lane_top - 3, rx - lx, 1, colour::sec);
            p.fill(lx, lane_top - 4, 1, 3, colour::sec);
            p.fill(rx - 1, lane_top - 4, 1, 3, colour::sec);
        }
    }
    // Lanes: six tracks in view, the selected one among them.
    unsigned first = std::min<unsigned>(s.ui.track >= 2 ? s.ui.track - 2 : 0, track_count - lanes);
    for (unsigned row = 0; row < lanes; ++row) {
        unsigned t = first + row;
        float y = lane_top + row * (lane_h + lane_gap);
        auto tc = track_colour(t);
        bool selected = t == s.ui.track;
        if (selected) p.fill(zone::pad, y, 22, lane_h, tc, 3);
        p.text(juce::String(t + 1), zone::pad, y, 22, lane_h, plain(selected ? colour::bg : s.project.tracks[t].used() ? tc : colour::dim, type::small, Weight::Bold),
               juce::Justification::centred);
        for (unsigned n = 0; n < song.count; ++n)
            if (song.clips[n].track == t) draw_clip(p, s, song.clips[n], y, view, bw, int(n) == selected_clip);
    }
    float bottom = lane_top + lanes * (lane_h + lane_gap);
    p.fill(x0 - 1, lane_top - 2, 0.5f, bottom - lane_top + 2, colour::line);
    // The cursor, and the playhead when the song plays.
    float cx = x0 + (float(cursor) - float(view)) * bw;
    p.fill(cx, zone::picture_y + 14, 1, bottom - zone::picture_y - 14, c);
    p.fill(cx - 2, zone::picture_y + 13, 5, 5, c, 1);
    if (s.song_playing) {
        unsigned bar = s.project.bar_ticks();
        float at = float(s.song_bar) + float(s.ticks % bar) / bar;
        if (at >= view && at < view + zoom) {
            float px = x0 + (at - view) * bw;
            p.fill(px, zone::picture_y + 17, 1, bottom - zone::picture_y - 17, colour::text);
            juce::Path tri;
            tri.addTriangle(px - 3, zone::picture_y + 13, px + 4, zone::picture_y + 13, px + 0.5f, zone::picture_y + 18);
            p.g.setColour(colour::text); p.g.fillPath(tri);
        }
    }
    // The punch lane: punches written into the song, in their target's colour.
    if (song.punch_count) {
        p.text("P", zone::pad, bottom - 3, 22, 11, caps_text(colour::dim), juce::Justification::centred);
        for (unsigned n = 0; n < song.punch_count; ++n) {
            const auto& e = song.punches[n];
            float from = float(e.bar) + e.step / 16.0f, to = from + e.length / 16.0f;
            if (to <= view || from >= view + zoom) continue;
            float px = x0 + (std::max(from, float(view)) - view) * bw, pw = std::max(1.5f, (std::min(to, float(view + zoom)) - std::max(from, float(view))) * bw);
            p.fill(px, bottom, pw, 2.5f, e.target == 255 ? colour::text : track_colour(e.target), 1);
        }
    }
    // The whole song, with the window on it.
    unsigned total = std::max({song.length(), view + zoom, 16u});
    float oy = bottom + (song.punch_count ? 13 : 6), ow = x1 - x0;
    p.text(juce::String(song.length()), zone::pad, oy - 5.5f, 22, 11, plain(colour::dim, type::small, Weight::SemiBold), juce::Justification::centred);
    p.fill(x0, oy, ow, 2, colour::line, 1);
    p.fill(x0 + ow * view / total, oy - 1, std::max(4.0f, ow * zoom / total), 4, colour::mid, 2);
}

int clip_index(const Context& c) { return c.project.song.clip_at(c.ui.track, c.ui.song_cursor); }

void place(Context& c) {
    if (!c.track().used()) return;
    Clip clip;
    clip.track = c.ui.track; clip.pattern = c.ui.song_pattern; clip.bar = c.ui.song_cursor; clip.bars = c.ui.song_length;
    if (c.project.song.place(clip)) c.sync_song();
}

void duplicate(Context& c, int index) {
    Clip copy = c.project.song.clips[size_t(index)];
    copy.bar = copy.end();
    if (!c.project.song.place(copy)) return;
    c.ui.song_cursor = copy.bar;
    c.ui.song_view = uint16_t(view_of(c.ui));
    c.sync_song();
}

// A copy of its pattern in the first empty slot, so editing it changes this clip alone.
void own_pattern(Context& c, int index) {
    auto& song = c.project.song;
    Clip clip = song.clips[size_t(index)];
    const auto& patterns = c.patterns.at[clip.track];
    for (unsigned n = 0; n < pattern_count; ++n) {
        bool placed = false;
        for (unsigned k = 0; k < song.count; ++k) placed |= song.clips[k].track == clip.track && song.clips[k].pattern == n;
        if (placed || !patterns[n].empty() || n == clip.pattern) continue;
        c.copy_pattern(clip.track, clip.pattern, n);
        song.clips[size_t(index)].pattern = uint8_t(n);
        c.sync_song();
        return;
    }
    c.notice("No free pattern");
}

// Into the clip's pattern: its notes, or with `motion` its motion lanes.
void open_clip(Context& c, int index, bool motion) {
    Clip clip = c.project.song.clips[size_t(index)];
    c.launch(clip.track, clip.pattern);
    c.sync_song();
    auto engine = c.track().engine;
    c.open(motion ? PageId::Motion : engine == EngineKind::Sampler || engine == EngineKind::Kit909 ? PageId::StepEdit : PageId::Roll);
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (down) c.select_track(pad); // pads pick the lane
}

void wheel(Context& c, int delta) {
    if (c.ui.song_export) return;
    c.ui.song_cursor = uint16_t(std::clamp<int>(c.ui.song_cursor + delta, 0, int(song_bars) - 1));
    c.ui.song_view = uint16_t(view_of(c.ui));
}

void push(Context& c) {
    if (c.ui.song_export) return;
    int index = clip_index(c);
    if (index >= 0) open_clip(c, index, false);
    else place(c);
}

void soft(Context& c, int key) {
    if (c.ui.song_export) { c.song_export_soft(key); return; } // B1 Cancel, B2 Audio mixdown, B3 New pattern
    int index = clip_index(c);
    if (c.ui.shift) {
        if (index < 0) return;
        if (key == 1) open_clip(c, index, true);
        if (key == 2) own_pattern(c, index);
        return;
    }
    if (key == 0) c.ui.song_export = true; // Export, as on the MPC (Patterns is on Seq)
    if (key == 1 && index >= 0) {
        auto& clip = c.project.song.clips[size_t(index)];
        clip.muted = !clip.muted;
        c.sync_song();
    }
    if (key == 2) { if (index >= 0) duplicate(c, index); else place(c); }
}

// Minus / Plus: the previous or next marker, or clip edge while there are no markers.
void nudge(Context& c, int way) {
    const auto& song = c.project.song;
    int cursor = c.ui.song_cursor, best = -1;
    auto consider = [&](int bar) {
        if (way > 0 ? bar > cursor && (best < 0 || bar < best) : bar < cursor && (best < 0 || bar > best)) best = bar;
    };
    for (unsigned m = 0; m < song.marker_count; ++m) consider(song.markers[m].bar);
    if (!song.marker_count)
        for (unsigned n = 0; n < song.count; ++n) { consider(song.clips[n].bar); consider(song.clips[n].end()); }
    if (best < 0) best = way > 0 ? cursor : 0;
    c.ui.song_cursor = uint16_t(best);
    c.ui.song_view = uint16_t(view_of(c.ui));
}
} // namespace

// Shift + Push: a marker at the cursor with the next ready name, or on a
// marker the Name sheet to type its name (K1 there still steps the ready ones).
void song_marker(Context& c) {
    if (c.project.song.marker_at(c.ui.song_cursor) >= 0) c.name_marker(c.ui.song_cursor);
    else c.project.song.step_marker(c.ui.song_cursor);
}

// Chop: the clip in two at the cursor.
void song_chop(Context& c) {
    auto& song = c.project.song;
    int index = clip_index(c);
    if (index < 0 || c.ui.song_cursor <= song.clips[size_t(index)].bar) { c.notice("Chop: cursor inside a clip"); return; }
    Clip clip = song.clips[size_t(index)];
    unsigned cursor = c.ui.song_cursor;
    song.clips[size_t(index)].bars = uint16_t(cursor - clip.bar);
    Clip tail = clip;
    tail.bar = uint16_t(cursor); tail.bars = uint16_t(clip.end() - cursor);
    song.place(tail);
    c.sync_song();
}

void song_erase(Context& c) {
    int index = clip_index(c);
    if (index < 0) return;
    c.project.song.erase(unsigned(index));
    c.sync_song();
}

// Note Repeat: the clip again and again up to the next marker.
void song_repeat(Context& c) {
    auto& song = c.project.song;
    int index = clip_index(c);
    if (index < 0) return;
    Clip clip = song.clips[size_t(index)];
    unsigned until = song.next_marker(clip.bar);
    if (until <= clip.end()) { c.notice("Repeat: needs a marker ahead"); return; }
    for (unsigned at = clip.end(); at < until;) {
        Clip copy = clip;
        copy.bar = uint16_t(at); copy.bars = uint16_t(std::min<unsigned>(clip.bars, until - at));
        if (!song.place(copy)) break;
        at += copy.bars;
    }
    c.sync_song();
}

// Loop: the brace on the clip's bars; again, or on empty time, off.
bool song_loop(Context& c) {
    auto& song = c.project.song;
    int index = clip_index(c);
    if (index < 0 && !song.looping()) return false;
    c.ui.song_loop_was[0] = song.loop_start; c.ui.song_loop_was[1] = song.loop_end;
    bool same = index >= 0 && song.loop_start == song.clips[size_t(index)].bar && song.loop_end == song.clips[size_t(index)].end();
    if (index < 0 || same) song.loop_start = song.loop_end = 0;
    else { song.loop_start = song.clips[size_t(index)].bar; song.loop_end = song.clips[size_t(index)].end(); }
    c.sync_song();
    return true;
}

// The brace as it was: a tap that turned out to be the first of two, for the Looper.
void song_loop_back(Context& c) {
    c.project.song.loop_start = c.ui.song_loop_was[0];
    c.project.song.loop_end = c.ui.song_loop_was[1];
    c.sync_song();
}

const PageSpec song_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
