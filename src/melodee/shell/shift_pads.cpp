// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/shift_pads.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/shift_pad_jobs.h"

namespace melodee {
namespace {
// A toggle changed: say so, and keep it on the card with Melodee's settings.
void toggled(Context& c, const char* name, bool on) {
    c.notice(std::string(name) + (on ? " on" : " off"));
    if (c.store) c.store->save_settings(c.ui.settings.text());
}
} // namespace

void shift_pad(Context& c, unsigned pad) {
    auto& settings = c.ui.settings;
    const auto& job = shift_pad_job(pad);
    switch (pad % 16) {
    case 0: // Full Level: every hit at 127, or back to the pad response it came from
        settings.toggle_full_level();
        toggled(c, job.name, settings.full_level());
        return;
    case 1: // Half Seq: the first half of the pattern
        c.half_length();
        return;
    case 2: // Double Seq: the pattern twice as long, played twice (or the new half empty: Settings)
        c.double_length();
        return;
    case 5: // Half Speed: every note twice as far apart
        c.half_speed();
        return;
    case 6: // Double Speed: every note half as far apart
        c.double_speed();
        return;
    case 12: // Trim Sample: the last pad's region as a file of its own (one undo); then Stage's TRIM, as the MPC lands on Trim
        c.trim_sample();
        if (c.track().engine == EngineKind::Sampler) c.open(PageId::Trim);
        return;
    case 13: // Time Correct: the chosen pads' notes onto Q
        c.open(PageId::TimeCorrect);
        return;
    case 3: // Count-In: a bar of the metronome before Play starts a take
        settings.count_in = !settings.count_in;
        toggled(c, job.name, settings.count_in);
        return;
    case 9: // Rec Quantize: its page, on or off (B3) and the grid recorded notes land on (the pattern's Q or its own)
        c.open(PageId::RecQuantize);
        return;
    case 14: // Warp: Time stretch or Pitch for the kit (its Follow how)
        c.toggle_warp_mode();
        return;
    case 10: // Resample: then a pad, which gets the pattern rendered
        c.resample();
        return;
    case 4: // Compressor: the master's (parity phase 7)
        c.open(PageId::Compressor);
        break;
    case 7: // Settings, as doors (MIDI Config one of them; Kerem's choice over the printed MIDI Config)
        c.ui.midi_editing = false; c.ui.settings_editing = false; c.ui.settings_in = false;
        c.open(PageId::SettingsPage);
        return;
    case 8: // Fader: what the selected track's fader follows, PIN, on / off (parity phase 8; round 2 fader C)
        c.open(PageId::FaderPage);
        return;
    case 11: // Song
        c.open(PageId::Song);
        return;
    case 15: // Project: SAVE, OPEN and NEW, the projects on the card behind OPEN
        c.ui.project_list = false; c.ui.sheet_open = false; c.ui.sheet_more = false; c.ui.move_open = false;
        c.open(PageId::Project);
        if (c.store) c.store->list(c.project_folder);
        c.ui.project_rows = 64; // until the list arrives
        return;
    default: // printed, not built yet
        c.notice(std::string(job.name) + ": phase " + std::to_string(job.phase)); // short enough for the header
        return;
    }
}

} // namespace melodee
