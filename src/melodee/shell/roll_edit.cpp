// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// What Roll writes (shell/roll.h). A pad's note is one undo from press to
// release (Context::open_hold); erasing keeps its own (Step Edit's
// erase_event); the sheet's flags are a press on the page (PageEdit).
#include "melodee/shell/roll.h"
#include "melodee/shell/context.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/undo_scope.h"
#include <algorithm>
#include <string>

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
    if (roll_selected(c.ui, c.track().pattern)) { // the marked notes, one undo
        unsigned gone = 0;
        c.edit_pattern(c.ui.track, c.track().pattern, "Erase notes", [&] {
            gone = roll_marks_erase(c.pattern(), c.ui);
            c.sync_pattern(c.ui.track, c.track().pattern);
        });
        roll_select(c.pattern(), c.ui, lane);
        c.notice("Erase " + std::to_string(gone) + (gone == 1 ? " note" : " notes"));
        return true;
    }
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

namespace {
constexpr uint32_t marks_gesture = 0x5E1EC700u; // the wheel's moves of the marks: one undo until another control

bool selected(Context& c) { return c.ui.page == PageId::Roll && roll_selected(c.ui, c.track().pattern); }
} // namespace

bool roll_selection_wheel(Context& c, int way) {
    auto& ui = c.ui;
    const int lane = roll_lane(c.track());
    if (selected(c)) {
        if (ui.shift) ui.shift_used = true;
        c.touch_scope(pattern_scope(c.project, ui), marks_gesture | (ui.shift ? 1u : 0u));
        const int max_key = c.track().engine == EngineKind::Sampler ? int(sampler_last_key) : 127;
        bool moved = ui.shift ? roll_marks_pitch(c.pattern(), ui, way, lane, max_key) : roll_marks_move(c.pattern(), ui, way);
        if (!moved) { c.notice(ui.shift ? "No room to move the notes" : "The notes are at the pattern's edge"); return true; }
        c.sync_pattern(ui.track, c.track().pattern);
        roll_select(c.pattern(), ui, lane);
        return true;
    }
    if (!ui.shift) return false;
    ui.shift_used = true;
    roll_mark_walk(c.pattern(), ui, way, c.track().pattern, lane);
    return true;
}

void roll_shift_up(Context& c) {
    auto& ui = c.ui;
    if (ui.roll_marking) { // the marking done: the marks act now
        ui.roll_marking = false;
        unsigned n = unsigned(roll_marked_events(c.pattern(), ui).size());
        if (!n) roll_unmark(ui);
        else c.notice(std::to_string(n) + (n == 1 ? " note selected" : " notes selected"));
        return;
    }
    if (!ui.shift_used && selected(c)) { roll_unmark(ui); c.notice("Selection off"); } // a Shift tap
}

bool roll_selection_mute(Context& c) {
    if (!selected(c)) return false;
    if (c.ui.shift) c.ui.shift_used = true;
    bool muted = false;
    c.edit_pattern(c.ui.track, c.track().pattern, "Mute notes", [&] {
        muted = roll_marks_mute(c.pattern(), c.ui);
        c.sync_pattern(c.ui.track, c.track().pattern);
    });
    c.notice(muted ? "Notes muted" : "Notes unmuted");
    return true;
}

bool roll_selection_repeat(Context& c) {
    if (!selected(c)) return false;
    if (c.ui.shift) c.ui.shift_used = true;
    unsigned hits = 0;
    c.edit_pattern(c.ui.track, c.track().pattern, "Ratchet notes", [&] {
        hits = roll_marks_ratchet(c.pattern(), c.ui);
        c.sync_pattern(c.ui.track, c.track().pattern);
    });
    c.notice(hits > 1 ? "Ratchet x" + std::to_string(hits) : std::string("Ratchet off"));
    return true;
}

} // namespace melodee
