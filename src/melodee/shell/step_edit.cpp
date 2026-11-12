// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/step_edit.h"
#include "melodee/model/scales.h"
#include "melodee/shell/context.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/undo_scope.h"
#include <string>
#include <algorithm>

namespace melodee {
namespace {
// The step under the cursor, inside the pattern.
unsigned cursor_step(const Pattern& pattern, const UiState& ui) {
    return unsigned(std::max<int>(ui.step_cursor, 0)) % pattern.steps();
}

// The cursor onto `tick`, selecting the event of `key` there (Steps and Roll follow the same cursor).
void follow(Context& c, uint32_t tick, int key) {
    auto& pattern = c.pattern();
    unsigned step = tick / pattern.q();
    c.ui.step_cursor = int16_t(std::min<unsigned>(step, 0x7fff));
    c.ui.steps_bar = uint8_t(std::min<unsigned>(step / bar_steps, 255));
    auto in = step_events(pattern, step);
    c.ui.step_event = 0;
    for (unsigned n = 0; n < in.count; ++n) {
        const auto& e = pattern.events[in.first + n];
        if (e.tick == tick && e.key == key) { c.ui.step_event = uint8_t(std::min(n, 255u)); break; }
    }
}
} // namespace

std::string key_label(const Track& t, int key) {
    if (t.engine == EngineKind::Sampler && key >= int(sampler_first_note) && key <= int(sampler_last_key)) {
        char label[4];
        pad_label(unsigned(key) - sampler_first_note, label);
        return label;
    }
    if (drum_pads(t) && drum_pad_of(key) >= 0) { // Drums pads: the pad as a kit's (A01 is 36)
        char label[4];
        pad_label(unsigned(drum_pad_of(key)), label);
        return label;
    }
    return note_name(key);
}

bool select_key(Context& c, int key) {
    const auto& pattern = c.pattern();
    auto in = step_events(pattern, cursor_step(pattern, c.ui));
    for (unsigned n = 0; n < in.count; ++n)
        if (pattern.events[in.first + n].key == key) { c.ui.step_event = uint8_t(std::min(n, 255u)); return true; }
    return false;
}

void step_move(Context& c, int way) {
    const auto& pattern = c.pattern();
    int step = std::clamp<int>(int(cursor_step(pattern, c.ui)) + way, 0, int(pattern.steps()) - 1);
    c.ui.step_cursor = int16_t(std::min(step, 0x7fff));
    c.ui.steps_bar = uint8_t(std::min(step / int(bar_steps), 255));
    c.ui.step_event = 0;
}

void event_move(Context& c, int way) {
    const auto& pattern = c.pattern();
    auto in = step_events(pattern, cursor_step(pattern, c.ui));
    if (!in.count) return;
    c.ui.step_event = uint8_t(std::clamp<int>(int(std::min<unsigned>(c.ui.step_event, in.count - 1)) + way, 0, int(in.count) - 1));
}

void nudge_event(Context& c, int ticks) {
    auto& pattern = c.pattern();
    int at = selected_event(pattern, c.ui);
    if (at < 0 || !ticks) return;
    Event e = pattern.events[size_t(at)];
    int64_t tick = std::clamp<int64_t>(int64_t(e.tick) + ticks, 0, int64_t(pattern.length) - 1);
    if (uint32_t(tick) == e.tick) return;
    pattern.events.erase(pattern.events.begin() + at);
    e.tick = uint32_t(tick);
    pattern.add(e); // a note of the same key already there is replaced, as when recording onto it
    c.sync_pattern(c.ui.track, c.track().pattern);
    follow(c, e.tick, e.key);
}

void set_event_velocity(Context& c, int velocity) {
    auto& pattern = c.pattern();
    if (c.ui.page == PageId::Roll) roll_select(pattern, c.ui); // Roll's cards: the note in focus
    int at = selected_event(pattern, c.ui);
    if (at < 0) return;
    pattern.events[size_t(at)].velocity = uint8_t(std::clamp(velocity, 1, 127));
    c.sync_pattern(c.ui.track, c.track().pattern);
}

void set_event_length(Context& c, int steps) {
    auto& pattern = c.pattern();
    if (c.ui.page == PageId::Roll) roll_select(pattern, c.ui);
    int at = selected_event(pattern, c.ui);
    if (at < 0) return;
    pattern.events[size_t(at)].length = uint16_t(std::clamp<int64_t>(int64_t(std::max(steps, 1)) * pattern.q(), 1, 0xffff));
    c.sync_pattern(c.ui.track, c.track().pattern);
}

void set_event_chance(Context& c, int chance) {
    auto& pattern = c.pattern();
    if (c.ui.page == PageId::Roll) roll_select(pattern, c.ui);
    int at = selected_event(pattern, c.ui);
    if (at < 0) return;
    pattern.events[size_t(at)].chance = uint8_t(std::clamp(chance, 0, 100));
    c.sync_pattern(c.ui.track, c.track().pattern);
}

namespace {
// The selected event (on Roll the note in focus) through `edit`, and the pattern to the audio thread.
template <class Edit> void edit_selected(Context& c, Edit&& edit) {
    auto& pattern = c.pattern();
    if (c.ui.page == PageId::Roll) roll_select(pattern, c.ui);
    int at = selected_event(pattern, c.ui);
    if (at < 0) return;
    edit(pattern.events[size_t(at)]);
    c.sync_pattern(c.ui.track, c.track().pattern);
}
} // namespace

void set_event_ratchet(Context& c, int hits) {
    edit_selected(c, [&](Event& e) { e.ratchet = uint8_t(std::clamp<int>(hits, 1, int(max_ratchet))); });
}

void set_event_ramp(Context& c, int ramp) {
    edit_selected(c, [&](Event& e) { e.ramp = int8_t(std::clamp(ramp, -100, 100)); });
}

void set_event_pitch_ramp(Context& c, int semitones) {
    edit_selected(c, [&](Event& e) { e.pitch_ramp = int8_t(std::clamp(semitones, -max_pitch_ramp, max_pitch_ramp)); });
}

void set_event_condition(Context& c, int condition) {
    edit_selected(c, [&](Event& e) { e.condition = uint8_t(std::clamp(condition, 0, int(trig_count) - 1)); });
}

void erase_event(Context& c) {
    auto& pattern = c.pattern();
    int at = selected_event(pattern, c.ui);
    if (at < 0) { c.notice("No event here"); return; }
    std::string what = "Erase " + key_label(c.track(), pattern.events[size_t(at)].key);
    c.edit_pattern(c.ui.track, c.track().pattern, what.c_str(), [&] {
        pattern.events.erase(pattern.events.begin() + at);
        c.sync_pattern(c.ui.track, c.track().pattern);
    });
    event_move(c, 0); // the selection stays inside the step
    c.notice(what);
}

namespace {
unsigned caret_of(const Pattern& p, const UiState& ui) { return unsigned(std::max<int>(ui.step_cursor, 0)) % p.steps(); }
// The caret onto `step`, round the pattern either way.
void caret_to(Context& c, int step) {
    int steps = int(c.pattern().steps());
    step = ((step % steps) + steps) % steps;
    c.ui.step_cursor = int16_t(std::min(step, 0x7fff));
    c.ui.steps_bar = uint8_t(std::min(step / int(bar_steps), 255));
    c.ui.step_event = 0;
}
constexpr uint32_t entry_gesture = 0xE7700000u; // a step's entry, one undo however many pads it writes (| the step)
} // namespace

void entry_pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    auto& ui = c.ui;
    uint16_t bit = uint16_t(1u << (pad % 16));
    c.play_pad(pad, velocity, down); // heard as it goes in
    if (!down) {
        ui.entry_held = uint16_t(ui.entry_held & ~bit);
        if (!ui.entry_held && ui.entry_wrote) { // the last pad of the step let go: on to the next
            ui.entry_wrote = false;
            caret_to(c, int(caret_of(c.pattern(), ui)) + 1);
        }
        return;
    }
    int key = pad_key(c.project, c.track(), pad);
    if (key < 0 || !c.track().used()) return;
    ui.entry_held = uint16_t(ui.entry_held | bit);
    auto& p = c.pattern();
    unsigned q = p.q(), caret = caret_of(p, ui);
    c.touch_scope(pattern_scope(c.project, ui), entry_gesture | caret);
    Event e;
    e.tick = caret * q;
    e.key = uint8_t(key);
    e.velocity = ui.entry_velocity ? ui.entry_velocity : std::max<uint8_t>(velocity, 1);
    e.length = uint16_t(std::min<uint32_t>({uint32_t(ui.entry_length) * q, p.length, 65535u}));
    e.chance = ui.entry_chance;
    p.add(e); // a note of the same key already there is replaced
    c.sync_pattern(ui.track, c.track().pattern);
    ui.entry_wrote = true;
}

void entry_rest(Context& c) { caret_to(c, int(caret_of(c.pattern(), c.ui)) + 1); }

void entry_back(Context& c) {
    auto& p = c.pattern();
    int step = int(caret_of(p, c.ui)) - 1;
    caret_to(c, step);
    unsigned at = caret_of(p, c.ui), q = p.q();
    auto in = step_events(p, at);
    if (!in.count) return;
    std::string what = "Erase step " + std::to_string(at + 1);
    c.edit_pattern(c.ui.track, c.track().pattern, what.c_str(), [&] {
        auto& pattern = c.pattern();
        pattern.events.erase(std::remove_if(pattern.events.begin(), pattern.events.end(),
                                            [&](const Event& e) { return e.tick >= at * q && e.tick < (at + 1) * q; }),
                             pattern.events.end());
        c.sync_pattern(c.ui.track, c.track().pattern);
    });
    c.notice(what);
}

void entry_move(Context& c, int way) { caret_to(c, int(caret_of(c.pattern(), c.ui)) + way); }

void set_event_nudge(Context& c, int offset) {
    auto& pattern = c.pattern();
    if (c.ui.page == PageId::Roll) roll_select(pattern, c.ui); // the note in focus
    int at = selected_event(pattern, c.ui);
    if (at < 0) return;
    const auto& e = pattern.events[size_t(at)];
    int64_t to = int64_t(nudge_base(pattern, e)) + offset;
    nudge_event(c, int(std::clamp<int64_t>(to, 0, int64_t(pattern.length) - 1) - int64_t(e.tick)));
    if (c.ui.page == PageId::Roll) roll_follow(c.pattern(), c.ui); // the view onto it, so it stays in focus
}

void erase_key_events(Context& c, int key) {
    std::string name = key_label(c.track(), key), what = "Erase " + name + " events";
    bool found = false;
    c.edit_pattern(c.ui.track, c.track().pattern, what.c_str(), [&] { // inside a take, part of it
        auto& p = c.pattern();
        auto before = p.events.size();
        p.events.erase(std::remove_if(p.events.begin(), p.events.end(), [&](const Event& e) { return e.key == key; }), p.events.end());
        if (p.events.size() == before) return;
        c.sync_pattern(c.ui.track, c.track().pattern);
        found = true;
    });
    c.ui.step_event = 0;
    c.notice(found ? what : "No events on " + name);
}

} // namespace melodee
