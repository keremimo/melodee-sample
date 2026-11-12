// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SCENES (roadmap P5; Push on Patterns): the bank's sixteen scenes laid out
// as the pads sit (pad 1 bottom left; Seq held + Pad Bank another bank of
// them), each a column of patterns, pattern n of every track. A pad launches its scene: pattern n on each track holding notes
// there (and on the selected track) at the next bar, the scene's tempo with
// it; Erase + pad clears its name and tempo. Push names the scene in hand,
// the wheel sets its tempo (below 30 BPM: none of its own). The cards are the
// selected track's pattern's follow action (model/follow.h): K1 FOLLOW, K2
// AFTER (bars or plays), K3 CHANCE, Shift + K1 OR (the second action, on the
// odds the first leaves); B3 ALL puts it on every track's pattern in its
// column. Minus / Plus move the track. Beside the grid: the scene in hand's
// name and tempo, the follow action in glyphs, and a dot a track (in the
// scene, playing something else, stopped by a follow action).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;

juce::String tempo_text(unsigned tenths) {
    return juce::String(tenths / 10) + (tenths % 10 ? "." + juce::String(tenths % 10) : juce::String());
}

// The follow action as glyphs: "NEXT", "→ 4 BARS", the second on its odds under it.
void draw_follow(Paint& p, const Snapshot& s, float x, float y, float w, juce::Colour c) {
    const auto& f = s.pattern.follow;
    p.text("T" + juce::String(s.ui.track % track_count + 1) + " P" + juce::String(s.track().pattern % pattern_count + 1), x, y, w, 12,
           caps_text(colour::mid));
    if (!f.on()) {
        icon(p, Icon::Loop, x, y + 16, 12, colour::dim, 1.3f); // it plays on
        p.text("OFF", x + 16, y + 14, w - 16, 16, plain(colour::dim, type::body, Weight::Medium));
        return;
    }
    juce::String after = juce::String(f.count) + (f.plays ? (f.count == 1 ? " PLAY" : " PLAYS") : (f.count == 1 ? " BAR" : " BARS"));
    juce::String first = juce::String(follow_name(f.action)) + (f.chance < 100 ? " " + juce::String(f.chance) + "%" : juce::String());
    p.text(first, x, y + 14, w, 16, p.fit(first, plain(c, type::body, Weight::SemiBold), w, type::floor));
    if (f.other && f.chance < 100)
        p.text(juce::String(follow_name(f.other)) + " " + juce::String(100 - f.chance) + "%", x, y + 30, w, 14, plain(colour::sec, type::small, Weight::Medium));
    p.text(juce::String::fromUTF8("\xe2\x86\x92 ") + after, x, y + (f.other && f.chance < 100 ? 44 : 30), w, 14, plain(colour::sec, type::small, Weight::Medium));
}

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    unsigned hand = s.ui.scene % pattern_count;
    const auto& scene = s.project.scenes[hand];
    Header h = header_of(s);
    h.left.push_back(chip("SCENES", c));
    h.left.push_back(bank_pill(s));
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{"", "", "ALL"}, -1, c};
    frame(p, s, keys, h);

    // The scenes as the pads sit: pads 13-16 along the top, 1-4 along the bottom.
    constexpr float cw = 41, ch = 32, gap = 3, grid_w = 4 * cw + 3 * gap, grid_h = 4 * ch + 3 * gap;
    float gx = zone::pad, gy = zone::picture_y + (zone::picture_h - grid_h) / 2;
    bool blink = (s.now / 150) % 2;
    for (unsigned k = 0; k < pattern_pads; ++k) {
        unsigned n = s.ui.pad_pattern(k), row = 3 - k / 4, column = k % 4, with = 0, on = 0, waiting = 0;
        for (unsigned t = 0; t < track_count; ++t) {
            const auto& tr = s.project.tracks[t];
            if (!tr.used() || !s.sketches[t][n].notes) continue;
            ++with;
            bool live = s.playing ? s.playing_patterns[t] == n && s.positions[t] >= 0 : tr.pattern == n;
            on += live;
            waiting += s.playing && !live && tr.pattern == n;
        }
        float x = gx + column * (cw + gap), y = gy + row * (ch + gap);
        bool playing = with && on == with;
        juce::Colour text = with ? colour::sec : colour::dim;
        if (playing) { p.fill(x, y, cw, ch, colour::text, 5); text = colour::bg; }
        else if (waiting) { p.fill(x, y, cw, ch, colour::text.withAlpha(blink ? 0.4f : 0.12f), 5); p.ring(x, y, cw, ch, colour::text, 5, 1.5f); text = colour::text; }
        else if (with) p.fill(x, y, cw, ch, colour::text.withAlpha(0.06f + 0.02f * float(std::min(with, 8u))), 5);
        else p.fill(x, y, cw, ch, colour::quiet, 5);
        if (n == hand) p.ring(x - 1, y - 1, cw + 2, ch + 2, c, 6, 1.5f);
        const auto& sc = s.project.scenes[n];
        p.text(juce::String(n + 1), x + 4, y + 2, cw - 8, 13, plain(text, type::small, Weight::Bold));
        if (sc.tempo) p.fill(x + cw - 7, y + 6, 3, 3, playing ? colour::bg : colour::amber, 1.5f); // a tempo of its own
        if (sc.name[0]) { // as much of its name as fits, as the MPC shortens names
            auto style = plain(text, type::small, Weight::Medium);
            juce::String name(sc.name);
            while (name.length() > 1 && p.width(name, style) > cw - 6) name = name.dropLastCharacters(1);
            p.text(name, x + 3, y + ch - 15, cw - 6, 13, style);
        }
    }

    // Beside it: the scene in hand, its tempo, the follow action of the track's pattern, the tracks.
    float x = gx + grid_w + 10, w = zone::width - zone::pad - x, y = gy;
    juce::String title = "S" + juce::String(hand + 1) + (scene.name[0] ? " " + juce::String(scene.name) : juce::String());
    p.text(title, x, y, w, 18, p.fit(title, plain(colour::text, type::list, Weight::SemiBold), w, type::floor));
    icon(p, Icon::Metro, x, y + 21, 13, scene.tempo ? colour::amber : colour::dim, 1.3f);
    p.text(scene.tempo ? tempo_text(scene.tempo) + " BPM" : juce::String::fromUTF8("\xe2\x80\x94"), x + 17, y + 19, w - 17, 16,
           plain(scene.tempo ? colour::text : colour::dim, type::body, Weight::Medium));
    p.fill(x, y + 40, w, 1, colour::line);
    draw_follow(p, s, x, y + 46, w, c);
    // A dot a track: in this scene (its colour), playing another pattern (dim), stopped by a follow action (a ring).
    float dy = gy + grid_h - 9, dot = 6, step = w / 8;
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& tr = s.project.tracks[t];
        float tx = x + (t % 8) * step, ty = dy - (t < 8 ? 10 : 0);
        if (!tr.used()) { p.fill(tx + 1, ty + 2, dot - 2, 1.5f, colour::line); continue; }
        bool stopped = s.playing && !s.song_playing && s.positions[t] < 0;
        bool in_scene = (s.playing ? s.playing_patterns[t] : tr.pattern) == hand;
        if (stopped) p.ring(tx, ty, dot, dot, colour::rec, 3, 1.2f);
        else p.fill(tx, ty, dot, dot, in_scene ? track_colour(t) : track_colour(t).withAlpha(0.3f), 3);
    }
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (!down) return;
    if (c.ui.erase) { c.clear_scene(c.ui.pad_pattern(pad)); return; } // Erase + pad: its name and tempo
    c.launch_scene(c.ui.pad_pattern(pad));
}

// The wheel: the scene in hand's tempo by a BPM, from the tempo now when it has none; under 30, none of its own.
void wheel(Context& c, int delta) {
    unsigned hand = c.ui.scene % pattern_count;
    int now = c.project.scenes[hand].tempo ? int(c.project.scenes[hand].tempo) : int(c.tempo_now()) / 10 * 10 - delta * 10;
    c.set_scene_tempo(hand, now + delta * 10);
}

void push(Context& c) { c.name_scene(c.ui.scene); }

void soft(Context& c, int key) {
    if (key == 2) c.follow_to_column();
}

void nudge(Context& c, int way) { c.select_track(unsigned(std::clamp<int>(c.ui.track + way, 0, int(track_count) - 1))); }
} // namespace

const PageSpec scenes_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
