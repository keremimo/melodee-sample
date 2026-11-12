// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/mod_jobs.h"
#include "melodee/engine/command.h"
#include "melodee/model/lane_value.h"
#include <algorithm>

namespace melodee {
namespace {
using Kind = Binding::Kind;
using Type = Command::Type;

// The lane a card turns, on track `t` (the track in hand): -1 for a card that names none.
int lane_of(const Project& project, unsigned t, const Binding& b) {
    const auto& track = project.tracks[t % track_count];
    if (int lane = binding_lane(track, b); lane >= 0) return lane;
    switch (b.kind) {
    case Kind::Level: return int(motion_level);
    case Kind::Pan: return int(motion_pan);
    case Kind::Send: return int(motion_send(unsigned(b.id)));
    case Kind::BusReturn: return int(motion_return(unsigned(b.id)));
    case Kind::FxParam: {
        unsigned chain = unsigned(b.id) / 64, slot = (unsigned(b.id) / 8) % 8, q = unsigned(b.id) % 8;
        if (chain == t) return int(motion_fx_param(slot, q));
        if (chain == master_chain) return int(motion_master_fx(slot, q));
        return -1;
    }
    default: return -1;
    }
}
} // namespace

void learn_lane(Context& c, const Binding& b) {
    int lane = lane_of(c.project, c.ui.track, b);
    if (lane < 0) return;
    c.ui.learn_lane = int16_t(lane);
    c.ui.learn_track = uint8_t(c.ui.track % track_count);
}

void send_modulator(Context& c, unsigned n) {
    n %= modulator_count;
    auto m = c.project.modulators[n];
    for (unsigned i = 0; i < modulator_targets; ++i) {
        auto& g = m.targets[i];
        if (g.used()) g.own = int16_t(lane_own(c.project, g.track, g.param, g.pad));
        c.send(pack_mod_target(n, i, g));
    }
    c.send(pack_modulator(n, m));
}

void mod_add(Context& c) {
    auto& ui = c.ui;
    if (ui.learn_lane < 0) { c.notice("Turn a knob first"); return; }
    unsigned t = ui.learn_track % track_count, param = lane_param(ui.learn_lane), pad = lane_pad(ui.learn_lane) % sampler_pads;
    auto info = motion_param(c.project.tracks[t], ui.learn_lane, &c.project);
    if (!info.info) { c.notice("Nothing to move there"); return; }
    if (ui.mod_view == 0) {
        auto& m = c.project.modulators[ui.mod_index % modulator_count];
        for (const auto& g : m.targets) if (g.used() && g.track == t && g.param == param && g.pad == pad) { c.notice("Already on it"); return; }
        unsigned i = 0;
        while (i < modulator_targets && m.targets[i].used()) ++i;
        if (i == modulator_targets) { c.notice("Four targets at most"); return; }
        m.targets[i] = ModTarget {uint8_t(t), uint8_t(param), uint8_t(pad), 50, info.info->min, info.info->max, 0};
        if (m.kind == ModKind::Off) m.kind = ModKind::Lfo;
        ui.mod_target = uint8_t(i);
        send_modulator(c, ui.mod_index);
    } else {
        auto& m = c.project.macros[ui.macro_index % macro_count];
        for (const auto& g : m.targets) if (g.used() && g.track == t && g.param == param && g.pad == pad) { c.notice("Already on it"); return; }
        unsigned i = 0;
        while (i < macro_targets && m.targets[i].used()) ++i;
        if (i == macro_targets) { c.notice("Eight targets at most"); return; }
        // From where it stands now (the knob's left) to the far end of its range (its right).
        int now = info.value, far = now >= (info.info->min + info.info->max) / 2 ? info.info->min : info.info->max;
        m.targets[i] = MacroTarget {uint8_t(t), uint8_t(param), uint8_t(pad), int16_t(now), int16_t(far)};
        ui.macro_target = uint8_t(i);
    }
    c.dirty = true;
}

void mod_remove(Context& c) {
    auto& ui = c.ui;
    if (ui.mod_view == 0) {
        auto& g = c.project.modulators[ui.mod_index % modulator_count].targets[ui.mod_target % modulator_targets];
        if (!g.used()) return;
        g = ModTarget {};
        send_modulator(c, ui.mod_index);
    } else {
        auto& g = c.project.macros[ui.macro_index % macro_count].targets[ui.macro_target % macro_targets];
        if (!g.used()) return;
        g = MacroTarget {};
    }
    c.notice("Target off");
    c.dirty = true;
}

void set_lane(Context& c, unsigned t, unsigned param, unsigned pad, int value) {
    t %= track_count;
    auto& track = c.project.tracks[t];
    if (is_mix_lane(param)) {
        if (param == motion_level) { track.mix.level = uint8_t(std::clamp(value, 0, 127)); c.send({Type::Level, uint8_t(t), 0, 0, int16_t(track.mix.level)}); }
        else if (param == motion_pan) { track.mix.pan = int8_t(std::clamp(value, -50, 50)); c.send({Type::Pan, uint8_t(t), 0, 0, int16_t(track.mix.pan)}); }
        else if (param < motion_master_fx_base) {
            unsigned b = (param - motion_send(0)) % bus_count;
            track.mix.sends[b] = uint8_t(std::clamp(value, 0, 100));
            c.send({Type::Send, uint8_t(t), 0, uint16_t(b), int16_t(track.mix.sends[b])});
        } else if (param >= motion_return_base) c.set_return((param - motion_return_base) % bus_count, value);
        else c.set_effect_param(master_chain, (param - motion_master_fx_base) / 6, (param - motion_master_fx_base) % 6, value);
        return;
    }
    if (param == motion_wheel || param == motion_bend) return; // the fader's own
    if (int which = pad_param_of(param); which >= 0) {
        auto& sound = track.pads[pad % sampler_pads];
        set_pad_param(sound, PadParam(which), value);
        c.send_pad(t, pad % sampler_pads);
        return;
    }
    if (param >= motion_fx_base) { c.set_effect_param(t, (param - motion_fx_base) / 6, (param - motion_fx_base) % 6, value); return; }
    c.set_param(t, param, value);
}

bool mod_binding(Context& c, const Binding& b, int value) {
    auto& ui = c.ui;
    auto& m = c.project.modulators[ui.mod_index % modulator_count];
    auto& macro = c.project.macros[ui.macro_index % macro_count];
    bool resend = false;
    switch (b.kind) {
    case Kind::ModPick: ui.mod_index = uint8_t(value); ui.mod_target = 0; return true;
    case Kind::ModKind: m.kind = ModKind(value); resend = true; break;
    case Kind::ModShape: m.shape = LfoShape(value); resend = true; break;
    case Kind::ModSync: m.sync = value != 0; resend = true; break;
    case Kind::ModRate: if (m.sync) m.division = uint8_t(value); else m.rate = uint8_t(value); resend = true; break;
    case Kind::ModInput: m.source = uint8_t(value); resend = true; break;
    case Kind::ModAttack: m.attack = uint8_t(value); resend = true; break;
    case Kind::ModRelease: m.release = uint8_t(value); resend = true; break;
    case Kind::ModDepth: {
        auto& g = m.targets[ui.mod_target % modulator_targets];
        if (!g.used()) return true;
        g.depth = int8_t(value);
        c.send(pack_mod_target(ui.mod_index % modulator_count, ui.mod_target % modulator_targets,
                               ModTarget {g.track, g.param, g.pad, g.depth, g.low, g.high, int16_t(lane_own(c.project, g.track, g.param, g.pad))}));
        c.dirty = true;
        return true;
    }
    case Kind::MacroPick: ui.macro_index = uint8_t(value); ui.macro_target = 0; return true;
    case Kind::MacroValue: // one knob, every target from its low to its high
        macro.value = uint8_t(value);
        for (const auto& g : macro.targets) if (g.used()) set_lane(c, g.track, g.param, g.pad, Macro::at(g, value));
        c.dirty = true;
        return true;
    case Kind::MacroLow: case Kind::MacroHigh: {
        auto& g = macro.targets[ui.macro_target % macro_targets];
        if (!g.used()) return true;
        (b.kind == Kind::MacroLow ? g.low : g.high) = int16_t(value);
        c.dirty = true;
        return true;
    }
    default: return false;
    }
    if (resend) send_modulator(c, ui.mod_index);
    c.dirty = true;
    return true;
}

} // namespace melodee
