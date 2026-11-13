// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Edits of chosen notes (model/note_edits.h).
#include "melodee/model/note_edits.h"
#include "melodee/engine/arp.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace melodee {

std::vector<Event> take_events(Pattern& p, const std::vector<size_t>& at) {
    std::vector<Event> out;
    std::vector<size_t> sorted = at;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    for (auto n : sorted) if (n < p.events.size()) out.push_back(p.events[n]);
    for (auto n = sorted.rbegin(); n != sorted.rend(); ++n) if (*n < p.events.size()) p.events.erase(p.events.begin() + long(*n));
    return out;
}

void put_events(Pattern& p, const std::vector<Event>& events) {
    for (const auto& e : events) if (e.tick < p.length) p.add(e);
}

unsigned split_events(std::vector<Event>& notes, unsigned q) {
    q = std::max(1u, q);
    std::vector<Event> out;
    unsigned split = 0;
    for (const auto& e : notes) {
        if (e.length < 2 * q) { out.push_back(e); continue; }
        uint32_t first = std::max<uint32_t>(q, (e.length / 2 + q / 2) / q * q);
        if (first >= e.length) first = e.length - q;
        Event a = e, b = e;
        a.length = uint16_t(first);
        b.tick = e.tick + first;
        b.length = uint16_t(e.length - first);
        out.push_back(a); out.push_back(b);
        ++split;
    }
    notes = out;
    return split;
}

void legato_events(std::vector<Event>& notes, const Pattern& rest, bool per_key) {
    for (auto& e : notes) {
        uint32_t next = rest.length;
        auto later = [&](const Event& o) {
            if (o.tick > e.tick && o.tick < next && (!per_key || o.key == e.key)) next = o.tick;
        };
        for (const auto& o : rest.events) later(o);
        for (const auto& o : notes) later(o);
        e.length = uint16_t(std::clamp<uint32_t>(next - e.tick, 1, 0xffff));
    }
}

void reverse_events(std::vector<Event>& notes) {
    if (notes.size() < 2) return;
    uint32_t from = UINT32_MAX, to = 0;
    for (const auto& e : notes) { from = std::min(from, e.tick); to = std::max<uint32_t>(to, e.tick + e.length); }
    for (auto& e : notes) e.tick = from + (to - (e.tick + e.length));
}

void quantize_events(std::vector<Event>& notes, unsigned q, uint32_t length, unsigned percent) {
    q = std::max(1u, q);
    for (auto& e : notes) {
        int64_t grid = int64_t((e.tick + q / 2) / q) * q, off = grid - int64_t(e.tick);
        if (!off) continue;
        int64_t move = off * int64_t(percent) / 100;
        if (!move) move = off > 0 ? 1 : -1;
        int64_t tick = int64_t(e.tick) + move;
        if (tick < 0 || tick >= int64_t(length)) continue;
        e.tick = uint32_t(tick);
    }
}

void ramp_events(std::vector<Event>& notes, int percent) {
    if (notes.size() < 2) return;
    percent = std::clamp(percent, -100, 100);
    double first = notes.front().velocity, last = double(notes.size() - 1);
    for (size_t n = 1; n < notes.size(); ++n)
        notes[n].velocity = uint8_t(std::clamp(int(std::lround(first * (1.0 + percent / 100.0 * double(n) / last))), 1, 127));
}

int ramp_of(const std::vector<Event>& notes) {
    if (notes.size() < 2 || !notes.front().velocity) return 0;
    return std::clamp(int(std::lround((double(notes.back().velocity) / notes.front().velocity - 1.0) * 100.0)), -100, 100);
}

std::vector<Event> arp_events(const std::vector<Event>& notes, unsigned mode, unsigned rate, unsigned octaves, unsigned gate,
                              uint32_t length, int lane, int top) {
    std::vector<Event> out;
    const uint32_t each = arp_rate_ticks(rate);
    auto row = [&](const Event& e) { return lane >= 0 ? 60 + int(e.pitch) : int(e.key); };
    for (size_t first = 0; first < notes.size();) {
        size_t last = first;
        while (last < notes.size() && notes[last].tick == notes[first].tick) ++last;
        Arp arp;
        arp.mode = ArpMode(mode % unsigned(ArpMode::Count));
        arp.octaves = std::max(1u, octaves);
        arp.top = lane >= 0 ? 60 + 48 : top;
        arp.pressure_velocity = false;
        uint32_t start = notes[first].tick, end = start;
        for (size_t n = first; n < last; ++n) {
            arp.press(row(notes[n]), notes[n].velocity);
            end = std::max<uint32_t>(end, notes[n].tick + notes[n].length);
        }
        end = std::min(end, length);
        const Event& like = notes[first];
        for (uint32_t t = start; t < end; t += each) {
            int velocity = 100, key = arp.next(velocity);
            if (key < 0) break;
            Event e = like;
            e.tick = t;
            e.length = uint16_t(std::clamp<uint32_t>(std::min<uint32_t>(each * std::clamp(gate, 1u, 100u) / 100, end - t), 1, 0xffff));
            e.velocity = uint8_t(std::clamp(velocity, 1, 127));
            e.ratchet = 1; e.ramp = 0; e.pitch_ramp = 0;
            if (lane >= 0) { e.key = uint8_t(lane); e.pitch = int8_t(std::clamp(key - 60, -48, 48)); }
            else e.key = uint8_t(std::clamp(key, 0, 255));
            for (size_t n = first; n < last; ++n) if (row(notes[n]) == key) e.cutoff = notes[n].cutoff; // a level's cutoff kept
            out.push_back(e);
        }
        first = last;
    }
    return out;
}

} // namespace melodee
