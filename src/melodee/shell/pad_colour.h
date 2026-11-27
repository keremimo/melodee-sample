// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/sampler.h"
#include "melodee/ui/shades.h"

// Stage's COLOUR (B2 on a kit): K1 and the wheel walk the last pad's colour
// (model/sampler.h PadSound::colour) through a list: 0 the track's, 1-16 the
// sixteen track colours (ui/shades.h), and past them Kit, the colour the
// pad's MPC kit brought (PadSound::kit_colour), so a turn back finds it
// again after another was picked. Pure, for the bindings, the page and the
// tests.
namespace melodee {

constexpr int colour_track = 0;                 // the track's colour (PadSound::colour 0)
constexpr int colour_own = 1 + int(ui::track_shades.size()); // the pad's own, from its kit

// Where a colour sits on the list.
inline int colour_choice(uint32_t colour) {
    if (!colour) return colour_track;
    for (unsigned n = 0; n < ui::track_shades.size(); ++n)
        if (ui::track_shades[n][0] == colour) return int(n) + 1;
    return colour_own;
}
// The colour a pad's kit brought; 0 none. A pad from before PKCL (or a build that wrote none) has its colour as
// it came while that is none of the sixteen.
inline uint32_t kit_colour(const PadSound& pad) {
    if (pad.kit_colour) return pad.kit_colour;
    return colour_choice(pad.colour) == colour_own ? pad.colour : 0;
}
// The last place on the list for a pad: Kit when its kit brought a colour.
inline int colour_last(const PadSound& pad) { return kit_colour(pad) ? colour_own : colour_own - 1; }
// The colour at `choice` for `pad`.
inline uint32_t colour_at(int choice, const PadSound& pad) {
    if (choice <= colour_track) return 0;
    if (choice < colour_own) return ui::track_shades[size_t(choice - 1)][0];
    return kit_colour(pad);
}
// Stage's COLOUR turned to `choice` on `pad`: the kit's colour kept for a turn back.
inline void pick_colour(PadSound& pad, int choice) {
    pad.kit_colour = kit_colour(pad);
    pad.colour = colour_at(choice, pad);
}

} // namespace melodee
