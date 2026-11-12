// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The sixteen tracks as the pads sit, and the track in hand as a strip
// beside them: the Tracks page (Sample's second stop), and the screen while
// Pad Bank is held (its pads pick a track) or a track copy picks its tracks.
#include "melodee/pages/common.h"
#include "melodee/shell/pad_bank.h"
#include <cmath>

namespace melodee {
using namespace ui;

namespace {
constexpr float gap = 4, top = zone::picture_y + 2, cell_h = (zone::picture_h - 4 - 3 * gap) / 4;
constexpr float strip_w = 66, strip_x = zone::width - zone::pad - strip_w;
constexpr float cell_w = (strip_x - 6 - zone::pad - 3 * gap) / 4;

// A level bar, bottom up, on its groove.
void column(Paint& p, float x, float y, float w, float h, float fill, juce::Colour c) {
    p.fill(x, y, w, h, colour::line, w / 2);
    fill = std::clamp(fill, 0.0f, 1.0f);
    if (fill > 0) p.fill(x, y + h * (1 - fill), w, h * fill, c, w / 2);
}

float meter_of(const Snapshot& s, unsigned t) {
    float m = s.meters[t];
    return m > 0 ? std::clamp((20 * std::log10(m) + 48) / 48, 0.0f, 1.0f) : 0;
}

// One cell: number, the pattern it plays, the engine as a glyph, the level up its right edge; muted, a crossed speaker.
void cell(Paint& p, const Snapshot& s, unsigned t, float x, float y, int copying) {
    const auto& track = s.project.tracks[t];
    auto number = juce::String(t + 1);
    if (!track.used()) {
        p.fill(x, y, cell_w, cell_h, colour::quiet, 3);
        p.text(number, x + 6, y + 2, 24, 16, plain(colour::dim, type::body, Weight::Bold));
        return;
    }
    bool selected = t == s.ui.track, muted = track.mix.mute;
    auto c = track_colour(t);
    p.fill(x, y, cell_w, cell_h, selected ? colour::raised : colour::surf, 3);
    if (selected) p.fill(x, y, 2.5f, cell_h, c, 1);
    p.text(number, x + 6, y + 2, 20, 16, plain(muted ? colour::dim : c, type::body, Weight::Bold));
    p.text("P" + juce::String(track.pattern + 1), x + 6, y + 16, 24, 14, plain(colour::dim, type::small, Weight::Medium));
    icon(p, muted ? Icon::SpeakerOff : engine_icon(track.engine), x + 26, y + 5, 12, muted ? colour::rec : c, 1.4f);
    if (!muted) column(p, x + cell_w - 6, y + 4, 3, cell_h - 8, track.mix.level / 127.0f, c);
    // What it takes of a block (roadmap P2), along the bottom: the whole width a core's whole block, in the load's colour.
    float load = std::clamp(s.machine.track_load[t], 0.0f, 1.0f);
    if (load >= 0.005f) p.fill(x + 6, y + cell_h - 4, std::max(2.0f, (cell_w - 16) * load), 2, load_colour(load), 1);
    if (copying == int(t)) p.ring(x + 0.5f, y + 0.5f, cell_w - 1, cell_h - 1, colour::text, 3, 1.5f); // the track being copied
}

// The track in hand, wide: number and name, fader and meter, pan, its send and bus.
void strip(Paint& p, const Snapshot& s) {
    unsigned t = s.ui.track % track_count;
    const auto& track = s.project.tracks[t];
    float x = strip_x, y = top, h = zone::picture_h - 4;
    p.fill(x, y, strip_w, h, colour::panel, 4);
    auto c = track_colour(t);
    p.text(juce::String(t + 1), x + 6, y + 3, 20, 16, plain(c, type::body, Weight::Bold));
    if (!track.used()) return;
    auto name = plain(colour::text, type::small, Weight::SemiBold);
    float iw = strip_w - 12;
    auto lines = p.wrap(name_case(track.name), name, iw, 2);
    for (int l = 0; l < lines.size(); ++l) p.text(lines[l], x + 6, y + 19 + l * 12, iw, 14, p.fit(lines[l], name, iw, type::floor));
    float fy = y + 48, fh = h - 56;
    column(p, x + 8, fy, 7, fh, track.mix.level / 127.0f, c);
    column(p, x + 18, fy, 3, fh, track.mix.mute ? 0 : meter_of(s, t), c.withAlpha(0.6f));
    if (track.mix.mute) icon(p, Icon::SpeakerOff, x + 40, fy + fh - 14, 12, colour::rec, 1.4f);
    // Pan: an arc over the top, the needle where it points.
    float cx = x + 46, cy = fy + 22, r = 12;
    juce::Path arc;
    arc.addCentredArc(cx, cy, r, r, 0, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
    p.g.setColour(colour::line);
    p.g.strokePath(arc, juce::PathStrokeType(3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    float angle = track.mix.pan / 50.0f * juce::MathConstants<float>::halfPi;
    juce::Path needle;
    needle.startNewSubPath(cx, cy); needle.lineTo(cx + std::sin(angle) * r, cy - std::cos(angle) * r);
    auto pan_c = knob_shade(t, 1);
    p.g.setColour(pan_c);
    p.g.strokePath(needle, juce::PathStrokeType(2, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    p.circle(cx, cy, 2.6f, pan_c);
    // The four sends, A-D top to bottom, as short bars: how much goes into each bus (Stage's SENDS names them).
    for (unsigned b = 0; b < bus_count; ++b) {
        float sy = fy + 44 + b * 8, amount = track.mix.sends[b] / 100.0f;
        auto sc = b < 3 ? knob_shade(t, b) : colour::mid;
        p.fill(cx - 14, sy, 28, 3, colour::line, 1.5f);
        if (amount > 0) p.fill(cx - 14, sy, 28 * amount, 3, sc, 1.5f);
    }
}
} // namespace

void track_grid(Paint& p, const Snapshot& s) {
    int copying = s.ui.copying == UiState::Copying::Tracks ? s.ui.copy_from : -1;
    for (unsigned t = 0; t < track_count; ++t) {
        unsigned row = 3 - t / 4, col = t % 4; // pad 1 bottom left
        cell(p, s, t, zone::pad + col * (cell_w + gap), top + row * (cell_h + gap), copying);
    }
    strip(p, s);
}

// Pad Bank held (MELODEE_PARITY.md decisions): the tracks over the page, the
// kit's bank beside them; the knob strip stays the page's.
void draw_track_overlay(Paint& p, const Snapshot& s) {
    p.fill(0, zone::header_y, zone::width, zone::header_h + zone::picture_h, colour::bg);
    p.fill(0, zone::soft_y, zone::width, zone::soft_h, colour::bg);
    Header h = header_of(s);
    h.left.push_back(chip(Icon::Pads, "PAD BANK", colour_of(s)));
    const auto& t = s.track();
    bool kit = t.engine == EngineKind::Sampler;
    if (kit) h.left.push_back(pill(juce::String("BANK ") + char('A' + t.bank % sampler_banks)));
    header(p, h); // the selected track is the lit cell: no tag
    // B1-B3 do nothing while held: the row shows what does, in glyphs.
    auto hint = caps_text(colour::mid);
    float mid = zone::soft_y + zone::soft_h / 2.0f;
    if (kit) {
        icon(p, Icon::Minus, zone::pad + 4, mid - 5, 10, colour::mid, 1.6f);
        icon(p, Icon::Plus, zone::pad + 16, mid - 5, 10, colour::mid, 1.6f);
        p.text("BANK", zone::pad + 30, zone::soft_y, 60, zone::soft_h, hint);
    }
    float w = p.width("ERASE", hint), x = zone::width - zone::pad - 4 - 10 - 6 - w - 6 - 10;
    icon(p, Icon::Shift, x, mid - 5, 10, colour::mid, 1.5f);
    p.text("ERASE", x + 16, zone::soft_y, w + 1, zone::soft_h, hint);
    icon(p, Icon::Copy, x + 22 + w, mid - 5, 10, colour::mid, 1.5f);
    track_grid(p, s);
}

} // namespace melodee
