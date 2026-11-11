// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <array>
#include <cstdint>
#include <string>

// Scales, note names and what each pad plays in each layout.
namespace melodee {

struct Scale { const char* name; uint16_t mask; }; // bit n: n semitones above the root
// New scales go on the end: project files keep the index.
inline const std::array<Scale, 13>& scales() {
    static const std::array<Scale, 13> list {{
        {"Major", 0xAB5}, {"Minor", 0x5AD}, {"Dorian", 0x6AD}, {"Phrygian", 0x5AB}, {"Lydian", 0xAD5},
        {"Mixolydian", 0x6B5}, {"Locrian", 0x56B}, {"Harm minor", 0x9AD}, {"Mel minor", 0xAAD},
        {"Major pent", 0x295}, {"Minor pent", 0x4A9}, {"Blues", 0x4E9}, {"Chromatic", 0xFFF},
    }};
    return list;
}
constexpr uint8_t chromatic_scale = 12; // scales()'s index; what a new project starts in
inline const char* note_letter(unsigned pitch_class) {
    static const char* names[] {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[pitch_class % 12];
}
// C4 is MIDI 60, so D2 is 38.
inline std::string note_name(int key) { return std::string(note_letter(unsigned(key) % 12)) + std::to_string(key / 12 - 1); }

inline const char* layout_name(PadLayout layout) {
    static const char* names[] {"Scale", "Chromatic", "Chords", "16 Levels"};
    return names[unsigned(layout) % unsigned(PadLayout::Count)];
}

// The scale's semitone offsets above the root, in order.
inline unsigned scale_degrees(unsigned scale, std::array<uint8_t, 12>& out) {
    unsigned n = 0, mask = scales()[scale % scales().size()].mask;
    for (unsigned s = 0; s < 12; ++s) if (mask & (1u << s)) out[n++] = uint8_t(s);
    return n;
}

// The key pad 0..15 (pad 1 bottom left) plays on a track, and whether it is
// a root. 16 Levels plays the track's last note on every pad.
struct PadNote { int key; bool root; };
inline PadNote pad_note(const Project& project, const Track& track, unsigned pad) {
    int base = 12 * (track.octave + 1) + project.root + track.transpose;
    switch (track.layout) {
    case PadLayout::Chromatic: return {base + int(pad), pad % 12 == 0};
    case PadLayout::Levels: return {track.last_note, false};
    case PadLayout::Scale:
    case PadLayout::Chords:
    default: {
        std::array<uint8_t, 12> degrees {};
        unsigned count = scale_degrees(project.scale, degrees);
        return {base + 12 * int(pad / count) + degrees[pad % count], pad % count == 0};
    }
    }
}
// A pad's velocity in the 16 Levels layout: soft at pad 1, loud at pad 16.
inline uint8_t level_velocity(unsigned pad) { return uint8_t(8 + (pad * 119 + 7) / 15); }
// A pad's pitch in semitones when 16 Levels goes up the scale (pad 1 the root).
inline int level_scale_step(const Project& project, unsigned pad) {
    std::array<uint8_t, 12> degrees {};
    unsigned count = scale_degrees(project.scale, degrees);
    return 12 * int(pad / count) + degrees[pad % count];
}

// 16 Levels (the MPC's types, and Melodee's scale): what pad 0..15 plays
// of the one sound. Velocity soft to loud; Tune by semitones with the
// original on pad 4 (on a kit, a whole octave up or down by − / +); Filter the pad's cutoff from the bottom up to its own
// over pads 1-8 and on to the top over 9-16 (kit pads only); Scale up the
// project's scale. velocity 0: the pad's own; cutoff -1: the pad's own.
// Loop: 16 Levels opens the Looper, its pads the loop grid (docs/LOOPER.md).
enum class LevelsKind : uint8_t { Velocity, Scale, Tune, Filter, Loop, Count };
inline const char* levels_kind_name(LevelsKind k) {
    static const char* const names[] {"Velocity", "Scale", "Tune", "Filter", "Loop"};
    return names[unsigned(k) % unsigned(LevelsKind::Count)];
}
struct Level { int velocity = 0, semitones = 0, cutoff = -1, shown = 0; };
// A kit pad's 16 Levels in pitch (Scale, Tune) an octave a step of the track's
// octave, 2 as it is (− / + step it there); a synth's pads keep their own.
constexpr int kit_level_octave = 2, kit_lowest_octave = -1, kit_highest_octave = 7;
inline int level_octave(const Track& track) { return track.engine == EngineKind::Sampler ? 12 * (track.octave - kit_level_octave) : 0; }
inline Level levels_of(const Project& project, const Track& track, unsigned pad) {
    pad %= 16;
    Level out;
    switch (LevelsKind(track.levels_kind % unsigned(LevelsKind::Count))) {
    case LevelsKind::Scale: out.semitones = level_scale_step(project, pad) + level_octave(track); out.shown = out.semitones; break;
    case LevelsKind::Tune: out.semitones = int(pad) - 3 + level_octave(track); out.shown = out.semitones; break;
    case LevelsKind::Filter:
        if (track.engine == EngineKind::Sampler) {
            int own = track.pads[track.levels_pad % sampler_pads].cutoff;
            out.cutoff = pad < 8 ? own * int(pad) / 7 : own + (100 - own) * int(pad - 7) / 8;
            out.shown = out.cutoff;
            break;
        }
        [[fallthrough]]; // a synth has no pad filter: soft to loud
    case LevelsKind::Velocity:
    default: out.velocity = level_velocity(pad); out.shown = out.velocity; break;
    }
    return out;
}

} // namespace melodee
