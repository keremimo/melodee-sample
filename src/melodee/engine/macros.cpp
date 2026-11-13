// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/macros.h"
#include "melodee/engine/dsp.h"
#include <algorithm>
#include <cmath>

namespace melodee {

const ParamInfo& macro_param(MacroKind kind) {
    static const ParamInfo filter {"FILTER", -100, 100, 0, Format::Sweep};
    static const ParamInfo echo {"ECHO", 0, 100, 0, Format::Percent};
    static const ParamInfo space {"SPACE", 0, 100, 0, Format::Percent};
    switch (kind) {
    case MacroKind::Echo: return echo;
    case MacroKind::Space: return space;
    case MacroKind::Filter: case MacroKind::End: break;
    }
    return filter;
}

namespace {
constexpr int echo_sync = 7; // Echo's SYNC: 1/8D (effect_info.cpp, delay_syncs)
bool is_send(MacroKind kind) { return kind == MacroKind::Echo || kind == MacroKind::Space; }
EffectKind effect_of(MacroKind kind) {
    switch (kind) {
    case MacroKind::Echo: return EffectKind::Echo;
    case MacroKind::Space: return EffectKind::Reverb;
    case MacroKind::Filter: case MacroKind::End: break;
    }
    return EffectKind::Filter;
}
} // namespace

void MacroStage::set(unsigned slot, MacroKind kind, int value) {
    if (slot >= macro_slots) return;
    auto& s = slots[slot];
    const auto& info = macro_param(kind);
    if (s.kind != kind) {
        if (s.set_up && pool) s.fx.set_kind(EffectKind::None, *pool); // its memory back; the new kind starts from silence
        s.set_up = false; s.send = 0; s.quiet = 0;
        s.kind = kind;
    }
    s.value = std::clamp<int>(value, info.min, info.max);
    if (s.set_up) tune(s);
}

// The effect's own parameters for the slot's value.
void MacroStage::tune(Slot& s) {
    float amount = std::abs(s.value) / 100.0f;
    switch (s.kind) {
    case MacroKind::Filter: {
        bool high = s.value > 0;
        // Low pass from 20 kHz down to about 80 Hz; high pass from 20 Hz up to about 3.5 kHz; the resonance rises as it goes.
        s.fx.set_param(0, int(std::lround(high ? amount * 75 : 100 - amount * 80)));
        s.fx.set_param(1, int(std::lround(15 + amount * 35)));
        s.fx.set_param(2, high ? 2 : 0);
        break;
    }
    case MacroKind::Echo:
        s.fx.set_param(1, 55); s.fx.set_param(2, 100); s.fx.set_param(3, 45); s.fx.set_param(4, echo_sync); // feedback, all wet, darker, 1/8D
        break;
    case MacroKind::Space:
        s.fx.set_param(0, 80); s.fx.set_param(1, 88); s.fx.set_param(2, 35); s.fx.set_param(3, 100); // size, decay, damp, all wet
        break;
    case MacroKind::End: break;
    }
}

// Echo and Space: the input at the knob's level into the effect, all wet, added to the dry sound.
void MacroStage::send_into(Slot& s, float* left, float* right, unsigned frames) {
    float aim = s.value / 100.0f;
    if (!s.set_up) {
        if (aim <= 0 || !pool) return;
        s.fx.set_kind(effect_of(s.kind), *pool);
        if (!s.fx.holding_memory()) { s.fx.set_kind(EffectKind::None, *pool); return; } // the pool is used up: silent
        s.set_up = true; s.send = 0; s.quiet = 0;
        tune(s);
    }
    float wet_l[256], wet_r[256], peak = 0;
    for (unsigned at = 0; at < frames; at += 256) {
        unsigned n = std::min(256u, frames - at);
        // Toward the knob at a fixed pace (all the way in 10 ms): no zipper as it turns, and the same sound however the
        // host cuts its blocks.
        float pace = 100.0f / float(dsp::sample_rate()), g = s.send;
        for (unsigned f = 0; f < n; ++f) {
            g = aim > g ? std::min(aim, g + pace) : std::max(aim, g - pace);
            wet_l[f] = left[at + f] * g; wet_r[f] = right[at + f] * g;
        }
        s.send = g;
        s.fx.process(wet_l, wet_r, n);
        for (unsigned f = 0; f < n; ++f) {
            left[at + f] += wet_l[f]; right[at + f] += wet_r[f];
            peak = std::max({peak, std::fabs(wet_l[f]), std::fabs(wet_r[f])});
        }
    }
    // Silent with nothing going in for a second: its memory back to the pool until the knob comes up again.
    s.quiet = aim <= 0 && s.send <= 0 && peak < 1e-5f ? s.quiet + frames : 0;
    if (s.quiet >= uint32_t(dsp::sample_rate())) {
        s.fx.set_kind(EffectKind::None, *pool);
        s.set_up = false; s.send = 0; s.quiet = 0;
    }
}

void MacroStage::process(float* left, float* right, unsigned frames) {
    for (auto& s : slots) {
        if (is_send(s.kind)) { send_into(s, left, right, frames); continue; }
        if (s.value == 0) continue; // the filter in the middle: not there
        if (!s.set_up) {
            if (!pool) continue;
            s.fx.set_kind(effect_of(s.kind), *pool);
            s.set_up = true;
            tune(s);
        }
        for (unsigned at = 0; at < frames; at += 256) s.fx.process(left + at, right + at, std::min(256u, frames - at));
    }
}

void MacroStage::release() {
    for (auto& s : slots) {
        if (s.set_up && pool) s.fx.set_kind(EffectKind::None, *pool);
        s.set_up = false; s.send = 0; s.quiet = 0;
    }
}

bool MacroStage::sounding() const {
    for (const auto& s : slots) if (s.set_up && is_send(s.kind)) return true;
    return false;
}

} // namespace melodee
