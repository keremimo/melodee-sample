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
bool play_group(PageId page) { return page == PageId::Stage || page == PageId::Tracks || page == PageId::Keys || page == PageId::Pads; }

// The last pad, when the track is a kit and the pad holds a sample; else says why.
PadSound* kit_pad(Context& c, const char* job) {
    if (c.track().engine != EngineKind::Sampler || !c.pad().used()) { c.notice(std::string(job) + ": no sample"); return nullptr; }
    return &c.pad();
}

// Sample alone, where you go to look: Stage, then Tracks, then the pads (Keys, or Pads on a kit), round;
// from any other page Stage.
void open_sample_page(Context& c) {
    auto& ui = c.ui;
    PageId keys = c.track().engine == EngineKind::Sampler ? PageId::Pads : PageId::Keys;
    if (!play_group(ui.page)) c.open(PageId::Stage);
    else c.open(ui.page == PageId::Stage ? PageId::Tracks : ui.page == PageId::Tracks ? keys : PageId::Stage);
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
    if (c.refuse_instrument("Chop")) return;
    const auto& pad = c.pad();
    if (ui.chop_sample != pad.sample || ui.chop_from != pad.start || ui.chop_to != pad.end) {
        c.keep_chop(); // roadmap P3: the region left keeps its slices in the project
        ui.chop_sample = pad.sample; ui.chop_from = pad.start; ui.chop_to = pad.end;
        ui.chop_slice = 0; ui.chop_zoom = 0;
        if (!c.restore_chop()) c.rechop(); // as it was left, else cut afresh
    }
    c.open(PageId::Chop);
}

void levels_button(Context& c) {
    auto& t = c.track();
    auto& ui = c.ui;
    bool kit = t.engine == EngineKind::Sampler;
    // Loop (docs/LOOPER.md): 16 Levels opens the Looper, its pads the loop grid; again goes back.
    if (!ui.shift && t.levels_kind == unsigned(LevelsKind::Loop)) {
        if (ui.page == PageId::Looper) c.back(); else c.open(PageId::Looper);
        return;
    }
    if (!t.used() && !ui.shift) { c.notice("16 Levels: an empty track"); return; }
    if (ui.shift) { // the type: Melodee's Scale first (the most played), Tune, Velocity, Filter (kit pads), the Looper's Loop
        static const LevelsKind kit_order[] {LevelsKind::Scale, LevelsKind::Tune, LevelsKind::Velocity, LevelsKind::Filter, LevelsKind::Loop};
        static const LevelsKind synth_order[] {LevelsKind::Scale, LevelsKind::Tune, LevelsKind::Velocity, LevelsKind::Loop};
        static const LevelsKind empty_order[] {LevelsKind::Scale, LevelsKind::Loop};
        static const LevelsKind drum_order[] {LevelsKind::Velocity, LevelsKind::Loop}; // Drums pads: another pitch is another drum
        bool drums = drum_pads(t);
        const LevelsKind* order = !t.used() ? empty_order : kit ? kit_order : drums ? drum_order : synth_order;
        unsigned count = !t.used() ? 2 : kit ? 5 : drums ? 2 : 4, at = 0;
        for (unsigned n = 0; n < count; ++n) if (unsigned(order[n]) == t.levels_kind) at = n;
        t.levels_kind = uint8_t(order[(at + 1) % count]);
        ui.levels_hit = -1;
        c.notice(std::string("16 Levels: ") + levels_kind_name(LevelsKind(t.levels_kind)));
        return;
    }
    if (ui.page == PageId::Levels) { t.levels = false; c.open(kit ? PageId::Pads : PageId::Keys); return; }
    // A kit: its last pad on all sixteen; a synth: its last note.
    if (!kit && t.levels_kind == unsigned(LevelsKind::Filter)) t.levels_kind = uint8_t(LevelsKind::Velocity);
    if (drum_pads(t)) t.levels_kind = uint8_t(LevelsKind::Velocity); // Loop went to the Looper above
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

// Layers (roadmap P3), Shift + B1: the browser, whose Push then loads onto the layer in hand
// (layer 1: the pad itself, as Sample Select's Push does).
void Context::layer_load() {
    if (track().engine != EngineKind::Sampler) return;
    unsigned n = ui.pad_layer % pad_layers;
    if (n && !pad().used()) { notice("Layer 1 first"); return; }
    if (n && project.samples[pad().sample % sample_slots].instrument()) { notice("An instrument has its own layers"); return; }
    open(PageId::Samples);
    ui.layer_load = n ? int8_t(n) : int8_t(-1);
}

// Shift + B2: the layer in hand off the pad (layer 1 is the pad's own sample).
void Context::layer_clear() {
    auto* pad = kit_pad(*this, "Clear layer");
    if (!pad) return;
    unsigned n = ui.pad_layer % pad_layers;
    if (!n) { notice("Layer 1 is the pad's"); return; }
    if (!pad->layers[n - 1].used()) { notice("Layer " + std::to_string(n + 1) + " is empty"); return; }
    pad->layers[n - 1] = PadLayer {};
    send_pad(ui.track, track().last_pad);
    dirty = true;
    notice("Layer " + std::to_string(n + 1) + " cleared");
}

// The region's loudest sample to 0 dB: a gain on the pad (the file is not
// touched), one undo.
void Context::normalize() {
    auto* pad = kit_pad(*this, "Normalize");
    if (!pad || refuse_instrument("Normalize")) return;
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
    if (refuse_instrument("Warp")) return;
    int how = track().params[sampler::FollowHow];
    if (how == 2) set_param(ui.track, sampler::FollowHow, ui.warp_stretch % 2);
    else { ui.warp_stretch = uint8_t(how); set_param(ui.track, sampler::FollowHow, 2); }
    dirty = true;
    notice(how == 2 ? "Warp: Time stretch" : "Warp: Pitch");
}

} // namespace melodee
