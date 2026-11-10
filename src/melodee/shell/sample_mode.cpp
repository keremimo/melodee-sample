// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/sample_mode.h"
#include "melodee/engine/sampler.h"
#include "melodee/model/scales.h"
#include "melodee/shell/sample_library.h"
#include <cmath>
#include <cstdio>

namespace melodee {
namespace {
bool play_group(PageId page) { return page == PageId::Stage || page == PageId::Keys || page == PageId::Pads || page == PageId::Mix; }

// The last pad, when the track is a kit and the pad holds a sample; else says why.
PadSound* kit_pad(Context& c, const char* job) {
    if (c.track().engine != EngineKind::Sampler || !c.pad().used()) { c.notice(std::string(job) + ": no sample"); return nullptr; }
    return &c.pad();
}

// Sample alone: the play pages in turn, as before (Stage, the pads, Mix).
void open_sample_page(Context& c) {
    auto& ui = c.ui;
    if (play_group(ui.page)) {
        PageId keys = c.track().engine == EngineKind::Sampler ? PageId::Pads : PageId::Keys;
        c.open(ui.page == PageId::Stage ? keys : (ui.page == PageId::Keys || ui.page == PageId::Pads) ? PageId::Mix : PageId::Stage);
    } else c.open(PageId::Stage);
}
} // namespace

void sample_button(Context& c, bool pressed) {
    auto& ui = c.ui;
    if (pressed) {
        if (ui.shift) { ui.shift_used = true; ui.sample_held = false; c.open(PageId::InputConfig); return; } // as printed
        ui.sample_held = true; ui.sample_used = false;
        return;
    }
    if (ui.sample_held && !ui.sample_used) open_sample_page(c);
    ui.sample_held = false;
}

bool sample_pad(Context& c, unsigned pad) {
    auto& t = c.track();
    if (!c.ui.sample_held || t.engine != EngineKind::Sampler || t.levels) return false;
    unsigned kit = t.bank % sampler_banks * 16 + pad % 16;
    t.last_pad = uint8_t(kit);
    c.ui.sample_used = true;
    c.notice(pad_text("Picked", kit));
    return true;
}

void loop_button(Context& c) {
    if (c.track().engine != EngineKind::Sampler) { c.notice("Loop: a kit pad's"); return; }
    if (c.ui.shift) c.toggle_reverse();
    else c.toggle_loop();
}

// Chop: the last pad's region cut into slices (between its start and end, as
// the MPC chops); the slices stay while the same region comes back. Chop again
// goes back to Trim, as on the MPC.
void chop_button(Context& c) {
    auto& ui = c.ui;
    if (ui.shift) { c.toggle_note_on(); return; } // Note On, as printed
    if (ui.page == PageId::Chop) { c.open(PageId::Trim); return; }
    if (c.track().engine != EngineKind::Sampler || !c.pad().used()) return;
    const auto& pad = c.pad();
    if (ui.chop_sample != pad.sample || ui.chop_from != pad.start || ui.chop_to != pad.end) {
        ui.chop_sample = pad.sample; ui.chop_from = pad.start; ui.chop_to = pad.end;
        ui.chop_slice = 0; ui.chop_zoom = 0;
        c.rechop();
    }
    c.open(PageId::Chop);
}

void levels_button(Context& c) {
    auto& t = c.track();
    auto& ui = c.ui;
    bool kit = t.engine == EngineKind::Sampler;
    if (!t.used()) { c.notice("16 Levels: an empty track"); return; }
    if (ui.shift) { // the type: Velocity, Filter (kit pads), Tune, then Melodee's Scale
        static const LevelsKind kit_order[] {LevelsKind::Velocity, LevelsKind::Filter, LevelsKind::Tune, LevelsKind::Scale};
        static const LevelsKind synth_order[] {LevelsKind::Velocity, LevelsKind::Tune, LevelsKind::Scale};
        const LevelsKind* order = kit ? kit_order : synth_order;
        unsigned count = kit ? 4 : 3, at = 0;
        for (unsigned n = 0; n < count; ++n) if (unsigned(order[n]) == t.levels_kind) at = n;
        t.levels_kind = uint8_t(order[(at + 1) % count]);
        ui.levels_hit = -1;
        c.notice(std::string("16 Levels: ") + levels_kind_name(LevelsKind(t.levels_kind)));
        return;
    }
    if (ui.page == PageId::Levels) { t.levels = false; c.open(kit ? PageId::Pads : PageId::Keys); return; }
    // A kit: its last pad on all sixteen; a synth: its last note.
    if (!kit && t.levels_kind == unsigned(LevelsKind::Filter)) t.levels_kind = uint8_t(LevelsKind::Velocity);
    t.levels = true;
    if (kit) t.levels_pad = t.last_pad;
    ui.levels_hit = -1;
    c.open(PageId::Levels);
}

// Loop: the last pad plays on from its loop point after its end; on, Loop Lock
// starts it on (the loop point on the start), as on the MPC.
void Context::toggle_loop() {
    auto* pad = kit_pad(*this, "Loop");
    if (!pad) return;
    pad->loop = !pad->loop;
    if (pad->loop) pad->loop_lock = true;
    send_pad(ui.track, track().last_pad);
    dirty = true;
    notice(pad->loop ? "Loop on" : "Loop off");
}

void Context::toggle_note_on() {
    auto* pad = kit_pad(*this, "Note On");
    if (!pad) return;
    pad->play = pad->play == PadPlay::Gate ? PadPlay::OneShot : PadPlay::Gate;
    send_pad(ui.track, track().last_pad);
    dirty = true;
    notice(pad->play == PadPlay::Gate ? "Note On" : "One shot");
}

void Context::toggle_loop_lock() {
    auto* pad = kit_pad(*this, "Loop Lock");
    if (!pad) return;
    if (pad->loop_lock) pad->loop_start = pad->start; // free, from where it was
    pad->loop_lock = !pad->loop_lock;
    send_pad(ui.track, track().last_pad);
    dirty = true;
    notice(pad->loop_lock ? "Loop Lock on" : "Loop Lock off");
}

// The region's loudest sample to 0 dB: a gain on the pad (the file is not
// touched), one undo.
void Context::normalize() {
    auto* pad = kit_pad(*this, "Normalize");
    if (!pad) return;
    float peak = library ? library->peak(pad->sample, pad->start, pad->end) : -1.0f;
    if (peak < 0) { notice("Normalize: still loading"); return; }
    if (peak < 1e-5f) { notice("Normalize: silent"); return; }
    unsigned kit = track().last_pad % sampler_pads;
    Edit edit;
    edit.what = pad_text("Normalize", kit); edit.track = ui.track; edit.pad = int16_t(kit);
    edit.pad_before = pad_state(track(), kit);
    edit.pad_after = edit.pad_before;
    edit.pad_after.sound.gain = int16_t(std::clamp(std::lround(-2000.0 * std::log10(peak)), -2000L, 6000L));
    if (edit.pad_after.sound.gain == edit.pad_before.sound.gain) { notice("Normalized already"); return; }
    restore_pad(ui.track, kit, edit.pad_after);
    char text[32];
    std::snprintf(text, sizeof(text), "%s %+.1f dB", edit.what.c_str(), edit.pad_after.sound.gain / 100.0);
    notice(text);
    edits.push(std::move(edit));
    dirty = true;
}

// Warp's mode for the kit: Pitch moves speed and pitch together (Follow by
// Tape); Time stretch keeps the pitch (Follow by Beats or Tones, the one used last).
void Context::toggle_warp_mode() {
    if (track().engine != EngineKind::Sampler) { notice("Warp: kit pads"); return; }
    int how = track().params[sampler::FollowHow];
    if (how == 2) set_param(ui.track, sampler::FollowHow, ui.warp_stretch % 2);
    else { ui.warp_stretch = uint8_t(how); set_param(ui.track, sampler::FollowHow, 2); }
    dirty = true;
    notice(how == 2 ? "Warp: Time stretch" : "Warp: Pitch");
}

} // namespace melodee
