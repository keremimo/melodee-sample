// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>

// The mod matrix's sources and destinations, the same for every engine; each
// engine decides how far a destination moves. A track holds mod_routes of
// them (model/project.h, ModRoute); amount runs -100..100, 0 is none.
namespace melodee {

enum class ModSource : uint8_t { None, Lfo, Velocity, Aftertouch, Wheel, KeyTrack, Random, FilterEnv, Count };
enum class ModDest : uint8_t { None, Cutoff, Resonance, AmpLevel, Pitch, OscAPitch, OscBPitch, PulseWidth, LfoRate, Noise, Count };

inline const char* mod_source_name(ModSource s) {
    static const char* names[] {"None", "LFO", "Velocity", "Aftertouch", "Wheel", "Key track", "Random", "Filter env"};
    return names[unsigned(s) < unsigned(ModSource::Count) ? unsigned(s) : 0];
}
inline const char* mod_dest_name(ModDest d) {
    static const char* names[] {"None", "Cutoff", "Resonance", "Amp level", "Pitch", "Osc A pitch", "Osc B pitch",
                                "Pulse width", "LFO rate", "Noise"};
    return names[unsigned(d) < unsigned(ModDest::Count) ? unsigned(d) : 0];
}

constexpr int mod_amount_max = 100;

} // namespace melodee
