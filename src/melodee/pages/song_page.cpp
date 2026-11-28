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
// Shift + B2 its motion, Shift + B3 gives it its own pattern (Shift + B1 opens JAMS, the jams to lay after the
// song's end: shell/jam_jobs.cpp; the foot counts them), Push opens its
// notes (on empty time, places as B3 does), Chop splits it at the cursor,
// Erase deletes it, Note Repeat repeats it to the next marker, Loop loops
// its bars. K1 zoom. Minus / Plus jump between markers (or clip edges while
// there are none). Shift + Push adds a marker at the cursor, or names it.
// Play here plays the song from the cursor; Seq Record on an audio track's lane records its input into a clip of its
// own from the cursor (shell/song_take.h), framed red while it grows. Shift + K1 sets the tempo lane at
// the cursor's bar (roadmap P5; under 30 BPM, none there), Shift + K2 its
// shape, a step on its bar or a ramp from the point before; with points the
// lane is a strip over the tracks, its tempo at the cursor beside it. Issue 60 (shell/song_range.h): Pad Bank shows
// every lane at once; Loop sets the loop range's first and last bar (twice on one bar of a clip: the clip's); with a range Shift + Erase copies
// its bars, Push pastes them as an insert, Erase takes them out; K2 steps through the patterns that hold notes.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/new_track.h"
#include "melodee/shell/song_range.h"
#include "melodee/model/print.h"
#include "melodee/model/smf.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdio>
#include <vector>

namespace melodee {
namespace {
using namespace ui;
// Six lanes at a time, tall enough for the clips' names; the window follows
// the selected track.
constexpr unsigned lanes = 6;
constexpr float x0 = 34, x1 = zone::width - zone::pad, lane_top = zone::picture_y + 29, lane_h = 15, lane_gap = 1;
constexpr float strip_h = 14; // roadmap P5: the tempo lane, over the tracks (one track lane fewer while it has points)

unsigned zoom_of(const UiState& ui) { return song_zooms[ui.song_zoom % (sizeof(song_zooms) / sizeof(song_zooms[0]))]; }
// The first bar in view: the remembered one, moved just enough to show the cursor.
unsigned view_of(const UiState& ui) {
    unsigned zoom = zoom_of(ui), view = ui.song_view;
    if (ui.song_cursor < view) view = ui.song_cursor;
    if (ui.song_cursor >= view + zoom) view = ui.song_cursor - zoom + 1;
    return view;
}
juce::String clock_of(const Song& song, unsigned bar, unsigned tempo_tenths, unsigned bar_ticks) { // through the tempo lane (roadmap P5)
    unsigned seconds = unsigned(song.tempo.seconds(bar, double(bar_ticks) / ticks_per_quarter, std::max(tempo_tenths, 300u)) + 0.5);
    char text[16];
    std::snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
    return text;
}
juce::String dash() { return juce::String::fromUTF8("\xe2\x80\x93"); }

// All tracks (issue 60): sixteen thin lanes in the six's room, every clip in its colour, no names.
void draw_thin_clip(Paint& p, const Clip& clip, float y, float h, unsigned view, float bw, bool selected) {
    float cx = x0 + (float(clip.bar) - float(view)) * bw, cw = clip.bars * bw;
    float left = std::max(cx, x0), right = std::min(cx + cw, x1);
    if (right <= left + 1) return;
    auto c = track_colour(clip.track);
    p.fill(left + 0.5f, y, right - left - 1, h, c.withAlpha(clip.muted ? 0.2f : 0.85f), 1.5f);
    if (selected) p.ring(left + 0.5f, y - 0.5f, right - left - 1, h + 1, colour::text, 1.5f);
}

void draw_clip(Paint& p, const Snapshot& s, const Clip& clip, float y, unsigned view, float bw, bool selected) {
    float cx = x0 + (float(clip.bar) - float(view)) * bw, cw = clip.bars * bw;
    float left = std::max(cx, x0), right = std::min(cx + cw, x1);
    if (right <= left + 1) return;
    auto c = track_colour(clip.track);
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(left), int(y), int(right - left + 1), int(lane_h)));
    p.fill(cx + 0.5f, y, cw - 1, lane_h, selected ? c : c.withAlpha(clip.muted ? 0.12f : 0.38f), 2.5f);
    const auto& sketch = s.sketches[clip.track][clip.pattern];
    if (sketch.audio.used()) { // freeze plan F3: an audio track's take, from the top of each pass of its pattern
        const unsigned bar = std::max(1u, s.project.bar_ticks());
        const float pass = bw * std::max<uint32_t>(sketch.length, 1) / bar;
        const float take = bw * float(frames_to_ticks(sketch.audio.frames, sketch.audio.warp ? sketch.audio.tempo : s.tempo)) / bar;
        for (float px = cx; px < cx + cw - 1; px += std::max(pass, 2.0f))
            region_wave(p, s, sketch.audio, px + 1, y + 1, std::min({take, pass, cx + cw - px}) - 2, lane_h - 2, selected ? colour::bg.withAlpha(0.55f) : c.withAlpha(0.9f));
    }
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
    if (sketch.audio.used()) { // the name on a backing, clear of the wave (freeze plan F3)
        float lw = p.width(juce::String(print_pattern_name(clip.pattern)), plain(c, type::small, Weight::Bold)) + 5;
        p.fill(cx + 1, y + 1, lw, lane_h - 2, selected ? c : colour::bg.interpolatedWith(c, clip.muted ? 0.12f : 0.38f), 2);
    }
    p.text(juce::String(print_pattern_name(clip.pattern)), cx + 3, y, 24, lane_h, plain(selected ? colour::bg : c, type::small, Weight::Bold));
    if (s.ui.song_rec_pattern == int(clip.pattern) && s.ui.audio_rec_track == int(clip.track)) // an audio take written into it
        p.ring(cx + 0.5f, y, cw - 1, lane_h, colour::rec, 2.5f);
    else if (selected) p.ring(cx + 0.5f, y, cw - 1, lane_h, colour::text, 2.5f);
    p.g.restoreState();
}

// B1-B3 follow what is under the cursor; with Shift, the clip's second jobs.
SoftKeys keys_of(const Snapshot& s, const Clip* clip, juce::Colour c) {
    if (s.ui.shift) return {{s.ui.jam_count || s.ui.jam_logging ? "JAMS" : "", clip ? "MOTION" : "", clip ? "OWN PATTERN" : ""}, -1, c};
    if (clip) return {{"EXPORT", clip->muted ? "UNMUTE" : "MUTE", "DUPLICATE"}, -1, c};
    return {{"EXPORT", "", s.project.tracks[s.ui.track].used() ? "PLACE" : ""}, -1, c};
}

// JAMS (jam capture, shell/jam_jobs.cpp): the picked jam large (its sequences' markers, bars, each track that played
// with its patterns; what FROM-TO leaves out dimmed; the place while B2 plays it), and under it the jams, newest
// first, each a row with a small picture and its length.
bool jam_heard(const Snapshot& s, unsigned t, uint8_t p) { // a pattern with something to play (TAKE gives it a clip)
    if (p == jam_silent) return false;
    const auto& sk = s.sketches[t % track_count][p % pattern_count];
    return sk.notes || sk.audio.used();
}
std::vector<unsigned> jam_tracks(const Snapshot& s, const JamLog& jam) { // the tracks that played in it, in order
    std::vector<unsigned> out;
    for (unsigned t = 0; t < track_count; ++t) {
        bool played = false;
        for (const auto& b : jam.bars) if (jam_heard(s, t, b.pattern[t])) { played = true; break; }
        if (played) out.push_back(t);
    }
    return out;
}
void draw_jam_lanes(Paint& p, const Snapshot& s, const JamLog& jam, float x, float y, float w, float h, bool labels) {
    const auto tracks = jam_tracks(s, jam);
    if (tracks.empty() || jam.bars.empty()) return;
    const unsigned bars = unsigned(jam.bars.size());
    const float bw = w / float(bars), lh = std::min(h / float(tracks.size()), 13.0f);
    for (size_t k = 0; k < tracks.size(); ++k) {
        const unsigned t = tracks[k];
        const float ly = y + float(k) * lh;
        const auto c = track_colour(t);
        p.fill(x, ly + 0.25f, w, std::max(lh - 0.5f, 0.5f), colour::quiet, 1);
        for (unsigned b = 0; b < bars;) {
            const uint8_t pat = jam.bars[b].pattern[t];
            const bool muted = jam.bars[b].muted >> t & 1;
            unsigned e = b + 1;
            while (e < bars && jam.bars[e].pattern[t] == pat && bool(jam.bars[e].muted >> t & 1) == muted) ++e;
            if (jam_heard(s, t, pat)) {
                const float bx = x + float(b) * bw, bwid = float(e - b) * bw;
                if (muted) p.ring(bx + 0.5f, ly + 0.25f, std::max(bwid - 1, 1.0f), std::max(lh - 0.5f, 0.5f), c.withAlpha(0.6f), 1.5f, 0.8f);
                else p.fill(bx + 0.5f, ly + 0.25f, std::max(bwid - 1, 1.0f), std::max(lh - 0.5f, 0.5f), c.withAlpha(0.55f), 1.5f);
                if (labels && !muted && lh >= 11 && bwid >= 18)
                    p.text("P" + juce::String(pat % pattern_count + 1), bx + 3, ly, bwid - 4, lh, plain(colour::bg, type::small, Weight::Bold));
            }
            b = e;
        }
    }
}
void draw_jams(Paint& p, const Snapshot& s) {
    const JamList* list = s.jams.get();
    const unsigned count = list ? unsigned(list->jams.size()) : 0;
    const unsigned pick = count ? std::min<unsigned>(s.ui.jam_pick, count - 1) : 0;
    SoftKeys keys {{"BACK", s.playing || s.ui.jam_audition ? "STOP" : "PLAY", "TAKE"}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    Header h = header_of(s);
    h.left.push_back(pill("JAMS", colour::text));
    if (count) h.left.push_back(pill("JAM " + juce::String(list->jams[pick].number)));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, keys, h);
    if (!count) return;
    const JamLog& jam = list->jams[pick];
    const unsigned bars = std::max<unsigned>(unsigned(jam.bars.size()), 1);
    const float x = zone::pad, w = x1 - x, bw = w / float(bars);
    const float marks_y = zone::picture_y, ruler_y = marks_y + 12, lanes_y = ruler_y + 14, lanes_h = 60;
    // The sequences that took over, named as Song's markers will be (only where the name fits before the next).
    std::vector<std::pair<unsigned, juce::String>> marks;
    for (unsigned b = 0; b < jam.bars.size(); ++b) {
        const int n = jam.bars[b].scene;
        if (n < 0) continue;
        const auto& scene = s.project.scenes[unsigned(n) % pattern_count];
        marks.emplace_back(b, scene.name[0] ? juce::String(scene.name) : "SEQ " + juce::String(n % int(pattern_count) + 1));
    }
    for (size_t m = 0; m < marks.size(); ++m) {
        const float mx = x + float(marks[m].first) * bw;
        const float room = (m + 1 < marks.size() ? x + float(marks[m + 1].first) * bw : x1) - mx - 3;
        p.fill(mx, marks_y, 0.5f, lanes_y + lanes_h - marks_y, colour::line);
        if (p.width(marks[m].second.toUpperCase(), caps_text(colour::mid)) <= room)
            p.text(marks[m].second, mx + 2, marks_y, room, 11, caps_text(colour::mid));
    }
    unsigned every = 1; // bar numbers at least 24 px apart
    while (float(every) * bw < 24) every *= 2;
    for (unsigned b = 0; b < jam.bars.size(); b += every)
        p.text(juce::String(b + 1), x + float(b) * bw + 2, ruler_y, 30, 11, plain(colour::mid, type::small, Weight::SemiBold));
    draw_jam_lanes(p, s, jam, x, lanes_y, w, lanes_h, true);
    // FROM-TO: the rest dimmed, a line over the part.
    const unsigned from = std::min<unsigned>(s.ui.jam_from, bars - 1), to = std::clamp<unsigned>(s.ui.jam_to, from + 1, bars);
    if (from > 0) p.fill(x, lanes_y, float(from) * bw, lanes_h, colour::bg.withAlpha(0.72f));
    if (to < bars) p.fill(x + float(to) * bw, lanes_y, float(bars - to) * bw, lanes_h, colour::bg.withAlpha(0.72f));
    p.fill(x + float(from) * bw, lanes_y - 2.5f, float(to - from) * bw, 1.5f, colour::text);
    if (s.ui.jam_audition && s.song_playing) { // where B2's playing is
        const unsigned bar = std::max(1u, s.project.bar_ticks());
        const float at = float(from) + float(s.song_bar) + float(s.ticks % bar) / float(bar);
        if (at < float(to)) p.fill(x + at * bw, lanes_y - 3, 1, lanes_h + 3, colour::text);
    }
    // The jams, newest first: three rows, the picked one among them.
    constexpr unsigned rows = 3;
    constexpr float row_h = 17, row_gap = 1;
    const float rows_y = lanes_y + lanes_h + 4;
    const unsigned picked_row = count - 1 - pick;
    const unsigned first = std::min(picked_row > 0 ? picked_row - 1 : 0, count > rows ? count - rows : 0);
    for (unsigned r = first; r < std::min(first + rows, count); ++r) {
        const JamLog& row = list->jams[count - 1 - r];
        const float ry = rows_y + float(r - first) * (row_h + row_gap);
        const bool on = r == picked_row;
        if (on) { p.fill(zone::pad - 2, ry, w + 4, row_h, colour::surf, 3); p.fill(zone::pad - 2, ry + 3, 2, row_h - 6, colour::text, 1); }
        p.text(juce::String(row.number), zone::pad, ry, 20, row_h, plain(on ? colour::text : colour::sec, type::body, Weight::SemiBold), juce::Justification::centred);
        juce::String length = juce::String(row.bars.size()) + (row.bars.size() == 1 ? " BAR" : " BARS");
        const float lw = p.width(length, caps_text(colour::sec)) + 1;
        p.text(length, x1 - lw, ry, lw, row_h, caps_text(on ? colour::text : colour::sec));
        draw_jam_lanes(p, s, row, zone::pad + 24, ry + 3, x1 - lw - 8 - (zone::pad + 24), row_h - 6, false);
    }
}

void draw(Paint& p, const Snapshot& s) {
    if (s.ui.jam_sheet) { draw_jams(p, s); return; }
    const auto& song = s.project.song;
    auto c = colour_of(s);
    unsigned zoom = zoom_of(s.ui), view = view_of(s.ui), cursor = s.ui.song_cursor;
    float bw = (x1 - x0) / zoom;
    int selected_clip = song.clip_at(s.ui.track, cursor);
    Header h = header_of(s);
    h.left.push_back(pill("BAR " + juce::String(cursor + 1) + juce::String::fromUTF8(" \xc2\xb7 ") + clock_of(song, cursor, s.project.tempo, s.project.bar_ticks())));
    if (zoom == song_zooms[0]) h.left.push_back(pill("SNAP 1 BAR"));
    // Following MIDI clock or MTC in, its chip takes the loop's room (the ruler's brace still shows the loop).
    else if (song.looping() && !s.following) h.left.push_back(pill("LOOP " + juce::String(song.loop_start + 1) + dash() + juce::String(song.loop_end), colour::text));
    if (s.ui.song_all) h.left.push_back(pill("ALL", colour::text)); // issue 60: every lane at once (PAD BANK)
    if (s.ui.copying == UiState::Copying::Bars) h.left.push_back(pill("COPY", c)); // Push pastes the copied bars
    h.right.push_back(tag_of(s, s.ui.track));

    if (s.ui.song_export) {
        // Export (B1), as on the MPC: B2 an audio mixdown, B3 a new pattern; a tile under each says what it makes.
        // Roadmap P4: the wheel picks what B2 makes: the mixdown as float, 24 or 16 bits, the stems, a MIDI file, an
        // MPC 3 project or an Ableton Live Set.
        static const char* const b2[] {"MIXDOWN", "MIXDOWN", "MIXDOWN", "STEMS", "MIDI FILE", "MPC 3", "LIVE SET"};
        static_assert(std::size(b2) == export_kinds::count);
        unsigned kind = s.ui.export_kind % export_kinds::count;
        const bool project_file = kind == export_kinds::mpc3 || kind == export_kinds::live; // a project of files: its tracks
        SoftKeys keys {{"CANCEL", b2[kind], "PATTERN"}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
        const Icon made = kind == export_kinds::live ? Icon::Laptop : kind == export_kinds::mpc3 ? Icon::Mpc
                          : kind == export_kinds::midi ? Icon::Note : kind == export_kinds::stems ? Icon::Layers : Icon::Wav;
        keys.glyphs = {Icon::None, made, Icon::Pattern};
        frame(p, s, keys, h);
        p.dim_zone(zone::picture_y, zone::picture_h, 0.86f);
        p.dim_zone(zone::knobs_y, zone::knobs_h);
        unsigned bars = song.length(), seconds = unsigned(song.tempo.seconds(bars, double(s.project.bar_ticks()) / ticks_per_quarter, std::max(300u, unsigned(s.project.tempo))) + 0.5);
        if (!bars && kind < export_kinds::midi) { // nothing to export yet (a MIDI file and the projects take the patterns)
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
        unsigned stems = 0;
        for (const auto& tr : s.project.tracks) stems += tr.used();
        static_assert(smf_ppq == 960, "the MIDI file's clock, as the label says");
        static const char* const formats[] {"32-BIT FLOAT", "24-BIT DITHER", "16-BIT DITHER", "", "SMF 960 PPQ", "PROJECT", "ABLETON LIVE"};
        juce::String sub = kind == export_kinds::stems ? juce::String(stems) + " STEMS" : juce::String(formats[kind]);
        juce::String value = project_file ? juce::String(stems) + (stems == 1 ? " TRACK" : " TRACKS") : bars ? length : juce::String("P");
        key_tiles(p, {KeyTile {}, value_tile(made, value, sub, true), // the wheel picks
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
    if (s.ui.song_loop_in >= 0 && unsigned(s.ui.song_loop_in) >= view && unsigned(s.ui.song_loop_in) < view + zoom) { // issue 60: the start, waiting for its end
        const float lx = x0 + (float(s.ui.song_loop_in) - float(view)) * bw;
        const unsigned a = std::min<unsigned>(unsigned(s.ui.song_loop_in), cursor), b = std::max<unsigned>(unsigned(s.ui.song_loop_in), cursor) + 1;
        const float from = std::max(x0, x0 + (float(a) - float(view)) * bw), to = std::min(x1, x0 + (float(b) - float(view)) * bw);
        if (to > from) p.fill(from, lane_top - 3, to - from, 1, colour::text.withAlpha(0.5f)); // where it would loop
        p.fill(lx, lane_top - 6, 1, 5, colour::text);
    }
    // Roadmap P5: the tempo lane, a strip over the tracks: its line across the view (a step straight up or down on
    // its bar, a ramp sloping from the point before), a dot a point (the cursor's ringed), the tempo at the cursor.
    bool tempo_lane = !song.tempo.empty();
    float tracks_top = lane_top + (tempo_lane ? strip_h + 2 : 0);
    unsigned shown = tempo_lane ? lanes - 1 : lanes;
    if (tempo_lane) {
        const auto& lane = song.tempo;
        double low = lane.at(view, s.project.tempo), high = low; // the tempos in view, at least 10 BPM tall
        auto take = [&](double t) { low = std::min(low, t); high = std::max(high, t); };
        take(lane.at(view + zoom, s.project.tempo));
        for (unsigned n = 0; n < lane.count; ++n)
            if (lane.points[n].bar > view && lane.points[n].bar < view + zoom) { take(lane.points[n].tempo); take(lane.at(lane.points[n].bar - 0.001, s.project.tempo)); }
        if (high - low < 100) { double mid = (high + low) / 2; low = mid - 50; high = mid + 50; }
        float sy = lane_top, sh = strip_h;
        auto y_of = [&](double tempo) { return sy + sh - 2 - float((tempo - low) / (high - low)) * (sh - 4); };
        auto x_of = [&](double bar) { return x0 + float(bar - view) * bw; };
        p.fill(x0, sy, x1 - x0, sh, colour::quiet, 2);
        juce::Path line;
        double now = lane.at(view, s.project.tempo);
        line.startNewSubPath(x0, y_of(now));
        for (unsigned n = 0; n < lane.count; ++n) {
            const auto& pt = lane.points[n];
            if (pt.bar <= view) continue;
            if (pt.bar >= view + zoom) { // past the view: where its ramp has got to at the view's end
                if (pt.ramp) line.lineTo(x1, y_of(lane.at(view + zoom, s.project.tempo)));
                else line.lineTo(x1, y_of(now));
                now = -1;
                break;
            }
            float px = x_of(pt.bar);
            if (pt.ramp) line.lineTo(px, y_of(pt.tempo));
            else { line.lineTo(px, y_of(now)); line.lineTo(px, y_of(pt.tempo)); }
            now = pt.tempo;
        }
        if (now >= 0) line.lineTo(x1, y_of(now));
        p.g.setColour(colour::amber);
        p.g.strokePath(line, juce::PathStrokeType(1.5f));
        for (unsigned n = 0; n < lane.count; ++n) {
            const auto& pt = lane.points[n];
            if (pt.bar < view || pt.bar >= view + zoom) continue;
            float px = x_of(pt.bar), py = y_of(pt.tempo);
            if (pt.bar == cursor) p.ring(px - 3.5f, py - 3.5f, 7, 7, colour::text, 3.5f, 1.2f);
            p.fill(px - 2, py - 2, 4, 4, colour::amber, 2);
        }
        double here = lane.at(cursor, s.project.tempo);
        p.text(juce::String(int(std::lround(here / 10))), zone::pad, sy, 26, sh, plain(colour::amber, type::small, Weight::SemiBold),
               juce::Justification::centred);
    }
    // All tracks (issue 60): the sixteen lanes in the room of the six, the selected one's mark in its colour.
    if (s.ui.song_all) {
        const float room = shown * (lane_h + lane_gap), row_h = room / track_count, h = std::max(2.0f, row_h - 1);
        for (unsigned t = 0; t < track_count; ++t) {
            const float y = tracks_top + t * row_h;
            const bool selected = t == s.ui.track;
            p.fill(zone::pad, y, selected ? 22 : 6, h, selected ? track_colour(t) : s.project.tracks[t].used() ? track_colour(t).withAlpha(0.5f) : colour::quiet, 1);
            for (unsigned n = 0; n < song.count; ++n)
                if (song.clips[n].track == t) draw_thin_clip(p, song.clips[n], y, h, view, bw, selected && int(n) == selected_clip);
        }
    }
    // Lanes: six tracks in view (five under the tempo lane), the selected one among them.
    unsigned first = std::min<unsigned>(s.ui.track >= 2 ? s.ui.track - 2 : 0, track_count - shown);
    for (unsigned row = 0; row < shown && !s.ui.song_all; ++row) {
        unsigned t = first + row;
        float y = tracks_top + row * (lane_h + lane_gap);
        auto tc = track_colour(t);
        bool selected = t == s.ui.track;
        if (selected) p.fill(zone::pad, y, 22, lane_h, tc, 3);
        p.text(juce::String(t + 1), zone::pad, y, 22, lane_h, plain(selected ? colour::bg : s.project.tracks[t].used() ? tc : colour::dim, type::small, Weight::Bold),
               juce::Justification::centred);
        for (unsigned n = 0; n < song.count; ++n)
            if (song.clips[n].track == t) draw_clip(p, s, song.clips[n], y, view, bw, int(n) == selected_clip);
    }
    float bottom = tracks_top + shown * (lane_h + lane_gap);
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
    // The whole song, with the window on it, after its length in bars.
    unsigned total = std::max({song.length(), view + zoom, 16u});
    juce::String length = juce::String(song.length()) + (song.length() == 1 ? " BAR" : " BARS");
    auto length_style = caps_text(colour::dim);
    float oy = bottom + (song.punch_count ? 13 : 6), lw = p.width(length, length_style) + 1, sx = zone::pad + lw + 6, ow = x1 - sx;
    p.text(length, zone::pad, oy - 5.5f, lw, 11, length_style);
    if (s.ui.jam_count || s.ui.jam_logging) { // jam capture: the jams kept (JAMS, Shift + B1), a red dot while one is logged
        juce::String jam = juce::String(s.ui.jam_count) + (s.ui.jam_count == 1 ? " JAM" : " JAMS");
        float jw = p.width(jam, length_style) + 1, jx = x1 - jw;
        p.text(jam, jx, oy - 5.5f, jw, 11, caps_text(colour::sec));
        if (s.ui.jam_logging) { jx -= 8; p.circle(jx + 2.5f, oy + 1, 2.5f, colour::rec); }
        ow = jx - 8 - sx;
    }
    p.fill(sx, oy, ow, 2, colour::line, 1);
    p.fill(sx + ow * view / total, oy - 1, std::max(4.0f, ow * zoom / total), 4, colour::mid, 2);
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
    c.open(motion ? PageId::Motion : engine == EngineKind::Sampler ? PageId::StepEdit : PageId::Roll);
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (!down || c.ui.jam_sheet) return; // JAMS: no lanes to pick
    c.select_track(pad); // pads pick the lane
    offer_new_track(c);  // an empty one: its doors, then back here
}

void wheel(Context& c, int delta) {
    if (c.ui.jam_sheet) { c.pick_jam(delta); return; } // JAMS: an older (+) or newer (-) jam
    if (c.ui.song_export) { c.ui.export_kind = uint8_t(std::clamp(int(c.ui.export_kind) + delta, 0, export_kinds::count - 1)); return; } // what B2 makes (roadmap P4)
    c.ui.song_cursor = uint16_t(std::clamp<int>(c.ui.song_cursor + delta, 0, int(song_bars) - 1));
    c.ui.song_view = uint16_t(view_of(c.ui));
}

// Issue 60: the copied bars at the cursor, what is there moved on to make room; again for the next copy after it.
void paste(Context& c) {
    auto& ui = c.ui;
    const auto& section = c.song_section;
    if (!song_paste(c.project.song, section, ui.song_cursor)) { c.notice("Paste: the song is full"); return; }
    ui.song_cursor = uint16_t(std::min<unsigned>(ui.song_cursor + section.bars, song_bars - 1));
    ui.song_view = uint16_t(view_of(ui));
    c.sync_song();
    c.notice("Pasted " + std::to_string(section.bars) + (section.bars == 1 ? " bar" : " bars"));
}

void push(Context& c) {
    if (c.ui.jam_sheet) { c.take_jam(); return; } // JAMS: as B3
    if (c.ui.song_export) return;
    if (c.ui.copying == UiState::Copying::Bars) { paste(c); return; } // issue 60
    int index = clip_index(c);
    if (index >= 0) open_clip(c, index, false);
    else place(c);
}

void soft(Context& c, int key) {
    if (c.ui.song_export) { c.song_export_soft(key); return; } // B1 Cancel, B2 Audio mixdown, B3 New pattern
    if (c.ui.jam_sheet) { // JAMS: B1 back to Song, B2 play the jam (or stop), B3 take it
        if (key == 0 && !c.ui.shift) c.open_jams(false);
        if (key == 1 && !c.ui.shift) c.audition_jam();
        if (key == 2 && !c.ui.shift) c.take_jam();
        return;
    }
    int index = clip_index(c);
    if (c.ui.shift) {
        if (key == 0) { c.open_jams(true); return; } // jam capture: the jams (shell/jam_jobs.cpp)
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
    if (c.ui.jam_sheet) { c.pick_jam(way); return; } // JAMS: as the wheel
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
    if (c.ui.jam_sheet) return; // JAMS: the song is not in view
    if (c.project.song.marker_at(c.ui.song_cursor) >= 0) c.name_marker(c.ui.song_cursor);
    else c.project.song.step_marker(c.ui.song_cursor);
}

// Chop: the clip in two at the cursor.
void song_chop(Context& c) {
    if (c.ui.jam_sheet) return; // JAMS: the song is not in view
    auto& song = c.project.song;
    int index = clip_index(c);
    if (index < 0 || c.ui.song_cursor <= song.clips[size_t(index)].bar) { c.notice("Chop: cursor inside a clip"); return; }
    if (song.count >= song.clips.size()) { c.notice("Chop: the song is full"); return; } // the tail would be lost
    Clip clip = song.clips[size_t(index)];
    unsigned cursor = c.ui.song_cursor;
    song.clips[size_t(index)].bars = uint16_t(cursor - clip.bar);
    Clip tail = clip;
    tail.bar = uint16_t(cursor); tail.bars = uint16_t(clip.end() - cursor);
    song.place(tail);
    c.sync_song();
}

// Shift + Erase: the clip under the cursor again after itself, as DUPLICATE.
void song_copy(Context& c) {
    if (c.ui.jam_sheet) return; // JAMS: the song is not in view
    int index = clip_index(c);
    if (index >= 0) duplicate(c, index);
}

void song_erase(Context& c) {
    if (c.ui.jam_sheet) { c.erase_jam(); return; } // JAMS: the picked jam goes (one undo)
    auto& song = c.project.song;
    if (song.looping()) { // issue 60: the loop range's bars out, what comes after moved back (the lane, or every lane)
        const unsigned from = song.loop_start, to = song.loop_end;
        if (!song_remove_bars(song, from, to, song_lanes(c.ui.song_all, c.ui.track))) { c.notice("Erase: the song is full"); return; }
        song.loop_start = song.loop_end = 0;
        c.ui.song_loop_in = -1;
        c.ui.song_cursor = uint16_t(from);
        c.ui.song_view = uint16_t(view_of(c.ui));
        c.sync_song();
        c.notice("Bars " + std::to_string(from + 1) + "-" + std::to_string(to) + " out");
        return;
    }
    int index = clip_index(c);
    if (index < 0) return;
    c.project.song.erase(unsigned(index));
    c.sync_song();
}

// Note Repeat: the clip again and again up to the next marker.
void song_repeat(Context& c) {
    if (c.ui.jam_sheet) return; // JAMS: the song is not in view
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

// Loop (issue 60), on a clip or not: the first tap the loop's start at the cursor, the next its end (the cursor's bar
// the last in it, either way round). Two taps on one bar of a clip, the cursor not moved: the clip's bars. With a loop
// on, a tap turns it off.
bool song_loop(Context& c) {
    if (c.ui.jam_sheet) return true; // JAMS: the song is not in view
    auto& song = c.project.song;
    auto& ui = c.ui;
    switch (song_loop_tap(song, ui.song_loop_in, ui.song_cursor, clip_index(c))) { // shell/song_range.h
    case SongLoopTap::Set: c.sync_song(); c.notice("Loop " + std::to_string(song.loop_start + 1) + "-" + std::to_string(song.loop_end)); break;
    case SongLoopTap::Off: c.sync_song(); c.notice("Loop off"); break;
    case SongLoopTap::Start: c.notice("Loop from " + std::to_string(ui.song_cursor + 1) + ": LOOP at end"); break;
    }
    return true;
}

// Shift + Erase with a loop range (issue 60): its bars kept to paste, the lane's or, in the all-tracks view, every lane's.
bool song_copy_bars(Context& c) {
    const auto& song = c.project.song;
    if (!song.looping()) return false;
    c.song_section = song_copy_range(song, song.loop_start, song.loop_end, song_lanes(c.ui.song_all, c.ui.track));
    const unsigned bars = c.song_section.bars;
    c.notice("Copy " + std::to_string(bars) + (bars == 1 ? " bar" : " bars") + ": Push pastes");
    return true;
}

void song_view_all(Context& c) {
    c.ui.song_all = !c.ui.song_all;
    c.notice(c.ui.song_all ? "All tracks" : "Track " + std::to_string(c.ui.track % track_count + 1));
}

const PageSpec song_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
