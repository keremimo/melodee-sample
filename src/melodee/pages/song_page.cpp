// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SONG (B2 on Patterns): an arrange view of our own. One lane per track,
// time left to right; a clip is a pattern placed on a lane, in the track's
// colour with its notes in small, notches at each repeat. Markers name the
// parts, the brace is the loop, the strip at the bottom is the whole song.
// The wheel moves the cursor by the bar, pads pick the lane, Push on a clip
// opens its sheet, Push on empty time places the pattern on K2 for the
// length on K3. K1 zoom. Minus / Plus jump between markers (or clip edges
// while there are none). Shift + Push adds a marker at the cursor, or gives
// it the next name, or after the last name removes it. Play here plays the
// song from the cursor.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/bindings.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;
constexpr float x0 = 34, x1 = zone::width - zone::pad, lane_top = zone::picture_y + 21, lane_h = 13.5f, lane_gap = 1;
constexpr unsigned clip_actions = 8;

unsigned zoom_of(const UiState& ui) { return song_zooms[ui.song_zoom % (sizeof(song_zooms) / sizeof(song_zooms[0]))]; }
// The first bar in view: the remembered one, moved just enough to show the cursor.
unsigned view_of(const UiState& ui) {
    unsigned zoom = zoom_of(ui), view = ui.song_view;
    if (ui.song_cursor < view) view = ui.song_cursor;
    if (ui.song_cursor >= view + zoom) view = ui.song_cursor - zoom + 1;
    return view;
}
juce::String clock_of(unsigned bar, unsigned tempo_tenths) {
    unsigned seconds = unsigned(bar * 4 * 600.0 / tempo_tenths + 0.5);
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
    unsigned division = division_ticks(sketch.division), length = std::max<unsigned>(sketch.length, 1);
    float step_w = bw * division / ticks_per_bar;
    int lo = 127, hi = 0;
    for (unsigned k = 0; k < std::min<unsigned>(length, 64); ++k)
        if (sketch.keys[k]) { lo = std::min<int>(lo, sketch.keys[k]); hi = std::max<int>(hi, sketch.keys[k]); }
    unsigned steps = unsigned(clip.bars * ticks_per_bar / division);
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
    p.text("P" + juce::String(clip.pattern + 1), cx + 3, y, 20, 8, plain(selected ? colour::bg : c, 6.5f, Weight::Bold));
    if (selected) p.ring(cx + 0.5f, y, cw - 1, lane_h, colour::text, 2.5f);
    p.g.restoreState();
}

void draw(Paint& p, const Snapshot& s) {
    const auto& song = s.project.song;
    auto c = colour_of(s);
    unsigned zoom = zoom_of(s.ui), view = view_of(s.ui), cursor = s.ui.song_cursor;
    float bw = (x1 - x0) / zoom;
    int selected_clip = song.clip_at(s.ui.track, cursor);
    Header h = header_of(s);
    h.left.push_back(pill("BAR " + juce::String(cursor + 1) + juce::String::fromUTF8(" \xc2\xb7 ") + clock_of(cursor, s.project.tempo)));
    if (zoom == song_zooms[0]) h.left.push_back(pill("SNAP 1 BAR"));
    else if (song.looping()) h.left.push_back(pill("LOOP " + juce::String(song.loop_start + 1) + dash() + juce::String(song.loop_end), colour::text));
    h.right.push_back(tag_of(s, s.ui.track));

    if (s.ui.sheet_open && selected_clip >= 0) {
        // The clip sheet over the page.
        frame(p, s, {{"BACK", "", "DO IT"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}, h);
        p.dim_zone(zone::knobs_y, zone::knobs_h);
        const auto& clip = song.clips[size_t(selected_clip)];
        unsigned next_marker = 0;
        for (unsigned m = 0; m < song.marker_count; ++m)
            if (song.markers[m].bar > clip.bar && (!next_marker || song.markers[m].bar < next_marker)) next_marker = song.markers[m].bar;
        MenuSheet m;
        m.title = "Clip"; m.accent = track_colour(clip.track);
        m.detail = "BARS " + juce::String(clip.bar + 1) + dash() + juce::String(clip.end()) + juce::String::fromUTF8(" \xc2\xb7 TRACK ")
            + juce::String(clip.track + 1);
        bool can_split = cursor > clip.bar && cursor < clip.end();
        m.rows[0] = {"Split at bar " + juce::String(cursor + 1), "", false, false, !can_split};
        m.rows[1] = {"Duplicate after", "", false, false, false};
        m.rows[2] = {"Repeat to next marker", "", false, false, !next_marker};
        m.rows[3] = {song.loop_start == clip.bar && song.loop_end == clip.end() ? "Stop looping" : "Loop these bars", "", false, false, false};
        m.rows[4] = {"Make its own pattern", "", false, false, false};
        m.rows[5] = {"Show motion lane", "", false, false, false};
        m.rows[6] = {clip.muted ? "Unmute clip" : "Mute clip", "", false, false, false};
        m.rows[7] = {"Delete clip", "", false, true, false};
        m.count = clip_actions; m.current = s.ui.sheet_row % clip_actions;
        wide_sheet(p, m, "P" + juce::String(clip.pattern + 1), track_colour(clip.track));
        return;
    }
    frame(p, s, {{"PATTERNS", "SONG", "LENGTH"}, 1, c}, h);

    // Markers, the ruler and the loop brace.
    float label_every = zoom <= 8 ? 1 : zoom / 4.0f;
    for (unsigned m = 0; m < song.marker_count; ++m) {
        const auto& marker = song.markers[m];
        if (marker.bar < view || marker.bar >= view + zoom) continue;
        float mx = x0 + (marker.bar - view) * bw;
        p.text(marker.name, mx + 2, zone::picture_y, 80, 8, caps_text(colour::mid, 6.5f));
        p.fill(mx, zone::picture_y, 0.5f, lane_top - zone::picture_y + 8 * (lane_h + lane_gap), colour::line);
    }
    for (unsigned b = view; b < view + zoom; ++b)
        if (unsigned(b - view) % unsigned(label_every) == 0)
            p.text(juce::String(b + 1), x0 + (b - view) * bw + 2, zone::picture_y + 8, 30, 9, plain(colour::mid, 7, Weight::SemiBold));
    if (song.looping()) {
        float lx = x0 + (float(song.loop_start) - float(view)) * bw, rx = x0 + (float(song.loop_end) - float(view)) * bw;
        lx = std::max(lx, x0); rx = std::min(rx, x1);
        if (rx > lx) {
            p.fill(lx, lane_top - 3, rx - lx, 1, colour::sec);
            p.fill(lx, lane_top - 4, 1, 3, colour::sec);
            p.fill(rx - 1, lane_top - 4, 1, 3, colour::sec);
        }
    }
    // Lanes: eight tracks in view, the selected one among them.
    unsigned first = s.ui.track / 8 * 8;
    for (unsigned row = 0; row < 8; ++row) {
        unsigned t = first + row;
        float y = lane_top + row * (lane_h + lane_gap);
        auto tc = track_colour(t);
        bool selected = t == s.ui.track;
        if (selected) p.fill(zone::pad, y, 22, lane_h, tc, 3);
        p.text(juce::String(t + 1), zone::pad, y, 22, lane_h, plain(selected ? colour::bg : s.project.tracks[t].used() ? tc : colour::dim, 8.5f, Weight::Bold),
               juce::Justification::centred);
        for (unsigned n = 0; n < song.count; ++n)
            if (song.clips[n].track == t) draw_clip(p, s, song.clips[n], y, view, bw, int(n) == selected_clip);
    }
    float bottom = lane_top + 8 * (lane_h + lane_gap);
    p.fill(x0 - 1, lane_top - 2, 0.5f, bottom - lane_top + 2, colour::line);
    // The cursor, and the playhead when the song plays.
    float cx = x0 + (float(cursor) - float(view)) * bw;
    p.fill(cx, zone::picture_y + 9, 1, bottom - zone::picture_y - 9, c);
    p.fill(cx - 2, zone::picture_y + 8, 5, 5, c, 1);
    if (s.song_playing) {
        float at = float(s.song_bar) + float(s.ticks % ticks_per_bar) / ticks_per_bar;
        if (at >= view && at < view + zoom) {
            float px = x0 + (at - view) * bw;
            p.fill(px, zone::picture_y + 12, 1, bottom - zone::picture_y - 12, colour::text);
            juce::Path tri;
            tri.addTriangle(px - 3, zone::picture_y + 8, px + 4, zone::picture_y + 8, px + 0.5f, zone::picture_y + 13);
            p.g.setColour(colour::text); p.g.fillPath(tri);
        }
    }
    // The punch lane: punches written into the song, in their target's colour.
    if (song.punch_count) {
        p.text("P", zone::pad, bottom - 1, 22, 6, caps_text(colour::dim, 5.5f), juce::Justification::centred);
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
    float oy = bottom + 5, ow = x1 - x0;
    p.text(juce::String(song.length()), zone::pad, oy - 3, 22, 9, plain(colour::dim, 6.5f, Weight::SemiBold), juce::Justification::centred);
    p.fill(x0, oy, ow, 2, colour::line, 1);
    p.fill(x0 + ow * view / total, oy - 1, std::max(4.0f, ow * zoom / total), 4, colour::mid, 2);
}

Clip* clip_here(Context& c) {
    int n = c.project.song.clip_at(c.ui.track, c.ui.song_cursor);
    return n < 0 ? nullptr : &c.project.song.clips[size_t(n)];
}

void act(Context& c) {
    auto& song = c.project.song;
    int index = song.clip_at(c.ui.track, c.ui.song_cursor);
    if (index < 0) { c.ui.sheet_open = false; return; }
    Clip clip = song.clips[size_t(index)];
    unsigned cursor = c.ui.song_cursor;
    switch (c.ui.sheet_row % clip_actions) {
    case 0: // split at the cursor
        if (cursor <= clip.bar || cursor >= clip.end()) return;
        song.clips[size_t(index)].bars = uint16_t(cursor - clip.bar);
        { Clip tail = clip; tail.bar = uint16_t(cursor); tail.bars = uint16_t(clip.end() - cursor); song.place(tail); }
        break;
    case 1: { Clip copy = clip; copy.bar = clip.end(); song.place(copy); c.ui.song_cursor = copy.bar; break; }
    case 2: { // repeat until the next marker
        unsigned until = 0;
        for (unsigned m = 0; m < song.marker_count; ++m)
            if (song.markers[m].bar > clip.bar && (!until || song.markers[m].bar < until)) until = song.markers[m].bar;
        if (!until) return;
        for (unsigned at = clip.end(); at < until;) {
            Clip copy = clip; copy.bar = uint16_t(at); copy.bars = uint16_t(std::min<unsigned>(clip.bars, until - at));
            if (!song.place(copy)) break;
            at += copy.bars;
        }
        break;
    }
    case 3:
        if (song.loop_start == clip.bar && song.loop_end == clip.end()) song.loop_start = song.loop_end = 0;
        else { song.loop_start = clip.bar; song.loop_end = clip.end(); }
        break;
    case 4: { // a copy of its pattern in the first empty slot, so editing it changes this clip alone
        const auto& patterns = c.patterns.at[clip.track];
        for (unsigned n = 0; n < pattern_count; ++n) {
            bool placed = false;
            for (unsigned k = 0; k < song.count; ++k) placed |= song.clips[k].track == clip.track && song.clips[k].pattern == n;
            if (placed || !patterns[n].empty() || n == clip.pattern) continue;
            c.copy_pattern(clip.track, clip.pattern, n);
            song.clips[size_t(index)].pattern = uint8_t(n);
            break;
        }
        break;
    }
    case 5: // its motion: the Motion page on this clip's pattern
        c.launch(clip.track, clip.pattern);
        c.sync_song();
        c.open(PageId::Motion);
        return;
    case 6: song.clips[size_t(index)].muted = !clip.muted; break;
    case 7: song.erase(unsigned(index)); break;
    }
    c.ui.sheet_open = false;
    c.sync_song();
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (down && !c.ui.sheet_open) c.select_track(pad); // pads pick the lane
}

void wheel(Context& c, int delta) {
    if (c.ui.sheet_open) {
        c.ui.sheet_row = uint8_t(std::clamp<int>(c.ui.sheet_row + delta, 0, int(clip_actions) - 1));
        return;
    }
    c.ui.song_cursor = uint16_t(std::clamp<int>(c.ui.song_cursor + delta, 0, int(song_bars) - 1));
    c.ui.song_view = uint16_t(view_of(c.ui));
}

void push(Context& c) {
    if (c.ui.sheet_open) { act(c); return; }
    if (clip_here(c)) { c.ui.sheet_open = true; c.ui.sheet_row = 0; return; }
    if (!c.track().used()) return;
    Clip clip;
    clip.track = c.ui.track; clip.pattern = c.ui.song_pattern; clip.bar = c.ui.song_cursor; clip.bars = c.ui.song_length;
    if (c.project.song.place(clip)) c.sync_song();
}

void soft(Context& c, int key) {
    if (c.ui.sheet_open) {
        if (key == 0) c.ui.sheet_open = false;
        if (key == 2) act(c);
        return;
    }
    static const PageId pages[] {PageId::Patterns, PageId::Song, PageId::Length};
    c.open(pages[key % 3]);
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

void song_marker(Context& c) { c.project.song.step_marker(c.ui.song_cursor); }

const PageSpec song_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
