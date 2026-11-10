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
    case 2: // Double Seq: the pattern twice as long, played twice
        c.double_length();
        c.notice(job.name);
        return;
    case 3: // Count-In: a bar before recording (it counts in from phase 2)
        settings.count_in = !settings.count_in;
        toggled(c, job.name, settings.count_in);
        return;
    case 9: // Rec Quantize: recorded notes onto the grid (recording off the grid comes in phase 2)
        settings.rec_quantize = !settings.rec_quantize;
        toggled(c, job.name, settings.rec_quantize);
        return;
    case 11: // Song
        c.open(PageId::Song);
        return;
    case 15: // Project, the projects on the card
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
