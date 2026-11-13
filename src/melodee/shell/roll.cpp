// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Roll's cursor and view (shell/roll.h): pure, on the pattern and the UI state.
#include "melodee/shell/roll.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace melodee {
namespace {
void put_cursor(UiState& ui, unsigned step) {
    ui.step_cursor = int16_t(std::min<unsigned>(step, 0x7fff));
    ui.steps_bar = uint8_t(std::min<unsigned>(step / bar_steps, 255));
}
} // namespace

unsigned roll_cursor(const Pattern& pattern, const UiState& ui) {
    return unsigned(std::max<int>(ui.step_cursor, 0)) % pattern.steps();
}

unsigned roll_first(const Pattern& pattern, const UiState& ui) {
    unsigned steps = pattern.steps(), last = steps > roll_columns ? steps - roll_columns : 0;
    return std::min<unsigned>(unsigned(std::max<int>(ui.roll_view, 0)), last);
}

int roll_focus(const Pattern& pattern, const UiState& ui, int lane) {
    auto in = step_events(pattern, roll_cursor(pattern, ui));
    for (unsigned n = 0; n < in.count; ++n)
        if (roll_row(pattern.events[in.first + n], lane) == int(ui.roll_key)) return int(in.first + n);
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
    ui.roll_top = uint8_t(std::clamp(top, rows - 1, std::max(127, key + 1))); // a kit's keys run past 127
}

void roll_select(const Pattern& pattern, UiState& ui, int lane) {
    int at = roll_focus(pattern, ui, lane);
    auto in = step_events(pattern, roll_cursor(pattern, ui));
    ui.step_event = at >= 0 ? uint8_t(std::min<size_t>(size_t(at) - in.first, 255)) : 0;
}

void roll_open(const Pattern& pattern, UiState& ui, int lane) {
    int top = ui.roll_top, low = top - int(roll_rows) + 1, best = -1;
    int64_t at = int64_t(roll_cursor(pattern, ui)) * pattern.q(), distance = INT64_MAX;
    for (const auto& e : pattern.events) {
        if (e.tick >= pattern.length) break;
        int row = roll_row(e, lane);
        if (row < 0) continue;
        if (row >= low && row <= top) return; // a note in view: the view stays as it was left
        int64_t d = std::llabs(int64_t(e.tick) - at);
        if (d < distance) { distance = d; best = row; }
    }
    if (best < 0) return;
    ui.roll_key = uint8_t(best);
    roll_follow(pattern, ui);
    roll_select(pattern, ui, lane);
}

void roll_step(const Pattern& pattern, UiState& ui, int way, int lane) {
    // A detent at a time: the next note of the chord in the step (up going on, down going back), then the next step,
    // entered on its lowest note going on and its highest going back; an empty step keeps the pitch.
    int dir = way > 0 ? 1 : -1, last = int(pattern.steps()) - 1;
    for (int n = std::abs(way); n > 0; --n) {
        int step = int(roll_cursor(pattern, ui)), key = ui.roll_key, next = -1;
        auto in = step_events(pattern, unsigned(step));
        for (unsigned e = 0; e < in.count; ++e) {
            int k = roll_row(pattern.events[in.first + e], lane);
            if (k < 0 || (k - key) * dir <= 0) continue;
            if (next < 0 || (k - next) * dir < 0) next = k;
        }
        if (next >= 0) { ui.roll_key = uint8_t(next); continue; }
        if (step + dir < 0 || step + dir > last) break;
        step += dir;
        put_cursor(ui, unsigned(step));
        in = step_events(pattern, unsigned(step));
        int edge = -1;
        for (unsigned e = 0; e < in.count; ++e) {
            int k = roll_row(pattern.events[in.first + e], lane);
            if (k >= 0 && (edge < 0 || (k - edge) * dir < 0)) edge = k;
        }
        if (edge >= 0) ui.roll_key = uint8_t(edge);
    }
    roll_follow(pattern, ui);
    roll_select(pattern, ui, lane);
}

void roll_pitch(const Pattern& pattern, UiState& ui, int way, int lane) {
    auto in = step_events(pattern, roll_cursor(pattern, ui));
    int key = ui.roll_key, next = -1;
    for (unsigned n = 0; n < in.count; ++n) { // the chord first
        int k = roll_row(pattern.events[in.first + n], lane);
        if (k < 0) continue;
        if (way > 0 && k > key && (next < 0 || k < next)) next = k;
        if (way < 0 && k < key && (next < 0 || k > next)) next = k;
    }
    ui.roll_key = uint8_t(next >= 0 ? next : std::clamp(key + (way > 0 ? 1 : -1), 0, 127));
    roll_follow(pattern, ui);
    roll_select(pattern, ui, lane);
}

namespace {
bool same(const Event& e, const UiState::RollMark& m) { return e.tick == m.tick && e.key == m.key && e.pitch == m.pitch; }
UiState::RollMark mark_of(const Event& e) { return {e.tick, e.key, e.pitch}; }
// The marks onto the events as they are now (after a move, in their new place).
void remark(UiState& ui, const std::vector<Event>& events) {
    ui.roll_marked = uint8_t(std::min<size_t>(events.size(), UiState::roll_marks_max));
    for (unsigned n = 0; n < ui.roll_marked; ++n) ui.roll_marks[n] = mark_of(events[n]);
}
// The marked events taken out of the pattern, in order; the rest stay.
std::vector<Event> take_marked(Pattern& pattern, const UiState& ui) {
    std::vector<Event> out;
    auto at = roll_marked_events(pattern, ui);
    for (auto n = at.rbegin(); n != at.rend(); ++n) {
        out.insert(out.begin(), pattern.events[*n]);
        pattern.events.erase(pattern.events.begin() + long(*n));
    }
    return out;
}
bool taken(const Pattern& pattern, const Event& e) { // a note of the pattern on that tick, key and pitch
    for (size_t n = pattern.lower(e.tick); n < pattern.events.size() && pattern.events[n].tick == e.tick; ++n)
        if (pattern.events[n].key == e.key && pattern.events[n].pitch == e.pitch) return true;
    return false;
}
// `moved` put back `way` on (shift), hopping a further one at a time past notes in the way; `fits` says where a note
// may go. Put back where they were when they would leave the pattern.
template <class Shift, class Fits> bool put_back(Pattern& pattern, std::vector<Event>& moved, int way, Shift&& shift, Fits&& fits) {
    for (int by = way;; by += way > 0 ? 1 : -1) {
        bool out = false, blocked = false;
        for (const auto& e : moved) {
            Event to = e;
            if (!shift(to, by) || !fits(to)) { out = true; break; }
            if (taken(pattern, to)) blocked = true;
        }
        if (out) { for (const auto& e : moved) pattern.add(e); return false; }
        if (blocked) continue;
        for (auto& e : moved) { shift(e, by); pattern.add(e); }
        return true;
    }
}
} // namespace

bool roll_is_marked(const Pattern& pattern, const UiState& ui, size_t n) {
    if (n >= pattern.events.size()) return false;
    for (unsigned m = 0; m < ui.roll_marked; ++m) if (same(pattern.events[n], ui.roll_marks[m])) return true;
    return false;
}

std::vector<size_t> roll_marked_events(const Pattern& pattern, const UiState& ui) {
    std::vector<size_t> out;
    if (!ui.roll_marked) return out;
    for (size_t n = 0; n < pattern.events.size(); ++n) if (roll_is_marked(pattern, ui, n)) out.push_back(n);
    return out;
}

void roll_mark_walk(const Pattern& pattern, UiState& ui, int way, unsigned pattern_index, int lane) {
    if (ui.roll_mark_track != ui.track || ui.roll_mark_pattern != pattern_index) ui.roll_marked = 0;
    ui.roll_mark_track = ui.track; ui.roll_mark_pattern = uint16_t(pattern_index);
    auto mark = [&] {
        int at = roll_focus(pattern, ui, lane);
        if (at < 0 || roll_is_marked(pattern, ui, size_t(at)) || ui.roll_marked >= UiState::roll_marks_max) return;
        ui.roll_marks[ui.roll_marked++] = mark_of(pattern.events[size_t(at)]);
    };
    if (!ui.roll_marking) { ui.roll_marking = true; mark(); } // the note the walk starts on
    for (int n = std::abs(way); n > 0; --n) {
        roll_step(pattern, ui, way > 0 ? 1 : -1, lane);
        mark();
    }
}

bool roll_marks_move(Pattern& pattern, UiState& ui, int way) {
    if (!way) return false;
    const int64_t q = pattern.q(), length = pattern.length;
    auto moved = take_marked(pattern, ui);
    if (moved.empty()) return false;
    int step = 0;
    bool done = put_back(pattern, moved, way,
                         [&](Event& e, int by) {
                             int64_t tick = int64_t(e.tick) + by * q;
                             if (tick < 0 || tick >= length) return false;
                             e.tick = uint32_t(tick); step = by;
                             return true;
                         },
                         [](const Event&) { return true; });
    if (!done) return false;
    remark(ui, moved);
    put_cursor(ui, unsigned(std::clamp<int>(int(roll_cursor(pattern, ui)) + step, 0, int(pattern.steps()) - 1)));
    roll_follow(pattern, ui);
    return true;
}

bool roll_marks_pitch(Pattern& pattern, UiState& ui, int way, int lane, int max_key) {
    if (!way) return false;
    auto moved = take_marked(pattern, ui);
    if (moved.empty()) return false;
    int rows = 0;
    bool done = put_back(pattern, moved, way,
                         [&](Event& e, int by) {
                             if (lane >= 0) {
                                 int pitch = int(e.pitch) + by;
                                 if (pitch < -48 || pitch > 48) return false;
                                 e.pitch = int8_t(pitch);
                             } else {
                                 int key = int(e.key) + by;
                                 if (key < 0 || key > max_key) return false;
                                 e.key = uint8_t(key);
                             }
                             rows = by;
                             return true;
                         },
                         [&](const Event& e) { return roll_row(e, lane) >= 0; });
    if (!done) return false;
    remark(ui, moved);
    ui.roll_key = uint8_t(std::clamp(int(ui.roll_key) + rows, 0, 255));
    roll_follow(pattern, ui);
    return true;
}

unsigned roll_marks_erase(Pattern& pattern, UiState& ui) {
    unsigned gone = unsigned(take_marked(pattern, ui).size());
    roll_unmark(ui);
    return gone;
}

bool roll_marks_mute(Pattern& pattern, const UiState& ui) {
    auto at = roll_marked_events(pattern, ui);
    bool any_played = false;
    for (auto n : at) any_played |= !(pattern.events[n].flags & step_mute);
    for (auto n : at) {
        auto& f = pattern.events[n].flags;
        f = any_played ? uint8_t(f | step_mute) : uint8_t(f & ~step_mute);
    }
    return any_played;
}

unsigned roll_marks_ratchet(Pattern& pattern, const UiState& ui) {
    auto at = roll_marked_events(pattern, ui);
    if (at.empty()) return 0;
    static const uint8_t cycle[] {1, 2, 3, 4, 6, 8};
    unsigned now = pattern.events[at.front()].hits(), next = 1;
    for (auto h : cycle) if (h > now) { next = h; break; }
    for (auto n : at) pattern.events[n].ratchet = uint8_t(next);
    return next;
}

std::string roll_position(uint32_t tick, unsigned bar_ticks, unsigned beat_ticks, unsigned q) {
    bar_ticks = std::max(1u, bar_ticks); beat_ticks = std::max(1u, beat_ticks); q = std::max(1u, q);
    unsigned bar = tick / bar_ticks + 1, beat = tick % bar_ticks / beat_ticks + 1;
    std::string s = std::to_string(bar) + "." + std::to_string(beat);
    if (q < beat_ticks) s += "." + std::to_string(tick % beat_ticks / q + 1); // the step within the beat, as on the MPC
    return s;
}

} // namespace melodee
