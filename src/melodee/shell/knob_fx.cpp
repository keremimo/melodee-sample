// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Knob FX (parity phase 7), as on the MPC: Knob FX turns the mode on and
// off; while on, K1-K3 (Shift: the second row) are the effect's, on every
// page. The Knob FX page lists the effects (wheel or Minus / Plus, Push
// loads), its pads switch the effect on per kit pad (a kit track) or per
// track, B2 switches it on for all, B3 bypasses. Rec Input FX puts it on the
// input during a take.
#include "melodee/engine/effect_info.h"
#include "melodee/shell/context.h"

namespace melodee {
using Type = Command::Type;

bool Context::knob_fx_kit() { return track().engine == EngineKind::Sampler; }

void Context::knob_fx_button() {
    auto& k = project.knob_fx;
    k.on = !k.on;
    if (k.on) {
        if (!k.kind) knob_fx_load(k.choice);
        ui.knob_cursor = k.choice;
        open(PageId::KnobFx);
        notice(std::string("Knob FX: ") + knob_fx_entry(k.choice).name);
    } else {
        if (ui.page == PageId::KnobFx) back();
        notice("Knob FX off");
    }
    send_knob_state();
}

void Context::knob_fx_load(unsigned entry) {
    auto& k = project.knob_fx;
    const auto& e = knob_fx_entry(entry);
    k.choice = uint8_t(entry);
    k.kind = uint8_t(e.kind);
    const auto& info = effect_info(e.kind);
    for (unsigned q = 0; q < effect_params; ++q) k.params[q] = info.params[q].label ? info.params[q].initial : 0;
    send({Type::KnobFx, 0, 0, 0, int16_t(k.kind)});
    if (e.param >= 0 && unsigned(e.param) < effect_params) knob_fx_param(unsigned(e.param), e.value);
    dirty = true;
}

void Context::knob_fx_param(unsigned q, int value) {
    auto& k = project.knob_fx;
    if (q >= effect_params) return;
    const auto& pi = effect_info(EffectKind(k.kind)).params[q];
    if (!pi.label) return;
    k.params[q] = int16_t(std::clamp<int>(value, pi.min, pi.max));
    send({Type::KnobFx, 0, uint8_t(q), 1, k.params[q]});
    dirty = true;
}

void Context::knob_fx_pad(unsigned pad) {
    auto& k = project.knob_fx;
    if (knob_fx_kit()) {
        unsigned kit = track().bank % sampler_banks * 16 + pad % 16;
        k.set_pad(ui.track, kit, !k.pad_on(ui.track, kit));
    } else {
        k.set_track(pad % track_count, !k.track_on(pad % track_count));
    }
    send_knob_targets();
}

void Context::knob_fx_all() {
    auto& k = project.knob_fx;
    if (knob_fx_kit()) { // the bank in view: all on, or (all on already) all off
        unsigned first = track().bank % sampler_banks * 16;
        bool all = true;
        for (unsigned n = 0; n < 16; ++n) all &= k.pad_on(ui.track, first + n);
        for (unsigned n = 0; n < 16; ++n) k.set_pad(ui.track, first + n, !all);
        notice(all ? "Knob FX: no pads" : "Knob FX: all pads");
    } else {
        bool all = k.tracks == 0xffff;
        k.tracks = all ? 0 : 0xffff;
        notice(all ? "Knob FX: no tracks" : "Knob FX: all tracks");
    }
    send_knob_targets();
}

void Context::knob_fx_bypass() {
    auto& k = project.knob_fx;
    k.bypass = !k.bypass;
    notice(k.bypass ? "Knob FX bypassed" : "Knob FX engaged");
    send_knob_state();
    dirty = true;
}

void Context::send_knob_state() {
    const auto& k = project.knob_fx;
    send({Type::KnobFx, 0, 0, 2, int16_t((k.active() ? 1 : 0) | (ui.settings.input.fx ? 2 : 0))});
}

void Context::send_knob_targets() {
    const auto& k = project.knob_fx;
    Command tracks {Type::KnobFx, 0, 0, 3};
    tracks.position = k.tracks;
    send(tracks);
    for (unsigned quarter = 0; quarter < 4; ++quarter) { // pads change only on the track in view
        Command pads {Type::KnobFx, ui.track, uint8_t(quarter), 4};
        pads.position = uint32_t(k.pads[ui.track % track_count][quarter / 2] >> (quarter % 2 * 32));
        send(pads);
    }
    dirty = true;
}

// The master compressor (Shift + Pad 5).
void Context::set_compressor(unsigned what, int value) {
    auto& c = project.compressor;
    switch (what) {
    case 0: c.on = value != 0; break;
    case 1: c.color = value != 0; break;
    case 2: c.attack = uint8_t(std::clamp(value, 0, 100)); value = c.attack; break;
    case 3: c.release = uint8_t(std::clamp(value, 0, 100)); value = c.release; break;
    case 4: c.amount = uint8_t(std::clamp(value, 0, 100)); value = c.amount; break;
    case 5: c.boost = uint8_t(std::clamp(value, 0, 24)); value = c.boost; break;
    default: return;
    }
    send({Type::Compressor, 0, 0, uint16_t(what), int16_t(value)});
    dirty = true;
}

} // namespace melodee
