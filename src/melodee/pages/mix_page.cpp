// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MIX (B3): sixteen strips with level, meter and pan tick. K1-K3 are the
// selected track's level, pan and send; the wheel picks the track.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    Header h = header_of(s);
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, play_keys(s, 2), h);

    constexpr float gap = 2, top = zone::picture_y + 1.5f, sh = 143.5f - 1.5f;
    constexpr float w = (zone::width - 2 * zone::pad - 15 * gap) / 16;
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& track = s.project.tracks[t];
        float x = zone::pad + t * (w + gap), cx = x + w / 2;
        bool selected = t == s.ui.track, used = track.used();
        auto c = track_colour(t);
        p.fill(x, top, w, sh, selected ? colour::raised : colour::quiet, 3);
        if (selected) p.fill(cx - 7, top + 3, 14, 13, c, 3);
        p.text(juce::String(t + 1), cx - 10, top + 3, 20, 13,
               plain(selected ? colour::bg : used ? c : colour::dim, 8.5f, Weight::Bold), juce::Justification::centred);
        // Fader (level) and meter side by side, 95 px tall.
        float fy = top + 22, fh = 95, fx = cx - 4;
        p.fill(fx, fy, 5, fh, colour::line, 2.5f);
        float level = used ? track.mix.level / 127.0f : 0;
        if (level > 0) p.fill(fx, fy + fh * (1 - level), 5, fh * level, c, 2.5f);
        p.fill(fx + 6.5f, fy, 2, fh, colour::line, 1);
        float m = s.meters[t];
        float shown = m > 0 ? std::clamp((20 * std::log10(m) + 48) / 48, 0.0f, 1.0f) : 0;
        if (shown > 0 && used && !track.mix.mute) p.fill(fx + 6.5f, fy + fh * (1 - shown), 2, fh * shown, colour::sec, 1);
        // Pan: a tick on a short line.
        float py = fy + fh + 8;
        p.fill(cx - 6.5f, py, 13, 2, colour::line, 1);
        if (used) p.fill(cx - 6.5f + 13 * (track.mix.pan + 50) / 100.0f - 1, py - 1.5f, 2, 5, colour::sec, 1);
        if (track.mix.mute || track.mix.solo)
            p.text(track.mix.solo ? "S" : "M", x, py + 5, w, 8, caps_text(colour::text, 6.5f), juce::Justification::centred);
        if (track.mix.mute) p.fill(x, top, w, sh, colour::bg.withAlpha(0.5f), 3);
    }
}

void wheel(Context& c, int delta) { c.select_track(unsigned(int(c.ui.track) + delta + 16) % 16); }
} // namespace

const PageSpec mix_page {draw, play_pad, wheel, no_push, play_soft, wheel}; // Minus / Plus: the track, as the wheel

} // namespace melodee
