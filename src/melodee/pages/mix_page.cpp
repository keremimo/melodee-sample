// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MIX (B3): eight strips at a time (the half with the selected track) with
// level, meter and pan tick. K1-K3 are the
// selected track's level, pan and send; the wheel picks the track.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    Header h = header_of(s);
    h.left.push_back(track_half_pill(s));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, play_keys(s, 2), h);

    constexpr float gap = 4, top = zone::picture_y + 2, sh = zone::picture_h - 4;
    constexpr float w = (zone::width - 2 * zone::pad - 7 * gap) / 8;
    unsigned first = track_half(s) * 8;
    for (unsigned n = 0; n < 8; ++n) {
        unsigned t = first + n;
        const auto& track = s.project.tracks[t];
        float x = zone::pad + n * (w + gap), cx = x + w / 2;
        bool selected = t == s.ui.track, used = track.used();
        auto c = track_colour(t);
        p.fill(x, top, w, sh, selected ? colour::raised : colour::quiet, 3);
        if (selected) p.fill(cx - 12, top + 4, 24, 18, c, 3);
        p.text(juce::String(t + 1), x, top + 4, w, 18,
               plain(selected ? colour::bg : used ? c : colour::dim, 15, Weight::Bold), juce::Justification::centred);
        // Fader (level) and meter side by side.
        float fy = top + 28, fh = 80, fx = cx - 5;
        p.fill(fx, fy, 6, fh, colour::line, 3);
        float level = used ? track.mix.level / 127.0f : 0;
        if (level > 0) p.fill(fx, fy + fh * (1 - level), 6, fh * level, c, 3);
        p.fill(fx + 8, fy, 2, fh, colour::line, 1);
        float m = s.meters[t];
        float shown = m > 0 ? std::clamp((20 * std::log10(m) + 48) / 48, 0.0f, 1.0f) : 0;
        if (shown > 0 && used && !track.mix.mute) p.fill(fx + 8, fy + fh * (1 - shown), 2, fh * shown, colour::sec, 1);
        // Pan: a tick on a short line.
        float py = fy + fh + 8;
        p.fill(cx - 12, py, 24, 2, colour::line, 1);
        if (used) p.fill(cx - 12 + 24 * (track.mix.pan + 50) / 100.0f - 1, py - 2, 2, 6, colour::sec, 1);
        if (track.mix.mute || track.mix.solo)
            p.text(track.mix.solo ? "S" : "M", x, py + 6, w, 14, caps_text(colour::text), juce::Justification::centred);
        if (track.mix.mute) p.fill(x, top, w, sh, colour::bg.withAlpha(0.5f), 3);
    }
}

void wheel(Context& c, int delta) { c.select_track(unsigned(int(c.ui.track) + delta + 16) % 16); }
} // namespace

const PageSpec mix_page {draw, play_pad, wheel, no_push, play_soft, wheel}; // Minus / Plus: the track, as the wheel

} // namespace melodee
