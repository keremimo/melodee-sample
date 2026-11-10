// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Roll's cursor and view (shell/roll.h): pure, on the pattern and the UI state.
#include "melodee/shell/roll.h"
#include <algorithm>
#include <cstdlib>

namespace melodee {
namespace {
void put_cursor(UiState& ui, unsigned step) {
    ui.step_cursor = int16_t(std::min<unsigned>(step, 0x7fff));
    ui.steps_bar = uint8_t(std::min<unsigned>(step / bar_steps, 255));
}

// The key of the note starting in `step` nearest to `key`; `key` when none starts there.
int nearest_key(const Pattern& pattern, unsigned step, int key) {
    auto in = step_events(pattern, step);
    int best = key, distance = 1 << 20;
    for (unsigned n = 0; n < in.count; ++n) {
        int k = pattern.events[in.first + n].key, d = std::abs(k - key);
        if (d == 0) return key;
        if (d < distance) { distance = d; best = k; }
    }
    return best;
}
} // namespace

unsigned roll_cursor(const Pattern& pattern, const UiState& ui) {
    return unsigned(std::max<int>(ui.step_cursor, 0)) % pattern.steps();
}

unsigned roll_first(const Pattern& pattern, const UiState& ui) {
    unsigned steps = pattern.steps(), last = steps > roll_columns ? steps - roll_columns : 0;
    return std::min<unsigned>(unsigned(std::max<int>(ui.roll_view, 0)), last);
}

int roll_focus(const Pattern& pattern, const UiState& ui) {
    auto in = step_events(pattern, roll_cursor(pattern, ui));
    for (unsigned n = 0; n < in.count; ++n)
        if (pattern.events[in.first + n].key == ui.roll_key) return int(in.first + n);
    return -1;
}

void roll_follow(const Pattern& pattern, UiState& ui) {
    int cursor = int(roll_cursor(pattern, ui)), first = int(roll_first(pattern, ui));
    int columns = int(roll_columns), margin = int(roll_margin);
    if (cursor < first + margin) first = cursor - margin;
    if (cursor > first + columns - 1 - margin) first = cursor - columns + 1 + margin;
    int last = std::max(0, int(pattern.steps()) - columns);
    ui.roll_view = int16_t(std::clamp(first, 0, last));
    int top = ui.roll_top, key = ui.roll_key, rows = int(roll_rows);
    if (key > top - 1) top = key + 1;              // a row above the cursor's key
    if (key < top - (rows - 2)) top = key + rows - 2; // and one below
    ui.roll_top = uint8_t(std::clamp(top, rows - 1, 127));
}

void roll_select(const Pattern& pattern, UiState& ui) {
    int at = roll_focus(pattern, ui);
    auto in = step_events(pattern, roll_cursor(pattern, ui));
    ui.step_event = at >= 0 ? uint8_t(std::min<size_t>(size_t(at) - in.first, 255)) : 0;
}

void roll_step(const Pattern& pattern, UiState& ui, int way) {
    int step = std::clamp<int>(int(roll_cursor(pattern, ui)) + way, 0, int(pattern.steps()) - 1);
    put_cursor(ui, unsigned(step));
    ui.roll_key = uint8_t(nearest_key(pattern, unsigned(step), ui.roll_key));
    roll_follow(pattern, ui);
    roll_select(pattern, ui);
}

bool roll_jump(const Pattern& pattern, UiState& ui, int way) {
    unsigned q = pattern.q(), cursor = roll_cursor(pattern, ui);
    int to = -1;
    for (const auto& e : pattern.events) { // in order of their ticks
        if (e.tick >= pattern.length) break;
        unsigned step = e.tick / q;
        if (way > 0 && step > cursor) { to = int(step); break; }
        if (way < 0 && step < cursor) to = int(step);
    }
    if (to < 0) return false;
    put_cursor(ui, unsigned(to));
    ui.roll_key = uint8_t(nearest_key(pattern, unsigned(to), ui.roll_key));
    roll_follow(pattern, ui);
    roll_select(pattern, ui);
    return true;
}

void roll_pitch(const Pattern& pattern, UiState& ui, int way) {
    auto in = step_events(pattern, roll_cursor(pattern, ui));
    int key = ui.roll_key, next = -1;
    for (unsigned n = 0; n < in.count; ++n) { // the chord first
        int k = pattern.events[in.first + n].key;
        if (way > 0 && k > key && (next < 0 || k < next)) next = k;
        if (way < 0 && k < key && (next < 0 || k > next)) next = k;
    }
    ui.roll_key = uint8_t(next >= 0 ? next : std::clamp(key + (way > 0 ? 1 : -1), 0, 127));
    roll_follow(pattern, ui);
    roll_select(pattern, ui);
}

std::string roll_position(uint32_t tick, unsigned bar_ticks, unsigned beat_ticks, unsigned q) {
    bar_ticks = std::max(1u, bar_ticks); beat_ticks = std::max(1u, beat_ticks); q = std::max(1u, q);
    unsigned bar = tick / bar_ticks + 1, beat = tick % bar_ticks / beat_ticks + 1;
    std::string s = std::to_string(bar) + "." + std::to_string(beat);
    if (q < beat_ticks) s += "." + std::to_string(tick % beat_ticks / q + 1); // the step within the beat, as on the MPC
    return s;
}

} // namespace melodee
