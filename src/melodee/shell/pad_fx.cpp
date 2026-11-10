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
            ui.punch_slot[s] = -1;
        }
    for (unsigned n = first; n < ui.engaged_count; ++n) {
        const auto& e = ui.engaged[n];
        if (ui.punch_sounding(e.kind)) continue;
        for (unsigned s = 0; s < 4; ++s) {
            if (ui.punch_slot[s] >= 0) continue;
            Command on {Type::PunchOn, punch_target_now(), uint8_t(project.punch_snap | project.punch_tail << 4), uint16_t(e.kind), int16_t(e.depth)};
            on.position = s + 1;
            send(on);
            ui.punch_slot[s] = e.kind;
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
    if (ui.pad_punch_held >= 0) { send({Type::PunchOff, ui.track, 0, uint16_t((ui.pad_punch_held + 1) << 8)}); ui.pad_punch_held = -1; }
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

bool Context::punch_pressure(unsigned pad, uint8_t value) {
    if (ui.page != PageId::Punch) return false;
    for (unsigned n = 0; n < ui.engaged_count; ++n) {
        auto& e = ui.engaged[n];
        if (e.pad != int(pad)) continue;
        e.depth = uint8_t(value * project.punch_depth / 127); // pressure up to K1's depth; latched, it keeps the last
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
