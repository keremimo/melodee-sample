// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The wheel after a pot, as the MPC's encoder takes the parameter a knob
// turned last: on Trim and Chop it moves the handle in focus (start, end,
// loop point; a slice's start or end) by a 1024th of the view on screen, so
// zoomed all the way in (shell/zoom_view.h) a detent is one frame; spun fast,
// up to 64 times that a detent (wheel_gain), so a quick turn still travels. With Zero
// Snap on, a Chop cut steps to the next zero crossing the way it turns,
// never back to the one it left. Past the view's edge the view goes along.
#include "melodee/shell/context.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/zoom_view.h"

namespace melodee {

namespace {
WheelPace pace; // MIDI thread: how fast the wheel turns
}

void Context::fine_edit(const Binding& b, int delta) {
    using K = Binding::Kind;
    if (!delta) return;
    if (!handle_kind(b.kind)) { apply(b, b.value + delta); return; }
    focus_handle(ui, b);
    int64_t steps = int64_t(delta) * wheel_gain(pace, now, delta);
    Binding exact = b; // the handle's own limits, not the pot's stretch of them
    if (b.kind == K::PadStart || b.kind == K::PadEnd || b.kind == K::PadLoopPoint) {
        auto h = trim_handles(project, ui);
        if (!h.frames) return;
        uint32_t last = h.end > h.start ? h.end - 1 : h.start;
        int64_t step = int64_t(fine_step(trim_view(project, ui))) * steps;
        if (b.kind == K::PadStart) { exact.min = 0; exact.max = int(last); }
        if (b.kind == K::PadEnd) { exact.min = int(h.start + 1); exact.max = int(h.frames); }
        if (b.kind == K::PadLoopPoint) { exact.min = int(h.start); exact.max = int(std::max(h.start + 1, last)); }
        int64_t at = int64_t(h.at(ui.trim_focus % 3)) + step;
        apply(exact, int(std::clamp<int64_t>(at, exact.min, exact.max)));
        return;
    }
    uint32_t frames = chop_frames(project, ui);
    const auto& sl = ui.slices;
    unsigned n = unsigned(b.id);
    if (!frames || n >= sl.count) return;
    uint32_t was = b.kind == K::ChopStart ? sl.start[n] : sl.end(n, frames);
    int64_t at = std::clamp<int64_t>(int64_t(was) + int64_t(fine_step(chop_view(project, ui))) * steps, 0, frames);
    uint32_t cut = uint32_t(at);
    if (ui.settings.zero_snap && library && ui.chop_sample < sample_slots) {
        uint32_t snapped = library->zero_crossing_toward(ui.chop_sample, cut, delta);
        if ((delta > 0 && snapped > was) || (delta < 0 && snapped < was)) cut = snapped;
    }
    exact.min = 0; exact.max = int(frames);
    exact_cut = true;
    apply(exact, int(cut));
    exact_cut = false;
}

void Context::trim_moved(const FrameView& was) {
    if (!ui.trim_zoom) { ui.trim_view = {}; return; }
    auto h = trim_handles(project, ui);
    zoom_follow(ui.trim_view, trim_key(ui, project), was, h.at(ui.trim_focus % 3), h.frames, ui.trim_zoom);
}

void Context::chop_moved(const FrameView& was) {
    uint32_t frames = chop_frames(project, ui);
    const auto& sl = ui.slices;
    if (!ui.chop_zoom || !frames || !sl.count) { ui.chop_view = {}; return; }
    unsigned n = std::min<unsigned>(ui.chop_slice, sl.count - 1);
    zoom_follow(ui.chop_view, chop_key(ui), was, ui.chop_focus ? sl.end(n, frames) : sl.start[n], frames, ui.chop_zoom);
}

} // namespace melodee
