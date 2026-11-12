// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// COMPRESSOR (Shift + Pad 5, parity phase 7), as on the MPC: the master
// compressor after the master chain. B1 SIDECHAIN (one track keys it and
// the rest of the mix ducks under that track), B2 COLOR (a little low end,
// wobble and saturation), B3 bypasses or engages it. K1 attack (0.1-150
// ms), K2 release (3-300 ms), K3 amount (threshold and ratio together;
// makeup follows, none with Sidechain), Shift + K3 In Boost (into the
// detector alone with Sidechain). With Sidechain the pads pick the key
// (whichever track is selected): a track, or on a kit one of its pads (Push,
// or the kit's pad again); the wheel or Minus / Plus step the track, or the
// kit's bank; the header names the key. The picture (round 2, compressor B,
// "one bar of pump"): one bar of the song, beats numbered under it; what was
// taken off hangs from the top across it up to where the transport is, the
// bar before as a dashed ghost, and the detector's hits (the key's with
// Sidechain) marked along the foot. Release is tuned against the groove, as
// it is heard. At the bottom right what it takes off now.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;
constexpr float duck_db = 24;   // what is taken off: 0 .. 24 dB down from the top

// The key's track is a kit, so a pad of it can key (and the pads can show its kit).
bool key_is_kit(const Project& project) { return project.tracks[project.compressor.key % track_count].engine == EngineKind::Sampler; }
juce::String kit_pad_name(const Project& project, unsigned track, unsigned kit) {
    const auto& pad = project.tracks[track % track_count].pads[kit % sampler_pads];
    return pad.used() ? juce::String(project.samples[pad.sample % sample_slots].name) : juce::String("Empty");
}

// The header's key: the track, or on a kit the pad ("6 A01 Kick").
HeaderItem key_tag(const Snapshot& s) {
    const auto& c = s.project.compressor;
    unsigned key = c.key % track_count;
    if (c.key_pad == MasterComp::whole_kit || !key_is_kit(s.project)) return tag_of(s, key);
    char text[4]; pad_label(c.key_pad, text);
    return track_tag(int(key) + 1, text, kit_pad_name(s.project, key, c.key_pad), track_colour(key));
}

void draw(Paint& p, const Snapshot& s) {
    const auto& c = s.project.compressor;
    auto accent = colour::text;
    unsigned key = c.key % track_count;
    Header h = header_of(s);
    // Signs, not words. Left: an LED, lit while it works, a ring while bypassed. Right: the
    // master, or with Sidechain the pump sign and the track or kit pad the rest ducks under
    // (not the selected track); bypassed, nothing ducks: B1 lit and the grid show the source.
    h.left.push_back(sign(c.on ? Glyph::Led : Glyph::LedOff, c.on ? accent : colour::mid));
    h.left.push_back(pill("COMPRESSOR", c.on ? colour::text : colour::mid));
    if (c.sidechain && c.on) { h.right.push_back(sign(Glyph::Duck, track_colour(key))); h.right.push_back(key_tag(s)); }
    else if (!c.sidechain) h.right.push_back(label("MASTER", colour::mid));
    SoftKeys keys {{"SIDECHAIN", "COLOR", "BYPASS"}, -1, accent}; // three toggles, each lit while it is on
    keys.lit = uint8_t(c.sidechain | c.color << 1 | !c.on << 2);
    frame(p, s, keys, h);

    auto mix = c.on ? accent : colour::mid;                     // what it takes off
    auto heard = c.sidechain ? track_colour(key) : colour::sec; // what the detector hears
    if (!c.on) heard = colour::mid;

    // One bar: beats numbered under it, the one playing bright.
    float x = zone::pad, y = zone::picture_y + 4, w = zone::width - 2 * zone::pad, h_ = zone::picture_h - 26;
    p.fill(x, y, w, h_, colour::quiet, 3);
    unsigned beats = std::max(1u, s.project.bar_ticks() / std::max(1u, s.project.beat_ticks()));
    int at = s.comp_bar_at, beat_now = at >= 0 ? at * int(beats) / int(comp_bar_columns) : -1;
    for (unsigned b = 0; b < beats; ++b) {
        float bx = x + w * float(b) / float(beats);
        if (b) p.fill(bx - 0.75f, y, 1.5f, h_, colour::bg);
        p.text(juce::String(b + 1), bx + 3, y + h_ + 3, 24, 16, plain(int(b) == beat_now ? colour::text : colour::dim, type::small, Weight::SemiBold));
    }
    float cw = w / float(comp_bar_columns);
    auto duck_y = [&](float db) { return y + std::clamp(db / duck_db, 0.0f, 1.0f) * (h_ - 2); };
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), int(w), int(h_)));
    // The bar before, dashed, across the whole width.
    juce::Path ghost;
    for (unsigned n = 0; n < comp_bar_columns; ++n) {
        float gx = x + (n + 0.5f) * cw, gy = duck_y(s.comp_ghost[n].reduce);
        if (n) ghost.lineTo(gx, gy); else ghost.startNewSubPath(gx, gy);
    }
    juce::Path dashed;
    const float dashes[] {2, 3};
    juce::PathStrokeType(1.2f).createDashedStroke(dashed, ghost, dashes, 2);
    p.g.setColour(colour::dim);
    p.g.fillPath(dashed);
    // This bar up to the transport, hanging from the top.
    if (at >= 0) {
        juce::Path taken;
        taken.startNewSubPath(x, y);
        for (int n = 0; n <= at; ++n) taken.lineTo(x + (n + 0.5f) * cw, duck_y(s.comp_bar[size_t(n)].reduce));
        juce::Path hanging = taken;
        hanging.lineTo(x + (at + 0.5f) * cw, y); hanging.closeSubPath();
        p.g.setColour(mix.withAlpha(0.25f));
        p.g.fillPath(hanging);
        p.g.setColour(mix);
        p.g.strokePath(taken, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        p.fill(x + (at + 0.5f) * cw - 0.5f, y, 1, h_, colour::text.withAlpha(0.7f)); // where the transport is
    }
    // The hits along the foot: this bar's so far, the bar before's after the transport.
    for (unsigned n = 0; n < comp_bar_columns; ++n) {
        bool now = int(n) <= at && s.comp_bar[n].hit, before = int(n) > at && s.comp_ghost[n].hit;
        if (now || before) p.fill(std::clamp(x + n * cw - 1.5f, x, x + w - 5), y + h_ - 10, 5, 8, now ? heard : heard.withAlpha(0.4f), 1.5f);
    }
    p.g.restoreState();

    // What it takes off: the most of the last third of a second, half a dB a step.
    float most = 0;
    for (unsigned n = comp_columns - 10; n < comp_columns; ++n) most = std::max(most, s.comp_trail[n].reduce);
    most = c.on ? std::round(std::min(most, 40.0f) * 2) / 2 : 0;
    auto unit_style = plain(colour::mid, type::body);
    float uw = p.width("dB", unit_style), vy = y + h_ - 30;
    p.text("dB", x + w - 8 - uw, vy + 4, uw, 24, unit_style, juce::Justification::centredRight);
    p.text(most > 0 ? juce::String::fromUTF8("\xe2\x88\x92") + juce::String(most, 1) : juce::String("0.0"), x, vy, w - 14 - uw, 26,
           plain(c.on ? colour::text : colour::dim, type::title, Weight::SemiBold), juce::Justification::centredRight);
}

void soft(Context& c, int key) {
    const auto& comp = c.project.compressor;
    // No messages: each key's bar, the LED and the grid show what changed.
    if (key == 0) c.set_compressor(6, !comp.sidechain);
    if (key == 1) c.set_compressor(1, !comp.color);
    if (key == 2) c.set_compressor(0, !comp.on);
}

void key_track(Context& c, unsigned track) {
    if (track != c.project.compressor.key) c.ui.comp_pads = false;
    c.set_compressor(7, int(track));
    c.set_compressor(8, -1);
}

// Sidechain on: the pads pick the key, whichever track is selected. A track; on a kit
// (Push, or its pad again) one of its pads, and that pad again the whole kit. A kit pad
// sounds as it is picked. Sidechain off: they play the selected track.
void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    const auto& comp = c.project.compressor;
    if (!comp.sidechain) { play_pad(c, pad, velocity, down); return; }
    unsigned key = comp.key % track_count;
    if (c.ui.comp_pads && key_is_kit(c.project)) {
        unsigned kit = c.ui.comp_bank % sampler_banks * 16 + pad % 16;
        if (!down) return;
        c.send({Command::Type::PadNote, uint8_t(key), 64, uint16_t(kit), int16_t(velocity)}); // heard as it is picked
        bool whole = comp.key_pad == kit;
        c.set_compressor(8, whole ? -1 : int(kit));
        return;
    }
    if (!down) return;
    if (pad % 16 == key && key_is_kit(c.project)) { // the key kit again: its pads
        c.ui.comp_pads = true;
        c.ui.comp_bank = uint8_t(comp.key_pad != MasterComp::whole_kit ? comp.key_pad / 16 : c.project.tracks[key].bank % sampler_banks);
        return;
    }
    key_track(c, pad % 16);
}

// Push: on a kit key, its pads or the tracks.
void push(Context& c) {
    const auto& comp = c.project.compressor;
    if (!comp.sidechain) return;
    if (!key_is_kit(c.project)) return; // no kit pads to show (the caption is not there)
    c.ui.comp_pads = !c.ui.comp_pads;
    if (c.ui.comp_pads)
        c.ui.comp_bank = uint8_t(comp.key_pad != MasterComp::whole_kit ? comp.key_pad / 16 : c.project.tracks[comp.key % track_count].bank % sampler_banks);
}

// The wheel or Minus / Plus: the key track, or with a kit's pads up its banks.
void step(Context& c, int way) {
    const auto& comp = c.project.compressor;
    if (!comp.sidechain || !way) return;
    if (c.ui.comp_pads && key_is_kit(c.project)) { c.ui.comp_bank = uint8_t(std::clamp<int>(c.ui.comp_bank + way, 0, int(sampler_banks) - 1)); return; }
    key_track(c, unsigned(std::clamp<int>(comp.key + way, 0, int(track_count) - 1)));
}
} // namespace

const PageSpec compressor_page {draw, pad, step, push, soft, step};

} // namespace melodee
