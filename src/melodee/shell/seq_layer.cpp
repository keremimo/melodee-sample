// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/seq_layer.h"
#include "melodee/engine/engine_host.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/context.h"
#include "melodee/shell/page_stack.h"
#include "melodee/shell/roll.h"
#include <algorithm>
#include <string>

namespace melodee {
namespace {
using B = SurfaceButton;

bool kit_track(Context& c) {
    auto engine = c.track().engine;
    return engine == EngineKind::Sampler;
}

std::string name_of(unsigned pattern) { return "P" + std::to_string(pattern % pattern_count + 1); }

// The pattern the selected track plays now (stopped: the one that will play).
unsigned playing_pattern(Context& c) {
    unsigned t = c.ui.track % track_count;
    return c.host.playing() ? unsigned(c.host.pattern_of(t)) % pattern_count : c.track().pattern % pattern_count;
}

// That pattern of the selected track, at the next bar while playing. An empty one starts as long as
// the one playing, so a new idea fits what is there.
void launch_one(Context& c, unsigned pattern) {
    unsigned t = c.ui.track % track_count, now = playing_pattern(c);
    pattern %= pattern_count;
    auto& target = c.patterns.at[t][pattern];
    uint32_t length = c.patterns.at[t][now].length;
    if (target.empty() && pattern != now && length && target.length != length) {
        target.length = length;
        c.sync_pattern(t, pattern);
    }
    c.launch(t, pattern);
    c.ui.pattern_bank = uint8_t(pattern / pattern_pads); // its bank in view (−/+ cross from one to the next)
    bool waits = c.host.playing() && pattern != now;
    c.notice(name_of(pattern) + (target.empty() ? " new" : "") + (waits ? ", next bar" : ""));
}

// Shift + pad: that column on every track holding notes there, and on the selected one: the scene (roadmap P5), its
// tempo with it.
void launch_column(Context& c, unsigned pattern) { c.launch_scene(pattern); }

// Seq Record held with Seq: the next empty pattern, armed (recording at once while playing).
void new_take(Context& c) {
    unsigned t = c.ui.track % track_count;
    int empty = -1;
    for (unsigned n = 0; n < pattern_count && empty < 0; ++n)
        if (c.patterns.at[t][n].empty()) empty = int(n);
    if (empty < 0) { c.notice("No empty pattern left"); return; }
    launch_one(c, unsigned(empty));
    if (c.ui.recording) return;
    if (c.host.playing()) c.seq_record();
    else c.ui.rec_armed = true;
    c.notice("New take: " + name_of(unsigned(empty)));
}

// Shift + Seq: Step Edit under the editor; again, back to the editor.
void step_edit(Context& c) {
    auto& ui = c.ui;
    ui.shift_used = true;
    if (ui.page == PageId::StepEdit) { c.open(seq_editor_page(ui, kit_track(c))); return; }
    if (ui.page == PageId::Roll) roll_select(c.pattern(), ui); // the note in focus is the event chosen
    c.open(PageId::StepEdit);
}
} // namespace

void seq_press(Context& c, bool pressed) {
    auto& ui = c.ui;
    if (pressed) {
        if (ui.shift) { step_edit(c); return; }
        ui.seq_held = true; ui.seq_used = false; ui.seq_left = false;
        ui.seq_down = c.now;
        ui.chain_building = false; // the first pad tapped in this hold starts a chain
        return;
    }
    if (!ui.seq_held) return;
    if (ui.seq_print) { // freeze plan F2c: Seq let go after Sample Record: the patterns frozen as they stand
        ui.seq_held = false; ui.seq_left = false;
        c.seq_print_end();
        return;
    }
    bool tap = !ui.seq_used && c.now - ui.seq_down < seq_sheet_ms;
    ui.seq_held = false; ui.seq_left = false;
    if (!tap) return;
    if (ui.shift) step_edit(c); // Shift came down during the tap
    else c.open(seq_tap_page(ui, kit_track(c)));
}

bool seq_pad(Context& c, unsigned pad, bool down) {
    auto& ui = c.ui;
    if (!down) { // a pattern pad let go: a tap launches it, unless it was copied from (the bank may have moved meanwhile)
        if (ui.pattern_held < 0 || unsigned(ui.pattern_held) % pattern_pads != pad % pattern_pads) return false;
        pad = unsigned(ui.pattern_held);
        if (!ui.pattern_copied && ui.seq_held && ui.chain_building) c.chain_add(pad); // seq-sheet2 B: after the first, a chain
        else if (!ui.pattern_copied) {
            launch_one(c, pad);
            if (ui.seq_held) { c.chain_start(pad); ui.chain_building = true; }
        }
        ui.pattern_held = -1;
        return true;
    }
    unsigned under = pad;
    pad = ui.pad_pattern(pad); // the pattern under it, in the bank in view (Seq held + Pad Bank)
    if (!ui.seq_held) return false;
    ui.seq_used = true;
    if (ui.seq_print) { c.seq_print_pad(under); return true; } // freeze plan F2c: that pattern frozen or thawed
    unsigned t = ui.track % track_count;
    if (ui.erase) {
        if (c.patterns.at[t][pad].empty()) c.notice(name_of(pad) + " is empty");
        else ask(c, UiState::Ask::ClearPattern, int(pad));
        return true;
    }
    if (ui.shift) { ui.shift_used = true; launch_column(c, pad); return true; }
    if (ui.pattern_held >= 0 && ui.pattern_held != int(pad)) { // hold one, tap another: copy
        unsigned from = unsigned(ui.pattern_held);
        c.edit_pattern(t, pad, "Copy pattern", [&] { c.copy_pattern(t, from, pad); });
        c.notice(name_of(from) + " to " + name_of(pad));
        ui.pattern_copied = true;
        return true;
    }
    ui.pattern_held = int8_t(pad);
    ui.pattern_copied = false;
    return true;
}

bool seq_button(Context& c, SurfaceButton button) {
    auto& ui = c.ui;
    if (!ui.seq_held || button == B::Seq || button == B::Shift) return false;
    ui.seq_used = true; // whatever it does, letting go of Seq then goes nowhere
    switch (button) {
    case B::TopLeft: case B::TopMiddle: case B::TopRight: {
        static const PageId editors[] {PageId::Steps, PageId::Roll, PageId::Motion};
        ui.seq_left = true; // the sheet stays away: the editor is what was asked for
        c.open(editors[button == B::TopLeft ? 0 : button == B::TopMiddle ? 1 : 2]);
        return true;
    }
    case B::Minus: case B::Plus: {
        if (ui.shift) return false; // undo / redo, as printed
        int from = int(c.track().pattern % pattern_count), way = button == B::Plus ? 1 : -1;
        int to = std::clamp(from + way, 0, int(pattern_count) - 1);
        if (to == from) { c.notice(way > 0 ? "P" + std::to_string(pattern_count) + " is the last" : "P1 is the first"); return true; }
        launch_one(c, unsigned(to));
        return true;
    }
    case B::SeqRecord:
        if (ui.shift) return false; // Recall, as printed
        if (c.frozen_edit(ui.track)) return true; // freeze plan F2c: a take on a frozen track asks first
        new_take(c);
        return true;
    case B::SampleRecord: // freeze plan F2c: the pattern pads freeze or thaw their patterns until Seq is let go
        if (ui.shift) return false; // Audio Recall, as printed
        if (!ui.seq_print) c.seq_print_start();
        return true;
    case B::WheelPush:
        ui.bpm_tenths = !ui.bpm_tenths;
        c.notice(ui.bpm_tenths ? "Tempo by tenths" : "Tempo by whole BPM");
        return true;
    default: return false; // the transport and the rest keep their jobs
    }
}

bool seq_wheel(Context& c, int delta) {
    auto& ui = c.ui;
    if (!ui.seq_held) return false;
    ui.seq_used = true;
    c.set_tempo(int(c.tempo_now()) + delta * (ui.bpm_tenths ? 1 : 10));
    return true;
}

bool pattern_copy_pad(Context& c, unsigned pad) {
    auto& ui = c.ui;
    if (ui.copying != UiState::Copying::Patterns) return false;
    pad = ui.pad_pattern(pad); // in the bank in view: Seq held + Pad Bank between the two takes it to another
    if (ui.seq_held) ui.seq_used = true;
    if (ui.copy_from < 0) { // first the pattern to copy (this track), then where (this or another track)
        if (c.patterns.at[ui.track][pad].empty()) { c.notice(name_of(pad) + " is empty"); return true; }
        ui.copy_from = int8_t(pad); ui.copy_track = ui.track;
        return true;
    }
    if (ui.copy_track == ui.track && unsigned(ui.copy_from) == pad) return true;
    std::string what = "Copy " + name_of(unsigned(ui.copy_from)) + " to " + name_of(pad);
    c.edit_pattern(ui.track, pad, what.c_str(), [&] { c.copy_pattern(ui.track, unsigned(ui.copy_from), pad, ui.copy_track); });
    c.notice(what);
    ui.stop_copying();
    return true;
}

} // namespace melodee
