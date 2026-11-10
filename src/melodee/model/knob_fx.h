// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include <array>
#include <cstdint>

// Knob FX (parity phase 7): the MPC's performance effect. One effect on
// K1-K3 (Shift + K1-K3 its second row), heard on the kit pads or tracks the
// pads switched on. Saved with the project except whether the mode is on.
namespace melodee {

struct KnobFx {
    bool on = false;      // the mode (Knob FX lit): the pots are the effect's; not saved
    bool bypass = false;  // B3: every target dry
    uint8_t choice = 0;   // the entry of the Knob FX list loaded (engine/effect_info.h knob_fx_entry)
    uint8_t kind = 0;     // its EffectKind
    std::array<int16_t, 6> params {};
    uint16_t tracks = 0;  // off kit pages: a bit a track
    std::array<std::array<uint64_t, 2>, track_count> pads {}; // kit pads, a bit a pad, per track
    bool track_on(unsigned t) const { return (tracks >> (t % track_count)) & 1; }
    void set_track(unsigned t, bool value) {
        uint16_t bit = uint16_t(1u << (t % track_count));
        tracks = value ? uint16_t(tracks | bit) : uint16_t(tracks & ~bit);
    }
    bool pad_on(unsigned t, unsigned pad) const { pad %= 128; return (pads[t % track_count][pad / 64] >> (pad % 64)) & 1; }
    void set_pad(unsigned t, unsigned pad, bool value) {
        pad %= 128;
        uint64_t bit = uint64_t(1) << (pad % 64);
        auto& word = pads[t % track_count][pad / 64];
        word = value ? word | bit : word & ~bit;
    }
    bool any_pad(unsigned t) const { return pads[t % track_count][0] || pads[t % track_count][1]; }
    bool active() const { return on && !bypass && kind != 0; } // heard now
};

} // namespace melodee
