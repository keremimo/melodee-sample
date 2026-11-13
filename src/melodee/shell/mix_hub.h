// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"
#include "surface/controls.h"

// The MIX hub (docs/HUBS_PLAN.md): Pad FX and Knob FX pressed together, in
// either order, open it on Tracks from any page. Tracks, Sends and the effect
// chains share one row of keys, B1 TRACKS, B2 SENDS, B3 FX. FX is a cycle along the signal: the track's
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

// SENDS shows the kit's pads instead of the tracks: B2 again on a kit's Sends; any other track the tracks.
inline bool mix_sends_pads(const UiState& ui, bool kit) { return kit && ui.sends_pads && ui.page == PageId::Sends; }
// B2 on Sends of a kit steps Tracks -> Pads -> Tracks; opened from elsewhere it starts on the tracks.
inline void mix_sends_key(UiState& ui, bool kit) {
    ui.sends_pads = kit && ui.page == PageId::Sends && !ui.sends_pads;
}

// Pad FX and Knob FX: alone, each does its own job when let go (Pad FX the punches, Knob FX its mode on or off);
// both down together, in either order, the MIX hub, and neither's own job. Shift + either (Flex Beat, FX Select,
// as printed) acts as it goes down, as before.
enum class FxChord : uint8_t { Own, Mix, Wait };
inline FxChord fx_chord(UiState& ui, SurfaceButton b, bool pressed) {
    bool pad = b == SurfaceButton::PadFx;
    bool& mine = pad ? ui.pad_fx_down : ui.knob_fx_down;
    bool other = pad ? ui.knob_fx_down : ui.pad_fx_down;
    if (pressed) {
        if (ui.shift && !other) return FxChord::Own; // printed Shift jobs: at once
        mine = true;
        if (other) { ui.fx_chord_used = true; return FxChord::Mix; }
        return FxChord::Wait;
    }
    if (!mine) return FxChord::Wait; // went down with Shift: done then
    mine = false;
    bool used = ui.fx_chord_used;
    if (!ui.pad_fx_down && !ui.knob_fx_down) ui.fx_chord_used = false;
    return used ? FxChord::Wait : FxChord::Own;
}

} // namespace melodee
