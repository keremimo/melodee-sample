// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/sampler.h"
#include <algorithm>

// A kit pad's own values that Motion records, one lane per pad (the MPC's
// pad automation: volume, tuning, pan, the amp and filter envelopes,
// velocity sensitivity and offset). Plain data: reading and writing them on
// a PadSound, and their ranges. The ones a pad does not hold yet (the
// filter envelope, velocity sensitivity and offset, parity phase 4) are
// named here so their lanes have a place; `pad_param_built()` says which
// exist, and only those are recorded or played.
namespace melodee {

enum class PadParam : uint8_t {
    Level, Pitch, Fine, Pan, Attack, Decay, Sustain, Release, // what a pad holds now
    VelocitySense, Offset,                                    // phase 4
    FilterAttack, FilterDecay, FilterSustain, FilterRelease, FilterAmount, // phase 4: the per-pad filter's envelope
    Count
};
constexpr unsigned pad_param_count = unsigned(PadParam::Count);
static_assert(motion_pad_base + pad_param_count <= 256, "pad lanes must fit a motion event's parameter byte");

// The motion parameter of a pad lane, and back (-1: not a pad lane).
constexpr unsigned motion_pad_param(PadParam p) { return motion_pad_base + unsigned(p); }
constexpr int pad_param_of(unsigned param) {
    return param >= motion_pad_base && param < motion_pad_base + pad_param_count ? int(param - motion_pad_base) : -1;
}

inline bool pad_param_built(PadParam p) { return unsigned(p) <= unsigned(PadParam::Release); }

struct PadParamRange { int min, max; };
inline PadParamRange pad_param_range(PadParam p) {
    switch (p) {
    case PadParam::Level: return {0, 127};
    case PadParam::Pitch: return {-24, 24};
    case PadParam::Fine: return {-50, 50};
    case PadParam::Pan: return {-50, 50};
    default: return {0, 100};
    }
}

inline int pad_param_value(const PadSound& pad, PadParam p) {
    switch (p) {
    case PadParam::Level: return pad.level;
    case PadParam::Pitch: return pad.pitch;
    case PadParam::Fine: return pad.fine;
    case PadParam::Pan: return pad.pan;
    case PadParam::Attack: return pad.attack;
    case PadParam::Decay: return pad.decay;
    case PadParam::Sustain: return pad.sustain;
    case PadParam::Release: return pad.release;
    default: return 0; // phase 4
    }
}

inline void set_pad_param(PadSound& pad, PadParam p, int value) {
    auto r = pad_param_range(p);
    value = std::clamp(value, r.min, r.max);
    switch (p) {
    case PadParam::Level: pad.level = uint8_t(value); break;
    case PadParam::Pitch: pad.pitch = int8_t(value); break;
    case PadParam::Fine: pad.fine = int8_t(value); break;
    case PadParam::Pan: pad.pan = int8_t(value); break;
    case PadParam::Attack: pad.attack = uint8_t(value); break;
    case PadParam::Decay: pad.decay = uint8_t(value); break;
    case PadParam::Sustain: pad.sustain = uint8_t(value); break;
    case PadParam::Release: pad.release = uint8_t(value); break;
    default: break; // phase 4: the pad has no such value yet
    }
}

} // namespace melodee
