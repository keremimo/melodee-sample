// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The printed buttons on the note editors (shell/note_jobs.h).
#include "melodee/shell/note_jobs.h"
#include "melodee/engine/command.h"
#include "melodee/model/note_edits.h"
#include "melodee/shell/context.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/step_lane.h"
#include "melodee/shell/undo_scope.h"
#include <algorithm>
#include <string>

namespace melodee {
namespace {
// What a job acts on when no notes are marked: the note in hand, the notes starting in the cursor's step, or the
// notes in view.
enum class Scope { Note, Step, View };

bool marked(Context& c) { return c.ui.page == PageId::Roll && roll_selected(c.ui, c.track().pattern); }
unsigned cursor_of(const Pattern& p, const UiState& ui) { return unsigned(std::max<int>(ui.step_cursor, 0)) % p.steps(); }

// The ticks in view: Roll's columns, Steps' page of steps, Step Edit's bar.
void view_of(Context& c, uint32_t& from, uint32_t& to) {
    const auto& p = c.pattern();
    const auto& ui = c.ui;
    unsigned q = p.q();
    if (ui.page == PageId::Roll) { from = roll_first(p, ui) * q; to = from + roll_columns * q; }
    else { unsigned bar = std::max(1u, c.project.bar_ticks()); from = cursor_of(p, ui) * q / bar * bar; to = from + bar; }
    to = std::min(to, p.length);
}

std::vector<size_t> targets(Context& c, Scope scope) {
    const auto& p = c.pattern();
    const auto& ui = c.ui;
    const auto& t = c.track();
    if (marked(c)) return roll_marked_events(p, ui);
    std::vector<size_t> out;
    const int lane = ui.page == PageId::Roll ? roll_lane(t) : -1;
    auto fits = [&](const Event& e) {
        if (ui.page == PageId::Roll) return roll_row(e, lane) >= 0;
        if (ui.page == PageId::StepEdit && ui.step_pads) { int key = steps_lane(t); return key < 0 || e.key == key; }
        return true;
    };
    if (scope == Scope::Note && ui.page == PageId::Roll) {
        int at = roll_focus(p, ui, lane);
        if (at >= 0) out.push_back(size_t(at));
        return out;
    }
    if (scope == Scope::Note && ui.page == PageId::StepEdit) {
        int at = selected_event(p, ui);
        if (at >= 0) out.push_back(size_t(at));
        return out;
    }
    uint32_t from = cursor_of(p, ui) * p.q(), to = from + p.q();
    if (scope == Scope::View) view_of(c, from, to);
    for (size_t n = p.lower(from); n < p.events.size() && p.events[n].tick < std::min(to, p.length); ++n)
        if (fits(p.events[n])) out.push_back(n);
    return out;
}

// The chosen notes out, through `change`, back in: one undo. Roll's marks follow them; false when nothing changed.
template <class Change> bool apply(Context& c, const char* what, const std::vector<size_t>& at, Change&& change) {
    if (at.empty()) { c.notice(c.ui.page == PageId::Roll ? "No note here" : "No notes here"); return false; }
    const bool marks = marked(c);
    std::vector<Event> result;
    bool done = false;
    c.edit_pattern(c.ui.track, c.track().pattern, what, [&] {
        auto& p = c.pattern();
        auto notes = take_events(p, at);
        done = change(notes, p);
        put_events(p, notes);
        result = notes;
        c.sync_pattern(c.ui.track, c.track().pattern);
    });
    if (marks) roll_remark(c.ui, result);
    if (c.ui.page == PageId::Roll) roll_select(c.pattern(), c.ui, roll_lane(c.track()));
    return done;
}

std::string count_of(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }
} // namespace

bool note_editor_open(Context& c) {
    auto page = c.ui.page;
    return (page == PageId::Roll || page == PageId::StepEdit) && c.track().used();
}

bool note_chop(Context& c) {
    if (!note_editor_open(c)) return false;
    unsigned split = 0;
    const unsigned q = c.pattern().q();
    apply(c, "Split notes", targets(c, Scope::Note), [&](std::vector<Event>& notes, Pattern&) {
        split = split_events(notes, q);
        return split > 0;
    });
    c.notice(split ? "Split " + count_of(split, "note", "notes") : std::string("Notes at one step"));
    return true;
}

bool note_legato(Context& c) {
    if (!note_editor_open(c)) return false;
    c.ui.shift_used = true;
    const auto& t = c.track();
    const bool per_key = t.engine == EngineKind::Sampler && !t.levels; // a kit: each pad to its own next hit
    if (apply(c, "Legato", targets(c, Scope::Note), [&](std::vector<Event>& notes, Pattern& rest) {
            legato_events(notes, rest, per_key);
            return true;
        }))
        c.notice("Legato");
    return true;
}

bool note_reverse(Context& c) {
    if (!note_editor_open(c)) return false;
    auto at = targets(c, Scope::View);
    if (at.size() < 2) { c.notice("Reverse: two notes or more"); return true; }
    apply(c, "Reverse notes", at, [&](std::vector<Event>& notes, Pattern&) { reverse_events(notes); return true; });
    c.notice("Reverse " + count_of(at.size(), "note", "notes"));
    return true;
}

bool note_arp(Context& c) {
    if (!note_editor_open(c)) return false;
    c.ui.shift_used = true;
    const auto& t = c.track();
    const int lane = c.ui.page == PageId::Roll ? roll_lane(t) : -1;
    if (lane < 0 && (t.engine == EngineKind::Sampler || drum_pads(t))) { c.notice("Arp to notes: keys, not pads"); return true; }
    const uint32_t length = c.pattern().length;
    size_t made = 0;
    apply(c, "Arp to notes", targets(c, Scope::Step), [&](std::vector<Event>& notes, Pattern&) {
        notes = arp_events(notes, t.arp_mode, t.arp_rate, t.arp_octaves, t.arp_gate, length, lane, 127);
        made = notes.size();
        return true;
    });
    if (made) c.notice("Arp: " + count_of(made, "note", "notes"));
    return true;
}

bool edit_loop_tap(Context& c) {
    if (!note_editor_open(c)) return false;
    auto& ui = c.ui;
    if (ui.edit_loop) { edit_loop_off(c); c.notice("Loop off"); return true; }
    const auto& p = c.pattern();
    uint32_t from = 0, to = 0;
    if (marked(c)) { // the marked notes' bars
        from = UINT32_MAX;
        for (auto n : roll_marked_events(p, ui)) { from = std::min(from, p.events[n].tick); to = std::max<uint32_t>(to, p.events[n].tick + 1); }
    } else view_of(c, from, to);
    const uint32_t bar = std::max(1u, c.project.bar_ticks());
    from = from / bar * bar;
    to = std::min<uint32_t>((to + bar - 1) / bar * bar, p.length);
    if (to <= from) to = std::min<uint32_t>(from + bar, p.length);
    if (to <= from) return true;
    ui.edit_loop = true;
    ui.edit_loop_track = ui.track; ui.edit_loop_pattern = c.track().pattern;
    ui.edit_loop_from = from; ui.edit_loop_ticks = to - from;
    Command cmd {Command::Type::EditLoop, ui.track, 0, uint16_t(ui.edit_loop_ticks & 0xffff), int16_t(uint16_t(ui.edit_loop_ticks >> 16))};
    cmd.position = from;
    c.send(cmd);
    unsigned first = from / bar + 1, last = (to - 1) / bar + 1;
    c.notice(first == last ? "Loop bar " + std::to_string(first) : "Loop bars " + std::to_string(first) + "-" + std::to_string(last));
    return true;
}

void edit_loop_off(Context& c) {
    auto& ui = c.ui;
    if (!ui.edit_loop) return;
    ui.edit_loop = false;
    Command cmd {Command::Type::EditLoop, ui.edit_loop_track, 0, 0, 0};
    cmd.position = 0;
    c.send(cmd);
}

void edit_loop_check(Context& c) {
    const auto& ui = c.ui;
    if (!ui.edit_loop) return;
    bool editor = ui.page == PageId::Roll || ui.page == PageId::StepEdit;
    if (!editor || ui.track != ui.edit_loop_track || c.project.tracks[ui.edit_loop_track % track_count].pattern != ui.edit_loop_pattern)
        edit_loop_off(c);
}

bool roll_paste(Context& c) {
    auto& ui = c.ui;
    if (ui.copying != UiState::Copying::Notes || ui.page != PageId::Roll) return false;
    auto& p = c.pattern();
    auto at = roll_marked_events(p, ui);
    if (at.empty()) { ui.stop_copying(); c.notice("Copy off: no notes marked"); return true; }
    const uint32_t q = p.q(), first = p.events[at.front()].tick, to = cursor_of(p, ui) * q + first % q;
    std::vector<Event> copies;
    for (auto n : at) {
        Event e = p.events[n];
        int64_t tick = int64_t(e.tick) + int64_t(to) - int64_t(first);
        if (tick < 0 || tick >= int64_t(p.length)) { c.notice("No room to paste here"); return true; }
        e.tick = uint32_t(tick);
        copies.push_back(e);
    }
    c.edit_pattern(ui.track, c.track().pattern, "Paste notes", [&] {
        put_events(p, copies);
        c.sync_pattern(ui.track, c.track().pattern);
    });
    roll_remark(ui, copies); // the copies are marked: Push again pastes them on
    roll_select(p, ui, roll_lane(c.track()));
    c.notice("Paste " + count_of(copies.size(), "note", "notes"));
    return true;
}

void roll_marks_ramp(Context& c, int percent) {
    if (!marked(c)) return;
    auto& p = c.pattern();
    auto at = roll_marked_events(p, c.ui);
    if (at.size() < 2) return;
    std::vector<Event> notes;
    for (auto n : at) notes.push_back(p.events[n]);
    ramp_events(notes, percent);
    for (size_t n = 0; n < at.size(); ++n) p.events[at[n]].velocity = notes[n].velocity;
    c.sync_pattern(c.ui.track, c.track().pattern);
}

bool roll_marks_quantize(Context& c, int way) {
    if (!marked(c) || !way) return false;
    auto& ui = c.ui;
    c.touch_scope(pattern_scope(c.project, ui), 0x5E1EC7A0u);
    const unsigned q = c.pattern().q();
    const uint32_t length = c.pattern().length;
    auto& p = c.pattern();
    auto notes = roll_take_marked(p, ui);
    for (int n = std::abs(way); n > 0; --n) quantize_events(notes, q, length, 25);
    put_events(p, notes);
    roll_remark(ui, notes);
    c.sync_pattern(ui.track, c.track().pattern);
    roll_select(p, ui, roll_lane(c.track()));
    c.notice("Quantize");
    return true;
}

bool roll_marks_octave(Context& c, int way) {
    if (!marked(c) || !way) return false;
    auto& ui = c.ui;
    c.touch_scope(pattern_scope(c.project, ui), 0x5E1EC7B0u);
    const int max_key = c.track().engine == EngineKind::Sampler ? int(sampler_last_key) : 127;
    if (!roll_marks_pitch(c.pattern(), ui, way > 0 ? 12 : -12, roll_lane(c.track()), max_key, 12)) { c.notice("No room for an octave"); return true; }
    c.sync_pattern(ui.track, c.track().pattern);
    roll_select(c.pattern(), ui, roll_lane(c.track()));
    return true;
}

} // namespace melodee
