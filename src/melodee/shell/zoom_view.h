// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/bindings.h"
#include "melodee/model/slice_banks.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

// Trim's and Chop's view of the sample and their handles (start, end, loop
// point; a slice's start and end). The zoom (Shift + K1-K3) runs 0..100 as
// the MPC's "0 - 100%": the view's width goes from all of it (the file on
// Trim, the chopped region on Chop) down to zoom_closest frames, by the same
// factor every step, so a pot zooms smoothly. Zoomed, the view sits round the
// handle in focus and stays put while that handle moves inside it (the
// pot turns across exactly what is on screen); the wheel's edits carry it
// along at the edges. The handle in focus is the one whose pot was touched
// last, or whose zoom; on Chop the wheel edits it once a pot was touched.
namespace melodee {

constexpr int zoom_top = 100;
constexpr uint32_t zoom_closest = 256; // frames across the view at 100%: about one a column of the wave

inline uint32_t zoom_width(uint32_t span, int zoom) {
    if (zoom <= 0 || span <= zoom_closest) return span;
    double f = double(std::min(zoom, zoom_top)) / zoom_top;
    double w = double(span) * std::pow(double(zoom_closest) / double(span), f);
    return std::clamp(uint32_t(std::lround(w)), zoom_closest, span);
}
// The zoom that shows about `width` of `span`: 0 where all of it fits.
inline int zoom_for_width(uint32_t span, uint32_t width) {
    if (span <= width) return 0;
    if (width <= zoom_closest || span <= zoom_closest) return zoom_top;
    double f = std::log(double(width) / double(span)) / std::log(double(zoom_closest) / double(span));
    return std::clamp(int(std::lround(f * zoom_top)), 0, zoom_top);
}
// Chop: the wheel moving a cut closes the view in on it, about follow_view across, unless Shift + K set a zoom; the
// next pad, Minus / Plus or K1 / K2 open it again (chop_unfollow), so the pots cover the whole region.
constexpr uint32_t follow_view = 5292; // 120 ms, as the project counts frames (44.1 kHz)
constexpr uint32_t chop_heard_ms = 150; // a moved cut is heard once it has been still this long
constexpr uint32_t heard_lead = 13230, heard_tail = 44100; // with 300 ms before a start; an end, the slice's last second
inline void chop_follow(UiState& ui, uint32_t span) {
    if (ui.chop_zoom && !ui.chop_zoom_auto) return; // a zoom set by Shift + K stays as it is
    const int z = zoom_for_width(span, follow_view);
    if (!z || (ui.chop_zoom_auto && ui.chop_zoom == z)) return;
    ui.chop_zoom = uint8_t(z); ui.chop_zoom_auto = true; ui.chop_view = {};
}
inline void chop_unfollow(UiState& ui) {
    if (!ui.chop_zoom_auto) return;
    ui.chop_zoom = 0; ui.chop_zoom_auto = false; ui.chop_view = {};
}
// Chop's take (model/slice_banks.h): whether it still plays, and the slice whose bank the pads show: the next one's while
// it plays, and after a take while the newest slice is in hand (the red stays on the next pad); else the slice in hand's.
inline bool chop_take_playing(const Project& project, const UiState& ui, uint32_t now) {
    const auto& sl = ui.slices;
    if (!ui.chop_take_at || !sl.count || ui.chop_sample >= sample_slots) return false;
    return take_live(ui.chop_take_at, ui.chop_take_from, sl.end(sl.count - 1, project.samples[ui.chop_sample].frames), now);
}
inline bool chop_red_shown(const Project& project, const UiState& ui, uint32_t now) {
    return ui.slices.count < max_slices && (chop_take_playing(project, ui, now) || (ui.chop_more && ui.chop_mode == ChopMode::Manual));
}
inline unsigned chop_bank_slice(const Project& project, const UiState& ui, uint32_t now) {
    const unsigned count = ui.slices.count;
    const bool next = chop_take_playing(project, ui, now) || (ui.chop_more && ui.chop_mode == ChopMode::Manual && ui.chop_slice + 1u == count);
    return view_slice(ui.chop_slice, count, next, max_slices);
}
// How many times closer than unzoomed, for the screen.
inline double zoom_factor(uint32_t span, int zoom) { return double(span) / std::max(1u, zoom_width(span, zoom)); }

struct FrameView {
    uint32_t from = 0, to = 0;
    uint32_t width() const { return to > from ? to - from : 0; }
};

// The view of `span_from`..`span_to` at `zoom`, round `centre`, inside the file.
inline FrameView zoom_view(uint32_t frames, uint32_t span_from, uint32_t span_to, uint32_t centre, int zoom) {
    if (zoom <= 0 || !frames) return {span_from, span_to};
    uint32_t w = std::min(frames, zoom_width(span_to > span_from ? span_to - span_from : frames, zoom));
    uint32_t from = centre > w / 2 ? centre - w / 2 : 0;
    from = std::min(from, frames - w);
    return {from, from + w};
}

// The view now: the one kept while it is for the same handle and zoom (`key`), else round the handle.
inline FrameView zoom_now(const ZoomView& kept, uint32_t key, uint32_t frames, uint32_t span_from, uint32_t span_to, uint32_t centre,
                          int zoom) {
    if (zoom <= 0 || !frames) return {span_from, span_to};
    if (kept.key == key && kept.to > kept.from && kept.to <= frames) return {kept.from, kept.to};
    return zoom_view(frames, span_from, span_to, centre, zoom);
}
// After a handle moved to `at`: the view it moved in, kept, and carried along so `at` stays inside it.
inline void zoom_follow(ZoomView& kept, uint32_t key, FrameView was, uint32_t at, uint32_t frames, int zoom) {
    if (zoom <= 0 || !frames || !was.width()) { kept = {}; return; }
    uint32_t w = was.width(), from = was.from;
    if (at < from) from = at;
    else if (at > from + w) from = at - w;
    from = std::min(from, frames > w ? frames - w : 0);
    kept = {from, from + w, key};
}

// A handle's card in frames between lo and hi: zoomed in, the pot turns across the view on screen, which stays put
// while the handle is inside it.
inline Binding handle_card(Binding::Kind kind, int id, uint32_t value, uint32_t lo, uint32_t hi, FrameView view) {
    if (view.width() && value >= view.from && value <= view.to) { lo = std::max(lo, view.from); hi = std::min(hi, view.to); } // another handle's view: all of it
    if (hi <= lo) { lo = std::min(lo, value); hi = std::max(hi, value + 1); }
    return {kind, id, int(lo), int(hi), int(std::clamp(value, lo, hi))};
}

// The wheel's step on a handle: a 1024th of the view, a frame at the closest zoom.
inline uint32_t fine_step(const FrameView& view) { return std::max<uint32_t>(1, view.width() / 1024); }

// The wheel turned fast goes further a detent, as the MPC's encoder does: its pace in detents a second, eased over
// the last few, and up to 64 steps a detent from 12 a second up; slow turns stay a step each. A pause or a turn the
// other way starts the pace again.
struct WheelPace {
    uint32_t at = 0;
    float rate = 0;
    int way = 0;
};
constexpr float wheel_easy = 12;    // detents a second still a step each
constexpr unsigned wheel_most = 64; // steps a detent, spun as fast as it goes
inline unsigned wheel_gain(WheelPace& pace, uint32_t now, int delta) {
    int way = delta > 0 ? 1 : -1;
    uint32_t gap = pace.at && now >= pace.at ? now - pace.at : 1000;
    float rate = float(delta > 0 ? delta : -delta) * 1000.0f / float(std::max<uint32_t>(gap, 4));
    pace.rate = gap > 150 || way != pace.way ? std::min(rate, wheel_easy) : 0.6f * pace.rate + 0.4f * rate;
    pace.at = now; pace.way = way;
    if (pace.rate <= wheel_easy) return 1;
    return std::min(wheel_most, unsigned(std::pow(pace.rate / wheel_easy, 1.5f)));
}

inline uint32_t view_key(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    uint32_t h = 2166136261u;
    for (uint32_t v : {a, b, c, d, e}) { h ^= v; h *= 16777619u; }
    return h | 1u; // 0: none kept
}

// Trim: the last pad's handles in frames (its end the file's where the pad keeps 0).
struct TrimHandles {
    uint32_t frames = 0, start = 0, end = 0, loop = 0;
    uint32_t at(unsigned focus) const { return focus == 1 ? end : focus == 2 ? loop : start; }
};
inline TrimHandles trim_handles(const Project& project, const UiState& ui) {
    const auto& t = project.tracks[ui.track % track_count];
    const auto& pad = t.pads[t.last_pad % sampler_pads];
    TrimHandles h;
    if (!pad.used()) return h;
    h.frames = project.samples[pad.sample % sample_slots].frames;
    h.end = pad.end && pad.end < h.frames ? pad.end : h.frames;
    h.start = std::min(pad.start, h.end);
    h.loop = std::clamp(pad.loop_point(), h.start, h.end);
    return h;
}
inline uint32_t trim_key(const UiState& ui, const Project& project) {
    const auto& t = project.tracks[ui.track % track_count];
    unsigned pad = t.last_pad % sampler_pads;
    return view_key(ui.track, pad, t.pads[pad].sample, ui.trim_zoom, ui.trim_focus);
}
inline FrameView trim_view(const Project& project, const UiState& ui) {
    auto h = trim_handles(project, ui);
    if (!h.frames) return {};
    return zoom_now(ui.trim_view, trim_key(ui, project), h.frames, 0, h.frames, h.at(ui.trim_focus % 3), ui.trim_zoom);
}

// Chop: the region chopped, and the slice in hand's start and end.
inline uint32_t chop_frames(const Project& project, const UiState& ui) {
    return ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].frames : 0;
}
inline uint32_t chop_key(const UiState& ui) { return view_key(ui.chop_sample, ui.chop_from, ui.chop_slice, ui.chop_zoom, ui.chop_focus); }
inline FrameView chop_view(const Project& project, const UiState& ui) {
    uint32_t frames = chop_frames(project, ui);
    const auto& sl = ui.slices;
    if (!frames || !sl.count) return {};
    unsigned n = std::min<unsigned>(ui.chop_slice, sl.count - 1);
    uint32_t centre = ui.chop_focus ? sl.end(n, frames) : sl.start[n];
    return zoom_now(ui.chop_view, chop_key(ui), frames, sl.start[0], sl.end(sl.count - 1, frames), centre, ui.chop_zoom);
}

// A handle's pot or zoom touched: it is the one in focus (the view closes in round it; the wheel moves it).
// True when the focus moved, so the cards change.
inline bool focus_handle(UiState& ui, const Binding& b) {
    using K = Binding::Kind;
    int trim = -1, chop = -1;
    switch (b.kind) {
    case K::PadStart: trim = 0; break;
    case K::PadEnd: trim = 1; break;
    case K::PadLoopPoint: trim = 2; break;
    case K::TrimZoom: trim = int(b.id % 3); break;
    case K::ChopStart: chop = 0; break;
    case K::ChopEnd: chop = 1; break;
    case K::ChopZoom: chop = int(b.id % 2); break;
    case K::BrowseStart: case K::BrowseEnd: case K::BrowseZoom: { // Samples' trim sheet (shell/browse_trim.h)
        const int browse = b.kind == K::BrowseZoom ? int(b.id % 2) : b.kind == K::BrowseEnd;
        const bool moved = ui.browse.focus != browse;
        ui.browse.focus = uint8_t(browse);
        return moved;
    }
    default: return false;
    }
    if (trim >= 0) {
        bool moved = ui.trim_focus != trim;
        ui.trim_focus = uint8_t(trim);
        return moved;
    }
    bool moved = ui.chop_focus != chop || !ui.chop_fine;
    ui.chop_focus = uint8_t(chop); ui.chop_fine = true;
    return moved;
}
inline bool handle_kind(Binding::Kind k) {
    using K = Binding::Kind;
    return k == K::PadStart || k == K::PadEnd || k == K::PadLoopPoint || k == K::ChopStart || k == K::ChopEnd || k == K::BrowseStart
           || k == K::BrowseEnd;
}

} // namespace melodee
