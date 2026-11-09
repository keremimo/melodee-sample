// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// STAGE (Sample): sixteen tracks drawn as the pads sit, pad 1 bottom left.
// Each cell shows engine, sound, the pattern playing and level. The wheel
// picks the track, Push opens its sound (or, on an empty cell, starts one).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    Header h = header_of(s);
    h.left.push_back(pill(juce::String(s.project.used_tracks()) + " OF 16 TRACKS"));
    h.right.push_back(label(s.project.name));
    frame(p, s, play_keys(s, 0), h);

    constexpr float gap = 3, top = zone::picture_y + 1.5f;
    constexpr float w = (zone::width - 2 * zone::pad - 3 * gap) / 4, ch = (143.5f - 1.5f - 3 * gap) / 4;
    for (unsigned t = 0; t < track_count; ++t) {
        unsigned row = 3 - t / 4, column = t % 4; // pad 1 bottom left
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        const auto& track = s.project.tracks[t];
        auto number = juce::String(t + 1);
        if (!track.used()) {
            p.fill(x, y, w, ch, colour::quiet, 4);
            p.text(number, x + 5, y + 3.5f, 30, 12, plain(colour::dim, 11, Weight::Bold));
            p.text("EMPTY", x + 5, y + ch - 13, w - 10, 10, caps_text(colour::dim, 6.5f));
            continue;
        }
        bool selected = t == s.ui.track;
        auto c = track_colour(t);
        auto fg = selected ? colour::bg : c;
        p.fill(x, y, w, ch, selected ? c : colour::surf, 4);
        float nw = p.text(number, x + 5, y + 3.5f, 30, 12, plain(fg, 11, Weight::Bold));
        juce::String state = track.mix.mute ? "MUTE" : (s.playing && s.has_notes[t] ? "P" + juce::String(track.pattern + 1) : "");
        auto state_style = caps_text(selected ? colour::bg : colour::sec, 6.5f);
        float sw = state.isEmpty() ? 0 : p.width(state, state_style) + 4;
        if (state.isNotEmpty())
            p.text(state, x + 5, y + 3.5f, w - 10, 12, state_style, juce::Justification::centredRight);
        float room = w - 10 - nw - 4 - sw;
        auto engine = p.fit(engine_info(track.engine).name, caps_text(fg, 6.5f), room, 5);
        p.text(engine_info(track.engine).name, x + 5 + nw + 4, y + 3.5f, room + 1, 12, engine);
        auto name = p.fit(track.name, plain(selected ? colour::bg : colour::text, 8.5f, Weight::Medium), w - 10, 6.5f);
        p.text(track.name, x + 5, y + 15, w - 10, 10, name);
        float level = track.mix.level / 127.0f;
        p.fill(x + 5, y + ch - 5.5f, w - 10, 2, selected ? juce::Colours::black.withAlpha(0.2f) : colour::line, 1);
        p.fill(x + 5, y + ch - 5.5f, (w - 10) * level, 2, fg, 1);
        if (track.mix.mute) p.fill(x, y, w, ch, colour::bg.withAlpha(0.5f), 4);
    }
}

void wheel(Context& c, int delta) { c.select_track(unsigned(int(c.ui.track) + delta + 16) % 16); }

void push(Context& c) {
    if (!c.track().used()) {
        // An empty cell: the first engine that is built, with its first sound.
        c.load_sound(c.ui.track, EngineKind::Prophet, 0);
        c.open(PageId::Sounds);
        return;
    }
    c.open(c.track().engine == EngineKind::Sampler ? PageId::Trim : PageId::Edit);
}
} // namespace

const PageSpec stage_page {draw, play_pad, wheel, push, play_soft, octave};

} // namespace melodee
