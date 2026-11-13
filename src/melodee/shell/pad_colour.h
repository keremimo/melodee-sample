// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/sampler.h"
#include "melodee/ui/shades.h"

// Stage's COLOUR (B2 on a kit): K1 and the wheel walk the last pad's colour
// (model/sampler.h PadSound::colour) through a list: 0 the track's, 1-16 the
// sixteen track colours (ui/shades.h), and past them the pad's own when it
// is none of those (an MPC kit's), so a turn back finds it again until
// another is picked. Pure, for the bindings, the page and the tests.
namespace melodee {

constexpr int colour_track = 0;                 // the track's colour (PadSound::colour 0)
constexpr int colour_own = 1 + int(ui::track_shades.size()); // the pad's own, from its kit

// Where a pad's colour sits on the list.
inline int colour_choice(uint32_t colour) {
    if (!colour) return colour_track;
    for (unsigned n = 0; n < ui::track_shades.size(); ++n)
        if (ui::track_shades[n][0] == colour) return int(n) + 1;
    return colour_own;
}
// The last place on the list for a pad: its own colour when it has one off the sixteen.
inline int colour_last(uint32_t colour) { return colour_choice(colour) == colour_own ? colour_own : colour_own - 1; }
// The colour at `choice`; `own` is the pad's colour now, kept at colour_own.
inline uint32_t colour_at(int choice, uint32_t own) {
    if (choice <= colour_track) return 0;
    if (choice < colour_own) return ui::track_shades[size_t(choice - 1)][0];
    return own;
}

} // namespace melodee
