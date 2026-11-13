// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// The MIX hub (docs/HUBS_PLAN.md, M-b): Tracks, Sends and the effect chains
// share one row of keys, B1 TRACKS, B2 SENDS, B3 FX, on each of those pages
// and while Pad Bank is held. FX is a cycle along the signal: the track's
// chain, a bus's, the master's, and on a kit the last pad's; the lit FX key
// steps on, as Stage's keys do. A loop track's chain (the Looper's) keeps its
// own keys. Pure, for the pages, the shell and the tests.
namespace melodee {

enum class MixSection : uint8_t { Tracks, Sends, Fx, None };

// The FX cycle on a track (a kit has its pads' chain too), and how many.
inline unsigned mix_fx_pages(bool kit, std::array<PageId, 4>& out) {
    out = {PageId::Rack, PageId::Bus, PageId::Master, PageId::PadFx};
    return kit ? 4 : 3;
}
// Which section a page is (None: not the MIX hub's), and its place in the FX cycle.
inline MixSection mix_section(PageId page) {
    switch (page) {
    case PageId::Tracks: return MixSection::Tracks;
    case PageId::Sends: return MixSection::Sends;
    case PageId::Rack: case PageId::Bus: case PageId::Master: case PageId::PadFx: return MixSection::Fx;
    default: return MixSection::None;
    }
}
inline unsigned mix_fx_at(PageId page, bool kit) {
    std::array<PageId, 4> pages {};
    unsigned count = mix_fx_pages(kit, pages);
    for (unsigned n = 0; n < count; ++n) if (pages[n] == page) return n;
    return 0;
}
// Where B1-B3 (`key` 0-2) go from `page`: TRACKS, SENDS, or FX (its first page; on it, the next).
inline PageId mix_next(PageId page, unsigned key, bool kit) {
    if (key % 3 == 0) return PageId::Tracks;
    if (key % 3 == 1) return PageId::Sends;
    std::array<PageId, 4> pages {};
    unsigned count = mix_fx_pages(kit, pages);
    if (mix_section(page) != MixSection::Fx) return pages[0];
    return pages[(mix_fx_at(page, kit) + 1) % count];
}

} // namespace melodee
