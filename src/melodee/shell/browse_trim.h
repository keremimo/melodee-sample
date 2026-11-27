// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/zoom_view.h"
#include <array>

// Samples' trim sheet: the audio file under the cursor trimmed before it goes onto a pad. K2 is its START, K3 its END
// (UiState::BrowseTrim). The first turn reads the file into a sample slot, as Push would, and raises the sheet over the
// list: the whole file, and a closer view round the handle turned last. Unzoomed, each pot turns across the whole
// file and the view (about 120 ms) follows its handle; Shift + K2 / K3 set a zoom as on Trim, and then the pot turns
// across the view on screen. The wheel moves the handle turned last, finer, as on Trim. Push or Shift + pad load the
// file with START and END on the pad; the file on the card is not changed. Pure, so the test runs it as it is.
namespace melodee {

struct BrowseHandles {
    uint32_t frames = 0, start = 0, end = 0;
    uint32_t at(unsigned focus) const { return focus ? end : start; }
};
inline BrowseHandles browse_handles(const Project& project, const BrowseTrim& b) {
    BrowseHandles h;
    if (!b.up()) return h;
    h.frames = project.samples[unsigned(b.slot) % sample_slots].frames;
    h.end = b.end && b.end < h.frames ? b.end : h.frames;
    h.start = std::min(b.start, h.end ? h.end - 1 : 0);
    return h;
}
// The zoom in use: Shift + K's, else the one that shows about follow_view round the handle (0 where all of it fits).
inline int browse_zoom(const BrowseTrim& b, uint32_t frames) { return b.own_zoom ? b.zoom : zoom_for_width(frames, follow_view); }
inline uint32_t browse_key(const BrowseTrim& b) { return view_key(uint32_t(b.slot), b.focus, b.zoom, b.own_zoom, 0x7b); }
inline FrameView browse_view(const Project& project, const BrowseTrim& b) {
    const auto h = browse_handles(project, b);
    if (!h.frames) return {};
    const int zoom = browse_zoom(b, h.frames);
    if (!b.own_zoom) return zoom_view(h.frames, 0, h.frames, h.at(b.focus), zoom); // round the handle as it moves
    return zoom_now(b.view, browse_key(b), h.frames, 0, h.frames, h.at(b.focus), zoom);
}
// After a handle moved: an own zoom's view goes along so the handle stays inside it.
inline void browse_moved(BrowseTrim& b, const Project& project, FrameView was) {
    if (!b.own_zoom) { b.view = {}; return; }
    const auto h = browse_handles(project, b);
    zoom_follow(b.view, browse_key(b), was, h.at(b.focus), h.frames, b.zoom);
}

// K2 START and K3 END (K1 is JUMP). Not read yet: cards with no range the pots can drive, so a turn raises the sheet
// and moves nothing. With an own zoom the pot covers the view on screen, else the whole file.
inline std::array<Binding, 2> browse_cards(const Project& project, const BrowseTrim& b) {
    using K = Binding::Kind;
    const auto h = browse_handles(project, b);
    if (!h.frames) return {Binding {K::BrowseStart, 1, 0, 1, 0}, Binding {K::BrowseEnd, 1, 0, 1, 1}};
    const FrameView view = b.own_zoom && b.zoom ? browse_view(project, b) : FrameView {};
    const uint32_t last = h.end > h.start ? h.end - 1 : h.start;
    return {handle_card(K::BrowseStart, 0, h.start, 0, last, view), handle_card(K::BrowseEnd, 0, h.end, h.start + 1, h.frames, view)};
}
inline std::array<Binding, 2> browse_zoom_cards(const Project& project, const BrowseTrim& b) {
    using K = Binding::Kind;
    const auto h = browse_handles(project, b);
    const int zoom = h.frames ? browse_zoom(b, h.frames) : 0;
    return {Binding {K::BrowseZoom, 0, 0, zoom_top, zoom}, Binding {K::BrowseZoom, 1, 0, zoom_top, zoom}};
}

// A value from a card or the wheel, as frames: START kept before END, END after START (the file's end kept as 0, so
// the pad follows a longer reload as Trim's does). True when it moved.
inline bool browse_set(BrowseTrim& b, const Project& project, Binding::Kind kind, int value) {
    const auto h = browse_handles(project, b);
    if (!h.frames) return false;
    const FrameView was = browse_view(project, b);
    if (kind == Binding::Kind::BrowseStart) {
        const uint32_t at = uint32_t(std::clamp<int64_t>(value, 0, int64_t(h.end) - 1));
        if (at == h.start) return false;
        b.start = at;
    } else {
        const uint32_t at = uint32_t(std::clamp<int64_t>(value, int64_t(h.start) + 1, h.frames));
        if (at == h.end) return false;
        b.end = at >= h.frames ? 0 : at;
    }
    browse_moved(b, project, was);
    return true;
}

// What a moved handle plays once it is still: from START to END; an END, its last second (as Chop's end).
inline std::pair<uint32_t, uint32_t> browse_heard(const BrowseHandles& h, unsigned focus) {
    if (!focus) return {h.start, h.end};
    return {std::max(h.start, h.end > heard_tail ? h.end - heard_tail : 0), h.end};
}

} // namespace melodee
