// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// TRACK COLOUR (issue 69, shell/track_colours.h), over the soft keys, the header and the picture: the sixteen colours
// as the pads sit (pads 13-16 along the top), the track's ringed, and its name; B1 BACK, B2 DEFAULT, B3 DONE.
// SEQUENCE COLOUR the same for the sequence in hand, B2 NONE.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/track_colours.h"

namespace melodee {

void draw_colour_sheet(ui::Paint& p, const Snapshot& s) {
    using namespace ui;
    const bool seq = s.ui.colour_seq; // a sequence's colour: NONE in place of DEFAULT
    const unsigned t = s.ui.colour_track % track_count, scene = s.ui.colour_scene % pattern_count;
    const int slot = colour_sheet_slot(s.project, s.ui); // -1: a sequence with none
    const auto c = slot >= 0 ? slot_colour(unsigned(slot)) : colour::text;
    p.fill(0, zone::soft_y, zone::width, zone::knobs_y - zone::soft_y, colour::bg);
    soft_keys(p, SoftKeys {{"BACK", seq ? "NONE" : "DEFAULT", "DONE"}, 2, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}});
    Header h = header_of(s);
    h.left.push_back(pill(seq ? "SEQUENCE COLOUR" : "TRACK COLOUR", colour::text));
    if (seq) {
        const auto& sc = s.project.scenes[scene];
        juce::String id = "SEQ " + juce::String(scene + 1);
        h.right.push_back(chip(sc.name[0] ? id + " " + juce::String(sc.name) : id, colour::amber));
    } else {
        h.right.push_back(tag_of(s, t));
    }
    header(p, h);

    constexpr float cell = 31, gap = 3, grid = 4 * cell + 3 * gap;
    const float gx = zone::pad, gy = zone::picture_y + (zone::picture_h - grid) / 2;
    for (unsigned k = 0; k < 16; ++k) {
        const unsigned row = 3 - k / 4, column = k % 4;
        const float x = gx + column * (cell + gap), y = gy + row * (cell + gap);
        p.fill(x, y, cell, cell, slot_colour(k), 5);
        if (int(k) == slot) p.ring(x - 2.5f, y - 2.5f, cell + 5, cell + 5, colour::text, 7, 2);
    }
    // Its name, and whether a track's follows its place (DEFAULT) or was picked; a sequence's, or none.
    const float tx = gx + grid + 18, tw = zone::width - tx - zone::pad;
    const juce::String name = slot >= 0 ? juce::String(ui::track_colour_names[unsigned(slot) % 16]) : juce::String("No colour");
    p.text(name, tx, gy + grid / 2 - 26, tw, 30, plain(slot >= 0 ? c : colour::dim, type::value, Weight::SemiBold));
    if (!seq) p.text(s.project.tracks[t].colour ? "PICKED" : "DEFAULT", tx, gy + grid / 2 + 6, tw, 14, caps_text(colour::mid));
}

} // namespace melodee
