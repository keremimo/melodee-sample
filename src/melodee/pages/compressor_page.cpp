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
// kit's bank. The picture: the key grid with Sidechain, else the curve with
// a dot where the level sits now; beside it the last 2.6 s: what the
// detector heard rising from the foot (the key in its track's colour), the
// threshold dashed, and what was taken off hanging from the top, so a
// ducking mix reads as the pump it is.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;
constexpr float floor_db = 40;  // the level scales: -40 .. 0 dB
constexpr float duck_db = 24;   // what is taken off: 0 .. 24 dB down from the top

// The key's track is a kit, so a pad of it can key (and the pads can show its kit).
bool key_is_kit(const Project& project) { return project.tracks[project.compressor.key % track_count].engine == EngineKind::Sampler; }
bool kit_view(const Snapshot& s) { return s.ui.comp_pads && key_is_kit(s.project); }
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

// Sidechain's key on the pads as they sit (pad 1 bottom left): the tracks, the key's
// lit; or the key kit's pads of a bank, the key pad lit. Under it what Push turns to.
void key_grid(Paint& p, const Snapshot& s, float x, float y, float side) {
    const auto& c = s.project.compressor;
    unsigned key = c.key % track_count;
    bool pads = kit_view(s);
    unsigned bank = s.ui.comp_bank % sampler_banks;
    float caption = 16, gh = side - caption - 4, cw = (side - 6) / 4, ch = (gh - 6) / 4;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned row = 3 - pad / 4, column = pad % 4;
        float cx = x + column * (cw + 2), cy = y + row * (ch + 2);
        bool lit, used;
        juce::String text;
        juce::Colour fill = track_colour(pads ? key : pad);
        if (pads) {
            unsigned kit = bank * 16 + pad;
            lit = c.key_pad == kit;
            used = s.project.tracks[key].pads[kit].used();
            char label[4]; pad_label(kit, label);
            text = label;
        } else {
            lit = key == pad;
            used = s.project.tracks[pad].used();
            text = juce::String(pad + 1);
        }
        if (!c.on) fill = fill.withAlpha(0.45f);
        p.fill(cx, cy, cw, ch, lit ? fill : used ? colour::surf : colour::quiet, 3);
        if (!lit && used) p.fill(cx, cy + ch - 3, cw, 3, fill.withAlpha(0.6f), 1); // a used one: its colour underneath
        p.text(text, cx, cy, cw, ch, plain(lit ? colour::bg : used ? colour::sec : colour::dim, type::small, Weight::SemiBold),
               juce::Justification::centred);
    }
    if (key_is_kit(s.project)) // the cells say tracks or which bank; under them what Push turns to
        p.text(pads ? "PUSH: TRACKS" : "PUSH: KIT PADS", x, y + side - caption, side, caption, caps_text(colour::mid));
}

void draw(Paint& p, const Snapshot& s) {
    const auto& c = s.project.compressor;
    auto accent = colour::text;
    unsigned key = c.key % track_count;
    Header h = header_of(s);
    // Left: the page, and BYPASSED only while it is. Right: what it works on: the whole
    // master, or with Sidechain the track or kit pad the rest of the mix ducks under
    // (not the selected track; "key" is the musical key elsewhere in Melodee).
    h.left.push_back(pill("COMPRESSOR"));
    if (!c.on) h.left.push_back(pill("BYPASSED", colour::mid));
    // Bypassed, nothing ducks: B1 lit and the grid still show the source.
    if (c.sidechain && c.on) { h.right.push_back(label("DUCKED BY", colour::mid)); h.right.push_back(key_tag(s)); }
    else if (!c.sidechain) h.right.push_back(label("MASTER", colour::mid));
    SoftKeys keys {{"SIDECHAIN", "COLOR", "BYPASS"}, -1, accent}; // three toggles, each lit while it is on
    keys.lit = uint8_t(c.sidechain | c.color << 1 | !c.on << 2);
    frame(p, s, keys, h);

    auto mix = c.on ? accent : colour::mid;                                      // the mix and what it keeps
    auto heard = c.sidechain ? track_colour(key) : colour::sec;                  // what the detector hears
    if (!c.on) heard = colour::mid;
    float amount = c.amount / 100.0f, threshold = -amount * 30, ratio = 1 + amount * amount * 9;
    float makeup = -threshold * (1 - 1 / ratio) * 0.5f;
    auto over = [&](float in_db) { // dB taken off a signal at in_db (In Boost in front)
        float boosted = in_db + c.boost;
        return boosted > threshold ? (boosted - threshold) * (1 - 1 / ratio) : 0.0f;
    };

    // Left: Sidechain's key on the pads (whichever track is selected), else the curve,
    // input across, output up (-40..0 dB), with a dot where the level sits now.
    float x = zone::pad + 4, y = zone::picture_y + 6, side = zone::picture_h - 16;
    if (c.sidechain) {
        key_grid(p, s, x, y, side);
    } else {
        p.fill(x, y, side, side, colour::panel, 4);
        auto at = [&](float in_db) { return std::clamp(in_db + c.boost - over(in_db) + makeup, -floor_db, 0.0f); };
        auto curve_y = [&](float in_db) { return y + side - (at(in_db) + floor_db) / floor_db * side; };
        p.fill(x, y + side - 1, side, 1, colour::line);
        float tx = x + (threshold - c.boost + floor_db) / floor_db * side;
        if (tx > x && tx < x + side) p.fill(tx, y + 4, 1, side - 8, colour::line);
        juce::Path curve; // one stroke, every 2 dB
        for (int db = -40; db <= 0; db += 2) {
            float px = x + (db + floor_db) / floor_db * side;
            if (db == -40) curve.startNewSubPath(px, curve_y(float(db))); else curve.lineTo(px, curve_y(float(db)));
        }
        p.g.setColour(mix);
        p.g.strokePath(curve, juce::PathStrokeType(1.5f));
        float now_db = s.comp_trail.back().level - c.boost; // the curve's input has In Boost in front
        if (c.on && now_db > -floor_db) {
            float dx = x + std::min(now_db + floor_db, floor_db) / floor_db * side, dy = curve_y(std::min(now_db, 0.0f));
            p.g.setColour(colour::panel); p.g.fillEllipse(dx - 5, dy - 5, 10, 10);
            p.g.setColour(heard); p.g.fillEllipse(dx - 3.5f, dy - 3.5f, 7, 7);
        }
    }

    // The last 2.6 s, newest on the right.
    float gx = x + side + 12, gw = zone::width - zone::pad - gx, gh = side - 32;
    p.fill(gx, y, gw, gh, colour::panel, 4);
    float cw = gw / float(comp_columns);
    auto level_y = [&](float db) { return y + gh - std::clamp((db + floor_db) / floor_db, 0.0f, 1.0f) * (gh - 2); };
    auto duck_y = [&](float db) { return y + std::clamp(db / duck_db, 0.0f, 1.0f) * (gh - 2); };
    juce::Path level, taken;
    level.startNewSubPath(gx, y + gh);
    taken.startNewSubPath(gx, y);
    for (unsigned n = 0; n < comp_columns; ++n) {
        const auto& col = s.comp_trail[n];
        float cx = gx + (n + 0.5f) * cw;
        level.lineTo(cx, level_y(col.level));
        taken.lineTo(cx, duck_y(col.reduce));
    }
    level.lineTo(gx + gw, y + gh); level.closeSubPath();
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(gx), int(y), int(gw), int(gh)));
    p.g.setColour(heard.withAlpha(c.sidechain ? 0.55f : 0.3f)); // the key stands out; the mix itself sits back
    p.g.fillPath(level);
    juce::Path hanging = taken;
    hanging.lineTo(gx + gw, y); hanging.closeSubPath();
    p.g.setColour(mix.withAlpha(0.14f));
    p.g.fillPath(hanging);
    p.g.setColour(mix);
    p.g.strokePath(taken, juce::PathStrokeType(1.5f));
    p.g.restoreState();
    float ty = level_y(threshold);
    for (float dx = gx + 2; dx < gx + gw - 2; dx += 6) p.fill(dx, ty, 3, 1, colour::line); // the threshold

    // What it takes off: the most of the last third of a second, half a dB a step.
    float most = 0;
    for (unsigned n = comp_columns - 10; n < comp_columns; ++n) most = std::max(most, s.comp_trail[n].reduce);
    most = c.on ? std::round(std::min(most, 40.0f) * 2) / 2 : 0;
    float ry = y + gh + 6, rh = side - gh - 6;
    p.text(c.sidechain ? "DUCKING" : "REDUCTION", gx, ry, gw, rh, caps_text(colour::mid), juce::Justification::centredLeft);
    auto unit_style = plain(colour::sec, type::small);
    float uw = p.width("dB", unit_style);
    p.text("dB", gx + gw - uw, ry + 3, uw, rh, unit_style, juce::Justification::centredRight);
    p.text(most > 0 ? "-" + juce::String(most, 1) : "0.0", gx, ry, gw - uw - 4, rh,
           plain(c.on ? colour::text : colour::dim, type::title, Weight::SemiBold), juce::Justification::centredRight);
}

void soft(Context& c, int key) {
    const auto& comp = c.project.compressor;
    if (key == 0) {
        c.set_compressor(6, !comp.sidechain);
        c.notice(comp.sidechain ? "Sidechain on: pick a track on the pads" : "Sidechain off");
    }
    if (key == 1) { c.set_compressor(1, !comp.color); c.notice(comp.color ? "Color on" : "Color off"); }
    if (key == 2) { c.set_compressor(0, !comp.on); c.notice(comp.on ? "Compressor on" : "Compressor bypassed"); }
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
        char label[4]; pad_label(kit, label);
        c.notice(whole ? "Track " + std::to_string(key + 1) + " ducks the mix, the whole kit"
                       : std::string(label) + " " + kit_pad_name(c.project, key, kit).toStdString() + " ducks the mix");
        return;
    }
    if (!down) return;
    if (pad % 16 == key && key_is_kit(c.project)) { // the key kit again: its pads
        c.ui.comp_pads = true;
        c.ui.comp_bank = uint8_t(comp.key_pad != MasterComp::whole_kit ? comp.key_pad / 16 : c.project.tracks[key].bank % sampler_banks);
        return;
    }
    key_track(c, pad % 16);
    c.notice("Track " + std::to_string(pad % 16 + 1) + " ducks the mix");
}

// Push: on a kit key, its pads or the tracks.
void push(Context& c) {
    const auto& comp = c.project.compressor;
    if (!comp.sidechain) return;
    if (!key_is_kit(c.project)) { c.notice("Track " + std::to_string(comp.key + 1) + " is not a sample kit"); return; }
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
