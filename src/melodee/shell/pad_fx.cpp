// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Pad FX (parity phase 7b), as on the MPC: hold pads to punch, up to four at
// once; a fifth bypasses the one engaged first until a newer one lets go.
// B1 (or Push) latches the punch last pressed at its depth; pressed again on
// it, B1 lets go. Pressure is each punch's depth. The MPC's sixteen carry
// their own K1-K3. Flex Beat (Shift + Pad FX): one time effect over the
// master for a bar, from pad 2-16; pad 1 Empty.
#include "melodee/engine/engine_host.h"
#include "melodee/engine/pad_fx_info.h"
#include "melodee/shell/context.h"
#include <algorithm>

namespace melodee {
using Type = Command::Type;

uint8_t Context::punch_target_now() const { return uint8_t(project.punch_target ? ui.track : 255); }

// The newest four engaged punches sound, each keeping its slot while it stays among them.
void Context::sync_live() {
    unsigned first = ui.engaged_count > 4 ? ui.engaged_count - 4 : 0;
    auto active = [&](int kind) { for (unsigned n = first; n < ui.engaged_count; ++n) if (ui.engaged[n].kind == kind) return true; return false; };
    for (unsigned s = 0; s < 4; ++s)
        if (ui.punch_slot[s] >= 0 && !active(ui.punch_slot[s])) {
            Command off {Type::PunchOff, punch_target_now()};
            off.position = s + 1;
            send(off);
            close_span(ui.punch_slot[s]);
            ui.punch_slot[s] = -1;
        }
    for (unsigned n = first; n < ui.engaged_count; ++n) {
        const auto& e = ui.engaged[n];
        if (ui.punch_sounding(e.kind)) continue;
        for (unsigned s = 0; s < 4; ++s) {
            if (ui.punch_slot[s] >= 0) continue;
            uint8_t timing = uint8_t(project.punch_snap | project.punch_tail << 4 | (project.punch_quantize ? 0 : 64));
            Command on {Type::PunchOn, punch_target_now(), timing, uint16_t(e.kind), int16_t(e.depth)};
            on.position = s + 1;
            send(on);
            ui.punch_slot[s] = e.kind;
            open_span(e.kind, e.depth);
            break;
        }
    }
}

void Context::punch_pad(unsigned pad, bool down) {
    int kind = punch_on_pad(project.punch_bank, pad);
    if (kind < 0) return;
    int at = ui.engaged_at(kind);
    if (down) {
        ui.punch_selected = int8_t(kind);
        if (at >= 0) { ui.engaged[size_t(at)].pad = int8_t(pad); return; } // latched already: pressed again, it is selected
        if (ui.engaged_count == UiState::engaged_capacity) return;
        ui.engaged[ui.engaged_count++] = {int8_t(kind), int8_t(pad), false, project.punch_depth};
        sync_live();
        begin_punch(unsigned(kind), punch_target_now(), project.punch_depth); // Seq Record into the song: the newest punch
        return;
    }
    if (at < 0) return;
    auto& e = ui.engaged[size_t(at)];
    e.pad = -1;
    if (e.latched) return;
    for (unsigned n = unsigned(at) + 1; n < ui.engaged_count; ++n) ui.engaged[n - 1] = ui.engaged[n];
    --ui.engaged_count;
    sync_live();
    end_punch();
}

void Context::latch_punch() {
    int at = ui.punch_selected >= 0 ? ui.engaged_at(ui.punch_selected) : -1;
    if (at < 0) { notice("Hold a punch, then Latch"); return; }
    auto& e = ui.engaged[size_t(at)];
    e.latched = !e.latched;
    notice(e.latched ? std::string("Latched: ") + punch_title(PunchKind(e.kind)) : std::string("Latch off"));
    if (!e.latched && e.pad < 0) { // not held either: it lets go
        for (unsigned n = unsigned(at) + 1; n < ui.engaged_count; ++n) ui.engaged[n - 1] = ui.engaged[n];
        --ui.engaged_count;
        sync_live();
        end_punch();
    }
}

void Context::punch_release(bool latched_too) {
    unsigned kept = 0;
    for (unsigned n = 0; n < ui.engaged_count; ++n) {
        auto e = ui.engaged[n];
        e.pad = -1;
        if (e.latched && !latched_too) ui.engaged[kept++] = e; // latched punches play on across pages
    }
    bool changed = kept != ui.engaged_count;
    ui.engaged_count = uint8_t(kept);
    if (changed) { sync_live(); end_punch(); }
}

void Context::set_punch_target(unsigned target) {
    if (project.punch_target == target) return;
    punch_release(true); // a new signal: everything lets go
    project.punch_target = uint8_t(target);
    notice(target ? "Pad FX: the track" : "Pad FX: the master");
}

void Context::step_punch_bank(int way) {
    punch_release();
    project.punch_bank = uint8_t((int(project.punch_bank) + int(punch_banks) + way) % int(punch_banks));
    static const char* const names[] {"Melodee 1", "Melodee 2", "MPC"};
    notice(std::string("Pad FX bank: ") + names[project.punch_bank % punch_banks]);
}

// Shift + B3: quantize on (while playing, a punch waits for its snap grid's next line, K2 SNAP's) or off (at once).
// Only when punches start: the grid still sets a repeat's slice and a gate's steps, and letting go is at once either way.
void Context::toggle_punch_quantize() {
    project.punch_quantize = !project.punch_quantize;
    dirty = true;
    notice(project.punch_quantize ? "Pad FX quantize on" : "Pad FX quantize off");
}

bool Context::punch_pressure(unsigned pad, uint8_t value) {
    if (ui.page != PageId::Punch) return false;
    for (unsigned n = 0; n < ui.engaged_count; ++n) {
        auto& e = ui.engaged[n];
        if (e.pad != int(pad)) continue;
        e.depth = uint8_t(value * project.punch_depth / 127); // pressure up to K1's depth; latched, it keeps the last
        for (unsigned n = ui.span_count; n-- > 0;) { // the span heard keeps the deepest (pad-fx C)
            auto& span = ui.spans[n];
            if (span.kind != uint8_t(e.kind) || span.until) continue;
            span.depth = span.felt ? std::max(span.depth, e.depth) : e.depth;
            span.felt = true;
            break;
        }
        for (unsigned s = 0; s < 4; ++s)
            if (ui.punch_slot[s] == e.kind) {
                Command depth {Type::PunchDepth, punch_target_now(), 0, 0, int16_t(e.depth)};
                depth.position = s + 1;
                send(depth);
            }
        return true;
    }
    return false;
}

void Context::set_pad_fx_param(unsigned mpc, unsigned n, int value) {
    if (mpc >= 16 || n >= punch_params) return;
    const auto& pi = mpc_pad_fx(mpc).params[n];
    if (!pi.label) return;
    project.pad_fx_params[mpc][n] = int16_t(std::clamp<int>(value, pi.min, pi.max));
    send({Type::PunchParam, 0, uint8_t(n), uint16_t(mpc), project.pad_fx_params[mpc][n]});
    dirty = true;
}

// pad-fx C (round 2): a punch heard from the next line of its snap grid (where the engine starts it), while playing;
// quantize off, from now. The first in a later pass of the selected track's pattern begins a new take: the last
// one's punches go, and one still sounding carries on into it from the loop's start.
void Context::open_span(int kind, uint8_t depth) {
    if (!host.playing() || kind < 0) return;
    unsigned snap = project.punch_snap & 3;
    uint32_t grid = project.punch_quantize ? 16 * sixteenth_ticks / (4u << snap) : 1, now = host.ticks();
    uint64_t from = uint64_t((now + grid - 1) / grid) * grid;
    int position = host.position_of(ui.track % track_count);
    if (position >= 0) {
        Speed sp = speed_of(track().speed);
        uint64_t loop = std::max<uint64_t>(uint64_t(pattern().length) * sp.den / sp.num, 1), at = uint64_t(position) * sp.den / sp.num;
        uint64_t pass = now >= at ? now - at : 0; // where this pass began, in the sequencer's ticks
        while (from >= pass + loop) pass += loop;  // the grid line may fall in the next
        if (!ui.span_count || from >= ui.take_until) {
            unsigned left = 0;
            for (unsigned n = 0; n < ui.span_count; ++n)
                if (!ui.spans[n].until) { auto span = ui.spans[n]; span.from = std::max(span.from, pass); ui.spans[left++] = span; }
            ui.span_count = uint8_t(left);
            ui.take_until = pass + loop;
        }
    }
    if (ui.span_count == UiState::span_capacity) { // full: the oldest goes
        std::move(ui.spans.begin() + 1, ui.spans.end(), ui.spans.begin());
        --ui.span_count;
    }
    ui.spans[ui.span_count++] = {from, 0, uint8_t(kind), punch_target_now(), depth, uint8_t(snap), uint8_t(project.punch_tail % 3), false};
}

void Context::close_span(int kind) {
    for (unsigned n = ui.span_count; n-- > 0;) {
        auto& span = ui.spans[n];
        if (span.kind != uint8_t(kind) || span.until) continue;
        span.until = std::max<uint64_t>(host.ticks(), span.from); // let go before its grid line: nothing heard
        return;
    }
}

// B3 KEEP: the take (the last punches heard, up to a pass of the selected track's pattern) kept in that pattern
// where it fell (as its speed plays it), beside what it kept before; the spans kept go, one still sounding carries
// on from now.
void Context::keep_punches() {
    unsigned t = ui.track % track_count, n = track().pattern % pattern_count;
    int position = host.position_of(t);
    if (!host.playing()) { notice("Play, punch, then Keep"); return; }
    if (position < 0) { notice("Track not playing"); return; } // an empty track, or no clip of it in the song
    auto& pattern = patterns.at[t][n];
    Speed sp = speed_of(track().speed);
    auto written = [&](uint64_t ticks) { return ticks * sp.num / sp.den; }; // the sequencer's ticks as the pattern's
    std::array<PunchSpan, UiState::span_capacity> spans;
    for (unsigned k = 0; k < ui.span_count; ++k) {
        spans[k] = ui.spans[k];
        spans[k].from = written(spans[k].from);
        if (spans[k].until) spans[k].until = written(spans[k].until);
    }
    auto heard = heard_punches(spans.data(), ui.span_count, written(host.ticks()), uint32_t(position), pattern.length);
    unsigned added = 0;
    for (const auto& k : heard) added += std::find(pattern.punches.begin(), pattern.punches.end(), k) == pattern.punches.end();
    if (!added) { notice(heard.empty() ? "Punch, then Keep" : "Kept already"); return; }
    if (pattern.punches.size() >= max_pattern_punches) { notice(std::to_string(max_pattern_punches) + " punches at most"); return; }
    edit_pattern(t, n, "Keep punches", [&] {
        for (const auto& k : heard)
            if (pattern.punches.size() < max_pattern_punches && std::find(pattern.punches.begin(), pattern.punches.end(), k) == pattern.punches.end())
                pattern.punches.push_back(k);
        std::stable_sort(pattern.punches.begin(), pattern.punches.end(), [](const PatternPunch& a, const PatternPunch& b) { return a.tick < b.tick; });
        sync_pattern(t, n);
    });
    unsigned left = 0;
    uint64_t now = host.ticks();
    for (unsigned k = 0; k < ui.span_count; ++k)
        if (!ui.spans[k].until) { auto span = ui.spans[k]; span.from = std::max(span.from, now); ui.spans[left++] = span; }
    ui.span_count = uint8_t(left);
    notice(added == 1 ? std::string("Punch kept") : std::to_string(added) + " punches kept");
}

// Erase + pad on Pad FX (pad-fx C): the pad's punch taken out of the selected track's pattern wherever it is kept.
void Context::erase_kept_punch(unsigned pad) {
    int kind = punch_on_pad(project.punch_bank, pad);
    if (kind < 0) return;
    unsigned t = ui.track % track_count, n = track().pattern % pattern_count;
    auto& punches = patterns.at[t][n].punches;
    auto is = [&](const PatternPunch& e) { return e.kind == uint8_t(kind); };
    std::string name = punch_title(PunchKind(kind));
    if (std::none_of(punches.begin(), punches.end(), is)) { notice(name + " not kept"); return; }
    edit_pattern(t, n, "Erase punches", [&] {
        punches.erase(std::remove_if(punches.begin(), punches.end(), is), punches.end());
        sync_pattern(t, n);
    });
    notice(name + " erased");
}

void Context::erase_punches() {
    unsigned t = ui.track % track_count, n = track().pattern % pattern_count;
    if (patterns.at[t][n].punches.empty()) return;
    edit_pattern(t, n, "Erase punches", [&] { patterns.at[t][n].punches.clear(); sync_pattern(t, n); });
    notice("Punches erased");
}

// Flex Beat.
void Context::flex_pad(unsigned pad) {
    send({Type::FlexBeat, 0, 0, 0, int16_t(pad % flex_pads)});
    ui.flex_chosen = uint8_t(pad % flex_pads);
    if (pad && !host.playing()) notice("Flex Beat: press Play first");
}

void Context::set_flex(unsigned what, int value) {
    auto& f = project.flex;
    switch (what) {
    case 1: f.mode = uint8_t(value != 0); value = f.mode; break;
    case 2: f.mix = uint8_t(std::clamp(value, 0, 100)); value = f.mix; break;
    case 3: f.quantize = value != 0; value = f.quantize; break;
    default: return;
    }
    send({Type::FlexBeat, 0, 0, uint16_t(what), int16_t(value)});
    dirty = true;
}

} // namespace melodee
