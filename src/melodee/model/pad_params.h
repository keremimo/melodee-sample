// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/sampler.h"
#include <algorithm>

// A kit pad's own values that Motion records, one lane per pad (the MPC's
// pad automation: volume, tuning, pan, the amp and filter envelopes,
// velocity sensitivity and offset, and the pad's filter). Plain data:
// reading and writing them on a PadSound, and their ranges. New lanes go at
// the end: the file keeps a lane by this number (MOTP).
namespace melodee {

enum class PadParam : uint8_t {
    Level, Pitch, Fine, Pan, Attack, Decay, Sustain, Release,
    VelocitySense, Offset,
    FilterAttack, FilterDecay, FilterSustain, FilterRelease, FilterAmount, // the pad filter's envelope
    FilterCutoff, FilterResonance,                                         // and the filter
    Count
};
constexpr unsigned pad_param_count = unsigned(PadParam::Count);
static_assert(motion_pad_base + pad_param_count <= 256, "pad lanes must fit a motion event's parameter byte");

// The motion parameter of a pad lane, and back (-1: not a pad lane).
constexpr unsigned motion_pad_param(PadParam p) { return motion_pad_base + unsigned(p); }
constexpr int pad_param_of(unsigned param) {
    return param >= motion_pad_base && param < motion_pad_base + pad_param_count ? int(param - motion_pad_base) : -1;
}

inline bool pad_param_built(PadParam p) { return unsigned(p) < pad_param_count; }

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
    case PadParam::VelocitySense: return pad.velocity_sense;
    case PadParam::Offset: return pad.offset;
    case PadParam::FilterAttack: return pad.filter_attack;
    case PadParam::FilterDecay: return pad.filter_decay;
    case PadParam::FilterSustain: return pad.filter_sustain;
    case PadParam::FilterRelease: return pad.filter_release;
    case PadParam::FilterAmount: return pad.filter_amount;
    case PadParam::FilterCutoff: return pad.cutoff;
    case PadParam::FilterResonance: return pad.resonance;
    default: return 0;
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
    case PadParam::VelocitySense: pad.velocity_sense = uint8_t(value); break;
    case PadParam::Offset: pad.offset = uint8_t(value); break;
    case PadParam::FilterAttack: pad.filter_attack = uint8_t(value); break;
    case PadParam::FilterDecay: pad.filter_decay = uint8_t(value); break;
    case PadParam::FilterSustain: pad.filter_sustain = uint8_t(value); break;
    case PadParam::FilterRelease: pad.filter_release = uint8_t(value); break;
    case PadParam::FilterAmount: pad.filter_amount = uint8_t(value); break;
    case PadParam::FilterCutoff: pad.cutoff = uint8_t(value); break;
    case PadParam::FilterResonance: pad.resonance = uint8_t(value); break;
    default: break;
    }
}

} // namespace melodee
