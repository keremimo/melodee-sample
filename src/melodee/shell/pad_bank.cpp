// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/context.h"
#include "melodee/shell/track_copy.h"

namespace melodee {
namespace {
void say_bank(Context& c) { c.notice(std::string("Bank ") + char('A' + c.track().bank % sampler_banks)); }
} // namespace

void step_bank(Context& c, int way) {
    auto& t = c.track();
    t.bank = uint8_t((int(t.bank % sampler_banks) + way % int(sampler_banks) + int(sampler_banks)) % int(sampler_banks));
}

void pad_bank_button(Context& c, bool pressed) {
    auto& ui = c.ui;
    if (pressed) { ui.pad_bank = true; ui.pad_bank_used = false; return; }
    ui.pad_bank = false;
    if (ui.pad_bank_used) return; // held for a pad, a jump or a copy: no step
    if (ui.shift) ui.shift_used = true;
    if (ui.page == PageId::Punch) { c.project.punch_bank ^= 1; return; } // the other bank of punches
    if (c.track().engine == EngineKind::Sampler) { step_bank(c, ui.shift ? -1 : 1); say_bank(c); return; }
    c.notice("Pad Bank + pad: track"); // no banks on this track: say what the button does here
}

bool pad_bank_move(Context& c, int way) {
    auto& ui = c.ui;
    if (!ui.pad_bank || !way) return false;
    ui.pad_bank_used = true;
    if (ui.page == PageId::Punch) c.project.punch_bank ^= 1;
    else if (c.track().engine == EngineKind::Sampler) { step_bank(c, way); say_bank(c); }
    else c.notice("Banks: kit tracks");
    return true;
}

bool track_pad(Context& c, unsigned pad) {
    auto& ui = c.ui;
    if (!tracks_on_pads(ui)) return false;
    pad %= track_count;
    if (ui.pad_bank) ui.pad_bank_used = true;
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
