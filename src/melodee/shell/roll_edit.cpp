// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// What Roll writes (shell/roll.h). A pad's note is one undo from press to
// release (Context::open_hold); erasing keeps its own (Step Edit's
// erase_event); the sheet's flags are a press on the page (PageEdit).
#include "melodee/shell/roll.h"
#include "melodee/shell/context.h"
#include "melodee/shell/step_edit.h"
#include <algorithm>

namespace melodee {

void roll_write(Context& c, int key, uint8_t velocity, int pitch, int cutoff) {
    auto& ui = c.ui;
    auto& pattern = c.pattern();
    if (key < 0 || key > int(sampler_last_key)) return; // a kit's keys run past 127
    const int lane = roll_lane(c.track());
    Event e;
    e.key = uint8_t(key);
    e.pitch = int8_t(std::clamp(pitch, -48, 48));
    const int row = roll_row(e, lane);
    if (row < 0 || row > 255) return;
    ui.roll_key = uint8_t(row);
    if (roll_focus(pattern, ui, lane) < 0) { // not there yet: written at the start of the cursor's step
        e.tick = roll_cursor(pattern, ui) * pattern.q();
        uint32_t length = ui.roll_new ? ui.roll_new : pattern.q();
        e.length = uint16_t(std::clamp<uint32_t>(std::min(length, pattern.length - e.tick), 1, 0xffff));
        e.velocity = uint8_t(std::clamp<int>(velocity, 1, 127));
        if (cutoff >= 0 && c.track().engine == EngineKind::Sampler) e.cutoff = uint8_t(std::min(cutoff, 100) + 1); // as Seq Record keeps it
        if (!pattern.add(e)) { c.notice("The pattern is full"); return; }
        c.sync_pattern(ui.track, c.track().pattern);
    }
    roll_follow(pattern, ui);
    roll_select(pattern, ui, lane);
}

bool roll_erase(Context& c) {
    const int lane = roll_lane(c.track());
    roll_select(c.pattern(), c.ui, lane);
    if (roll_focus(c.pattern(), c.ui, lane) < 0) return false;
    erase_event(c); // one undo, or part of a take; it says what went
    roll_select(c.pattern(), c.ui, lane);
    return true;
}

void roll_flag(Context& c, uint8_t flag) {
    auto& pattern = c.pattern();
    int at = roll_focus(pattern, c.ui, roll_lane(c.track()));
    if (at < 0) return;
    pattern.events[size_t(at)].flags ^= flag;
    c.sync_pattern(c.ui.track, c.track().pattern);
}

} // namespace melodee
