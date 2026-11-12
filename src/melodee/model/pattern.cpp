// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/model/pattern.h"

namespace melodee {
namespace {
bool before(const Event& a, const Event& b) { return a.tick != b.tick ? a.tick < b.tick : a.key != b.key ? a.key < b.key : a.pitch < b.pitch; }
} // namespace

bool Pattern::add(const Event& e) {
    auto at = std::lower_bound(events.begin(), events.end(), e, before);
    if (at != events.end() && at->tick == e.tick && at->key == e.key && at->pitch == e.pitch) { *at = e; return true; }
    if (events.size() >= max_pattern_events) return false;
    events.insert(at, e);
    return true;
}

bool Pattern::remove(uint32_t tick, unsigned key) {
    for (size_t n = lower(tick); n < events.size() && events[n].tick == tick; ++n)
        if (events[n].key == key) { events.erase(events.begin() + long(n)); return true; }
    return false;
}

Step Pattern::step(unsigned s) const {
    Step out;
    uint32_t start = uint32_t(s) * q(), end = std::min<uint32_t>(start + q(), length);
    unsigned count = 0;
    for (size_t n = lower(start); n < events.size() && events[n].tick < end && count < notes_per_step; ++n) {
        const auto& e = events[n];
        out.notes[count++] = {e.key, e.velocity, uint8_t(e.tick - start)};
        if (count == 1) {
            out.length = uint8_t(std::clamp<unsigned>((e.length + q() / 2) / q(), 1, 255));
            out.chance = e.chance; out.flags = e.flags; out.ratchet = uint8_t(e.hits()); out.ramp = e.ramp; out.condition = e.condition;
        }
    }
    return out;
}

void Pattern::set_step(unsigned s, const Step& value) {
    uint32_t start = uint32_t(s) * q();
    Step old = step(s);
    // The events the step showed, kept for their lengths.
    std::array<Event, notes_per_step> was {};
    unsigned count = 0;
    for (const auto& n : old.notes) {
        if (!n.velocity) continue;
        for (size_t k = lower(start + n.offset); k < events.size() && events[k].tick == start + n.offset; ++k)
            if (events[k].key == n.key) { was[count++] = events[k]; break; }
        remove(start + n.offset, n.key);
    }
    for (const auto& n : value.notes) {
        if (!n.velocity) continue;
        Event e;
        e.tick = start + std::min<unsigned>(n.offset, q() - 1);
        e.length = uint16_t(std::min<unsigned>(value.length * q(), 65535));
        if (value.length == old.length)
            for (unsigned k = 0; k < count; ++k) if (was[k].key == n.key) e.length = was[k].length;
        e.key = n.key; e.velocity = n.velocity; e.chance = value.chance; e.flags = value.flags;
        e.ratchet = uint8_t(std::clamp<unsigned>(value.ratchet, 1, max_ratchet)); e.ramp = value.ramp;
        e.condition = uint8_t(value.condition < trig_count ? value.condition : 0);
        bool same_ratchet = value.ratchet == old.ratchet && value.ramp == old.ramp, same_condition = value.condition == old.condition;
        for (unsigned k = 0; k < count; ++k) // the note it was keeps its pitch, and its ratchet while the step's is unchanged
            if (was[k].key == n.key && was[k].velocity) {
                e.pitch = was[k].pitch; e.pitch_ramp = was[k].pitch_ramp;
                if (same_ratchet) { e.ratchet = was[k].ratchet; e.ramp = was[k].ramp; }
                if (same_condition) e.condition = was[k].condition;
                was[k].velocity = 0;
                break;
            }
        add(e);
    }
}

void Pattern::sort() { std::stable_sort(events.begin(), events.end(), before); }

void Pattern::cut(uint32_t tick) {
    events.erase(events.begin() + long(lower(tick)), events.end());
    motion.erase(motion.begin() + long(motion_lower(tick)), motion.end());
}

namespace {
bool motion_before(const MotionPoint& a, const MotionPoint& b) {
    return a.tick != b.tick ? a.tick < b.tick : a.param != b.param ? a.param < b.param : a.pad < b.pad;
}
} // namespace

bool Pattern::write_motion(const MotionPoint& m) {
    auto at = std::lower_bound(motion.begin(), motion.end(), m, motion_before);
    if (at != motion.end() && at->tick == m.tick && at->on(m.param, m.pad)) { *at = m; return true; }
    if (motion.size() >= motion_points) return false;
    motion.insert(at, m);
    return true;
}

bool Pattern::clear_lane(unsigned param, unsigned pad) {
    auto before = motion.size();
    motion.erase(std::remove_if(motion.begin(), motion.end(), [&](const MotionPoint& m) { return m.on(param, pad); }), motion.end());
    return motion.size() != before;
}

unsigned Pattern::lane_points(unsigned param, unsigned pad) const {
    unsigned n = 0;
    for (const auto& m : motion) n += m.on(param, pad);
    return n;
}

const MotionPoint* Pattern::lock_at(uint32_t tick, unsigned param, unsigned pad) const {
    for (size_t n = motion_lower(tick); n < motion.size() && motion[n].tick == tick; ++n)
        if (motion[n].on(param, pad) && motion[n].lock()) return &motion[n];
    return nullptr;
}

int Pattern::lane_value(uint32_t tick, unsigned param, unsigned pad, int own) const {
    for (size_t n = std::min(motion_lower(tick + 1), motion.size()); n-- > 0;)
        if (motion[n].on(param, pad) && !motion[n].lock()) return motion[n].value;
    return own;
}

void Pattern::sort_motion() { std::stable_sort(motion.begin(), motion.end(), motion_before); }

void Pattern::set_lane_length(unsigned key, uint32_t ticks) {
    lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [&](const LaneLength& l) { return l.key == key; }), lanes.end());
    if (!ticks || ticks >= length) return;
    LaneLength l;
    l.key = uint8_t(key); l.length = ticks;
    lanes.insert(std::lower_bound(lanes.begin(), lanes.end(), l, [](const LaneLength& a, const LaneLength& b) { return a.key < b.key; }), l);
}

void Pattern::fit_lanes() {
    lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [&](const LaneLength& l) { return !l.length || l.length >= length; }), lanes.end());
}

bool double_pattern(Pattern& p, uint32_t max_length, bool copy) {
    if (p.length * 2 > max_length) return false;
    p.cut(p.length); // nothing hidden past the end comes back
    size_t count = copy ? p.events.size() : 0;
    for (size_t n = 0; n < count && p.events.size() < max_pattern_events; ++n) { Event e = p.events[n]; e.tick += p.length; p.events.push_back(e); }
    size_t points = copy ? p.motion.size() : 0; // its motion too, as far as there is room
    for (size_t n = 0; n < points && p.motion.size() < motion_points; ++n) { MotionPoint m = p.motion[n]; m.tick += p.length; p.motion.push_back(m); }
    p.length *= 2;
    return true;
}

void half_pattern(Pattern& p) {
    p.length = std::max<uint32_t>(p.length / 2, 1);
    p.cut(p.length);
    p.fit_lanes();
}

void scale_pattern(Pattern& p, bool slower) {
    std::vector<Event> moved;
    moved.reserve(p.events.size());
    for (auto e : p.events) {
        if (e.tick >= p.length) continue;
        e.tick = slower ? e.tick * 2 : e.tick / 2;
        e.length = uint16_t(slower ? std::min<unsigned>(e.length * 2u, 65535) : std::max<unsigned>(e.length / 2u, 1));
        if (e.tick < p.length) moved.push_back(e);
    }
    p.events.clear();
    for (const auto& e : moved) p.add(e);
    std::vector<MotionPoint> points; // its motion with them
    for (auto m : p.motion) {
        m.tick = slower ? m.tick * 2 : m.tick / 2;
        m.hold = uint16_t(slower ? std::min<unsigned>(m.hold * 2u, 0xffff) : (m.hold ? std::max<unsigned>(m.hold / 2u, 1) : 0));
        if (m.tick < p.length) points.push_back(m);
    }
    p.motion.clear();
    for (const auto& m : points) p.write_motion(m);
    for (auto& l : p.lanes) l.length = slower ? l.length * 2 : std::max<uint32_t>(l.length / 2, 1); // its lanes with it
    p.fit_lanes();
}

uint32_t quantise_tick(const Pattern& p, uint32_t tick) { return quantise_tick(p, tick, p.q()); }

uint32_t quantise_tick(const Pattern& p, uint32_t tick, unsigned grid) {
    grid = std::max(grid, 1u);
    uint32_t snapped = (tick + grid / 2) / grid * grid;
    return snapped >= p.length ? 0 : snapped; // past the last step: the first, next time round
}

void time_correct_all(Pattern& p, const std::array<uint64_t, 3>* keys, unsigned swing, int shift) {
    time_correct(p, [keys](unsigned key) { return !keys || (key < 192 && (((*keys)[key / 64] >> (key % 64)) & 1)); }, swing, shift);
}

StepEvents step_events(const Pattern& pattern, unsigned step) {
    uint32_t from = step * pattern.q(), to = std::min<uint32_t>(from + pattern.q(), pattern.length);
    StepEvents out;
    out.first = pattern.lower(from);
    size_t end = out.first;
    while (end < pattern.events.size() && pattern.events[end].tick < to) ++end;
    out.count = unsigned(end - out.first);
    return out;
}

} // namespace melodee
