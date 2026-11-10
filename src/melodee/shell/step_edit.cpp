// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/step_edit.h"
#include "melodee/model/scales.h"
#include "melodee/shell/context.h"
#include "melodee/shell/roll.h"
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

int pad_key(const Project& project, const Track& t, unsigned pad) {
    if (t.engine == EngineKind::Sampler) return int(sampler_first_note + t.bank % sampler_banks * 16 + pad % 16);
    if (t.engine == EngineKind::Kit909) return pad < 11 ? int(36 + pad) : -1;
    return t.used() ? pad_note(project, t, pad).key : -1;
}

std::string key_label(const Track& t, int key) {
    if (t.engine == EngineKind::Sampler && key >= int(sampler_first_note) && key <= int(sampler_last_key)) {
        char label[4];
        pad_label(unsigned(key) - sampler_first_note, label);
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
