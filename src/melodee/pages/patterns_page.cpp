// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PATTERNS (Loop): tracks down, sixteen patterns across, eight tracks in
// view; the wheel moves the selected track and the view follows. The pads
// are that track's sixteen patterns: tap to queue (it starts on the next
// bar), Erase + pad clears, hold a pad and tap another to copy. Shift + pad
// launches that column on every track (open question 8, built as drawn).
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    unsigned first = s.ui.track / 8 * 8;
    Header h = header_of(s);
    h.left.push_back(pill(juce::String(first + 1) + juce::String::fromUTF8("\xe2\x80\x93") + juce::String(first + 8) + " OF 16"));
    h.left.push_back(pill("LAUNCH BAR"));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"PATTERNS", "SONG", "LENGTH"}, 0, colour_of(s)}, h);

    constexpr float gap = 2, label_w = 16, head_h = 9, top = zone::picture_y + 1.5f;
    constexpr float cw = (zone::width - 2 * zone::pad - label_w - 16 * gap) / 16;
    constexpr float rh = (143.5f - 1.5f - head_h - 8 * gap) / 8;
    unsigned shown = s.playing ? s.playing_patterns[s.ui.track] : s.track().pattern;
    for (unsigned n = 0; n < pattern_count; ++n)
        p.text(juce::String(n + 1), zone::pad + label_w + gap + n * (cw + gap), top, cw, head_h,
               plain(n == shown ? colour::text : colour::dim, 7, Weight::SemiBold), juce::Justification::centred);
    for (unsigned row = 0; row < 8; ++row) {
        unsigned t = first + row;
        const auto& track = s.project.tracks[t];
        auto c = track_colour(t);
        float y = top + head_h + gap + row * (rh + gap);
        bool selected = t == s.ui.track;
        if (selected) p.fill(zone::pad, y, label_w, rh, c, 3);
        p.text(juce::String(t + 1), zone::pad, y, label_w, rh, plain(selected ? colour::bg : c, 9, Weight::Bold),
               juce::Justification::centred);
        unsigned live = s.playing ? s.playing_patterns[t] : track.pattern;
        for (unsigned n = 0; n < pattern_count; ++n) {
            float x = zone::pad + label_w + gap + n * (cw + gap);
            const auto& pattern = s.sketches[t][n];
            bool notes = track.used() && pattern.notes;
            bool playing = track.used() && n == live && notes;
            bool queued = track.used() && s.playing && n == track.pattern && n != live;
            if (playing) {
                p.fill(x, y, cw, rh, c, 3);
                if (s.playing && s.steps[t] >= 0) { // how far through the pattern
                    float done = float(s.steps[t] + 1) / pattern.length;
                    p.g.saveState();
                    p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), int(cw + 1), int(rh)));
                    p.fill(x, y + rh - 2.5f, cw * done, 2.5f, colour::bg.withAlpha(0.5f));
                    p.g.restoreState();
                }
            } else if (queued) {
                p.fill(x, y, cw, rh, c.withAlpha(0.2f), 3);
                p.ring(x, y, cw, rh, c, 3);
            } else if (notes) {
                p.fill(x, y, cw, rh, c.withAlpha(0.2f), 3);
                const float widths[] {8, 5, 6.5f};
                for (unsigned k = 0; k < 3; ++k) p.fill(x + 3, y + rh / 2 - 4 + k * 3, std::min(widths[k], cw - 5), 1.5f, c, 0.75f);
            } else {
                p.fill(x, y, cw, rh, colour::quiet, 3);
            }
        }
    }
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    auto& ui = c.ui;
    if (down) {
        if (ui.erase) { c.edit_pattern(ui.track, pad, "Clear pattern", [&] { c.clear_pattern(ui.track, pad); }); return; }
        if (ui.shift) { // Shift + pad: that column on every track
            for (unsigned t = 0; t < track_count; ++t) if (c.project.tracks[t].used()) c.launch(t, pad);
            ui.shift_used = true;
            return;
        }
        if (ui.pattern_held >= 0 && ui.pattern_held != int(pad)) { // hold one, tap another: copy
            c.edit_pattern(ui.track, pad, "Copy pattern", [&] { c.copy_pattern(ui.track, unsigned(ui.pattern_held), pad); });
            ui.pattern_copied = true;
            return;
        }
        ui.pattern_held = int8_t(pad);
        ui.pattern_copied = false;
        return;
    }
    if (ui.pattern_held != int(pad)) return;
    if (!ui.pattern_copied) c.launch(ui.track, pad); // a tap queues it
    ui.pattern_held = -1;
}

void wheel(Context& c, int delta) { c.select_track(unsigned(std::clamp<int>(c.ui.track + delta, 0, int(track_count) - 1))); }

void soft(Context& c, int key) {
    static const PageId pages[] {PageId::Patterns, PageId::Song, PageId::Length};
    c.open(pages[key % 3]);
}
} // namespace

const PageSpec patterns_page {draw, pad, wheel, no_push, soft, octave};

} // namespace melodee
