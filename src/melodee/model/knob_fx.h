// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include <array>
#include <cmath>
#include <cstdint>

// Knob FX (parity phase 7): the MPC's performance effect. One effect on
// K1-K3 (Shift + K1-K3 its second row), heard on the kit pads or tracks the
// pads switched on. Saved with the project except whether the mode is on.
// Round 2 (knob fx C): two settings of K1-K3, A and B, and the fader on the
// Knob FX page slides the effect from one to the other.
namespace melodee {

// The fader's card on the Knob FX page: Binding::Kind::KnobParam with this
// id is the morph from A to B (ids 0..5 are the effect's parameters).
constexpr unsigned knob_morph = 6;

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

    // Knob FX C: A (B1) and B (B2), K1-K3's values in each, and where the
    // fader has the effect between them (0 A .. 127 B). params[0..2] are the
    // values heard there; the pots change the setting in hand.
    std::array<int16_t, 3> a {}, b {};
    uint8_t morph = 0;
    uint8_t editing = 0; // the setting in hand: 0 A, 1 B (not saved: A after a load)
    std::array<int16_t, 3>& setting() { return editing ? b : a; }
    const std::array<int16_t, 3>& setting() const { return editing ? b : a; }
    // K1-K3's value `q` at the morph: A's, B's, or between them.
    int16_t morphed(unsigned q) const {
        q %= 3;
        return int16_t(a[q] + int(std::lround((b[q] - a[q]) * (morph / 127.0))));
    }
    // A new effect, or a file from before A and B: both are the values heard now, the fader at A.
    void settle() {
        for (unsigned q = 0; q < 3; ++q) a[q] = b[q] = params[q];
        morph = 0; editing = 0;
    }
};

} // namespace melodee
