// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The sixteen tracks as the pads sit: Stage's picture, and the screen while
// Pad Bank is held (its pads pick a track) or a track copy picks its tracks.
#include "melodee/pages/common.h"
#include "melodee/shell/pad_bank.h"

namespace melodee {
using namespace ui;

void track_grid(Paint& p, const Snapshot& s) {
    int copying = s.ui.copying == UiState::Copying::Tracks ? s.ui.copy_from : -1;
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
        if (copying == int(t)) p.ring(x + 0.5f, y + 0.5f, w - 1, ch - 1, colour::text, 4, 1.5f); // the track being copied
    }
}

// Pad Bank held (MELODEE_PARITY.md decisions): the tracks over the page, the
// kit's bank beside them; the knob strip stays the page's.
void draw_track_overlay(Paint& p, const Snapshot& s) {
    p.fill(0, zone::header_y, zone::width, zone::header_h + zone::picture_h, colour::bg);
    p.fill(0, zone::soft_y, zone::width, zone::soft_h, colour::bg);
    Header h = header_of(s);
    h.left.push_back(chip("PAD BANK", colour_of(s)));
    const auto& t = s.track();
    if (t.engine == EngineKind::Sampler) h.left.push_back(pill(juce::String("BANK ") + char('A' + t.bank % sampler_banks)));
    h.right.push_back(tag_of(s, s.ui.track));
    header(p, h);
    // B1-B3 do nothing while held; the keys say what does.
    bool kit = t.engine == EngineKind::Sampler;
    soft_keys(p, {{"PAD: TRACK", kit ? juce::String::fromUTF8("\xe2\x88\x92 / +: BANK") : juce::String(), "SHIFT+ERASE: COPY"}});
    track_grid(p, s);
}

} // namespace melodee
