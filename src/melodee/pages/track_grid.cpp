// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The sixteen tracks as the pads sit: Stage's picture, and the screen while
// Pad Bank is held (its pads pick a track) or a track copy picks its tracks.
#include "melodee/pages/common.h"
#include "melodee/shell/pad_bank.h"

namespace melodee {
using namespace ui;

// Eight tracks at a time, the half holding the selected track: tracks 1-8
// sit as pads 1-8, tracks 9-16 as pads 9-16, two rows of four.
void track_grid(Paint& p, const Snapshot& s) {
    int copying = s.ui.copying == UiState::Copying::Tracks ? s.ui.copy_from : -1;
    constexpr float gap = 4, top = zone::picture_y + 2;
    constexpr float w = (zone::width - 2 * zone::pad - 3 * gap) / 4, ch = (zone::picture_h - 4 - gap) / 2;
    unsigned first = track_half(s) * 8;
    for (unsigned n = 0; n < 8; ++n) {
        unsigned t = first + n, row = 1 - n / 4, column = n % 4; // pad 1 bottom left
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap), inner = w - 12;
        const auto& track = s.project.tracks[t];
        auto number = juce::String(t + 1);
        if (!track.used()) {
            p.fill(x, y, w, ch, colour::quiet, 4);
            p.text(number, x + 6, y + 3, inner, 16, plain(colour::dim, 15, Weight::Bold));
            p.text("EMPTY", x + 6, y + 18, inner, 14, caps_text(colour::dim));
            continue;
        }
        bool selected = t == s.ui.track;
        auto c = track_colour(t);
        auto fg = selected ? colour::bg : c;
        p.fill(x, y, w, ch, selected ? c : colour::surf, 4);
        float nw = p.text(number, x + 6, y + 3, 24, 16, plain(fg, 15, Weight::Bold));
        juce::String state = track.mix.mute ? "MUTE" : (s.playing && s.has_notes[t] ? "P" + juce::String(track.pattern + 1) : "");
        if (state.isNotEmpty())
            p.text(state, x + 6 + nw + 4, y + 3, inner - nw - 4, 16, caps_text(selected ? colour::bg : colour::sec),
                   juce::Justification::centredRight);
        juce::String engine = engine_info(track.engine).name;
        p.text(engine, x + 6, y + 18, inner, 14, p.fit(engine, caps_text(fg), inner, type::floor));
        auto name = plain(selected ? colour::bg : colour::text, type::body, Weight::Medium);
        auto lines = p.wrap(track.name, name, inner, 2);
        for (int l = 0; l < lines.size(); ++l) p.text(lines[l], x + 6, y + 32 + l * 14, inner, 15, name);
        float level = track.mix.level / 127.0f;
        p.fill(x + 6, y + ch - 4, inner, 2, selected ? juce::Colours::black.withAlpha(0.2f) : colour::line, 1);
        p.fill(x + 6, y + ch - 4, inner * level, 2, fg, 1);
        if (track.mix.mute) p.fill(x, y, w, ch, colour::bg.withAlpha(0.5f), 4);
        if (copying == int(t)) p.ring(x + 0.5f, y + 0.5f, w - 1, ch - 1, colour::text, 4, 1.5f); // the track being copied
    }
}

unsigned track_half(const Snapshot& s) { return (s.ui.track % track_count) / 8; }

HeaderItem track_half_pill(const Snapshot& s) {
    return pill(track_half(s) ? "TRACKS 9-16" : "TRACKS 1-8");
}

// Pad Bank held (MELODEE_PARITY.md decisions): the tracks over the page, the
// kit's bank beside them; the knob strip stays the page's.
void draw_track_overlay(Paint& p, const Snapshot& s) {
    p.fill(0, zone::header_y, zone::width, zone::header_h + zone::picture_h, colour::bg);
    p.fill(0, zone::soft_y, zone::width, zone::soft_h, colour::bg);
    Header h = header_of(s);
    h.left.push_back(chip("PAD BANK", colour_of(s)));
    h.left.push_back(track_half_pill(s));
    const auto& t = s.track();
    if (t.engine == EngineKind::Sampler) h.left.push_back(pill(juce::String("BANK ") + char('A' + t.bank % sampler_banks)));
    header(p, h); // the selected track is the lit cell: no tag

    // B1-B3 do nothing while held: the row says what does.
    bool kit = t.engine == EngineKind::Sampler;
    auto hint = caps_text(colour::mid);
    if (kit) p.text(juce::String::fromUTF8("\xe2\x88\x92 / +  BANK"), zone::pad, zone::soft_y, 120, zone::soft_h, hint);
    p.text("SHIFT + ERASE  COPY", zone::width / 2.0f - 30, zone::soft_y, zone::width / 2.0f + 30 - zone::pad, zone::soft_h, hint,
           juce::Justification::centredRight);
    track_grid(p, s);
}

} // namespace melodee
