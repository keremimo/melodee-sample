// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/pad_bank.h"
#include "melodee/model/scales.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/context.h"
#include "melodee/shell/track_copy.h"
#include <algorithm>

namespace melodee {
namespace {
void say_bank(Context& c) { c.notice(std::string("Bank ") + char('A' + c.track().bank % sampler_banks)); }

// Roll on a synth: the pads an octave up or down, and the notes they now play.
void step_octave(Context& c, int way) {
    auto& t = c.track();
    t.octave = int8_t(std::clamp(t.octave + way, 0, 7));
    c.notice(std::string("Pads ") + note_name(pad_note(c.project, t, 0).key) + "–" + note_name(pad_note(c.project, t, 15).key));
}
} // namespace

void step_bank(Context& c, int way) {
    auto& t = c.track();
    t.bank = uint8_t((int(t.bank % sampler_banks) + way % int(sampler_banks) + int(sampler_banks)) % int(sampler_banks));
}

void pad_bank_button(Context& c, bool pressed) {
    auto& ui = c.ui;
    if (pressed) { ui.pad_bank = true; ui.pad_bank_used = false; ui.pad_bank_down = c.now; return; }
    if (!ui.pad_bank) return; // its press went elsewhere (a question took it): no step
    ui.pad_bank = false;
    if (ui.pad_bank_used) return; // held for a pad, a jump or a copy: no step
    if (ui.shift) ui.shift_used = true;
    if (ui.page == PageId::Punch) { c.step_punch_bank(ui.shift ? -1 : 1); return; } // the next bank of punches
    if (c.track().engine == EngineKind::Sampler) { step_bank(c, ui.shift ? -1 : 1); say_bank(c); return; }
    if (ui.page == PageId::Roll) step_octave(c, ui.shift ? -1 : 1); // Roll on a synth: −/+ are its pitch
}

bool pad_bank_move(Context& c, int way) {
    auto& ui = c.ui;
    if (!ui.pad_bank || !way) return false;
    ui.pad_bank_used = true;
    if (ui.page == PageId::Punch) c.step_punch_bank(way);
    else if (c.track().engine == EngineKind::Sampler) { step_bank(c, way); say_bank(c); }
    else c.notice("Banks: kit tracks");
    return true;
}

bool track_pad(Context& c, unsigned pad) {
    auto& ui = c.ui;
    if (!tracks_on_pads(ui)) return false;
    pad %= track_count;
    if (ui.pad_bank) ui.pad_bank_used = true;
    if (ui.erase && ui.copying != UiState::Copying::Tracks) { // Erase + pad: that track cleared, once B3 says so
        c.select_track(pad);
        if (ui.recording) c.notice("Clear track: not in a take");
        else if (!track_clearable(c.project.tracks[pad], c.patterns.at[pad])) c.notice("Track " + std::to_string(pad + 1) + " is empty");
        else ask(c, UiState::Ask::ClearTrack, int(pad));
        return true;
    }
    if (ui.copying != UiState::Copying::Tracks) { c.select_track(pad); return true; }
    if (ui.copy_from < 0) { // the track to copy
        if (!c.project.tracks[pad].used()) { c.notice("Track " + std::to_string(pad + 1) + " is empty"); return true; }
        ui.copy_from = int8_t(pad);
        c.select_track(pad);
        return true;
    }
    if (unsigned(ui.copy_from) == pad) return true; // then where it goes
    copy_track(c, unsigned(ui.copy_from), pad);
    c.select_track(pad);
    ui.stop_copying();
    return true;
}

} // namespace melodee
