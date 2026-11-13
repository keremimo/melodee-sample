// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <array>
#include <cmath>
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
    static const char* names[] {"Scale", "Chromatic", "Chords", "16 Levels", "Drums"};
    return names[unsigned(layout) % unsigned(PadLayout::Count)];
}

// Drums (a plug-in or MIDI track): the pads play as a kit's do, whatever the
// key, the octave or the transpose: pad 1 of bank A is note 36 and pad n of
// the bank in view 36 + 16 * bank + n, the MPC's and Ableton's drum-rack
// notes (8W8's "Rack 36"). Banks A-D, so every note stays MIDI's (36-99).
constexpr unsigned drum_banks = 4;
constexpr unsigned drum_first_key = 36, drum_last_key = drum_first_key + 16 * drum_banks - 1;
inline bool drum_pads(const Track& track) {
    return track.layout == PadLayout::Drums && track.engine != EngineKind::Sampler && track.engine != EngineKind::None;
}
inline unsigned drum_bank(const Track& track) { return track.bank % drum_banks; }
inline int drum_key(const Track& track, unsigned pad) {
    if (track.engine == EngineKind::Midi) return track.midi.drum_note[(16 * drum_bank(track) + pad % 16) % sampler_pads]; // its drum map (roadmap P4, M3)
    return int(drum_first_key + 16 * drum_bank(track) + pad % 16);
}
// The pad (0..63, A01..D16) a drum key sits on, -1 for none.
inline int drum_pad_of(int key) { return key >= int(drum_first_key) && key <= int(drum_last_key) ? key - int(drum_first_key) : -1; }

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
    case PadLayout::Drums: return {drum_key(track, pad), false};
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
// of the one sound. Scale goes up the track's key from its root, pad 1 the
// root (the key on the page's K2 and K3): a synth's pads play those notes in
// the track's octave, a kit's sample counts as a C, moved the nearest way to
// the root. The others go evenly from pad 1's end to pad 16's
// (Track::levels_ends, set on K2 and K3): Velocity soft to loud; Tune by
// semitones (the original on pad 4 at first); Filter the pad's cutoff (kit
// pads only). On a kit − / + move Scale and Tune a whole octave. velocity 0:
// the pad's own; cutoff -1: the pad's own; key -1: a synth's last note moved.
// Loop: 16 Levels opens the Looper, its pads the loop grid (docs/LOOPER.md).
// Zones (a kit pad holding an instrument): the keys its zones sound on, lowest
// on pad 1, each pad one of them at its real pitch, sixteen to a page.
enum class LevelsKind : uint8_t { Velocity, Scale, Tune, Filter, Loop, Zones, Count };
inline const char* levels_kind_name(LevelsKind k) {
    static const char* const names[] {"Velocity", "Scale", "Tune", "Filter", "Loop", "Zones"};
    return names[unsigned(k) % unsigned(LevelsKind::Count)];
}
// How far each type's ends go.
struct LevelsRange { int low, high; };
inline LevelsRange levels_range(LevelsKind k) {
    switch (k) {
    case LevelsKind::Velocity: return {1, 127};
    case LevelsKind::Scale: return {-14, 30};
    case LevelsKind::Tune: return {-24, 24};
    case LevelsKind::Filter: return {0, 100};
    default: return {0, 0};
    }
}
// The types a track's K1 turns through, Scale first: a kit has Filter as well.
inline unsigned levels_types(bool kit, std::array<LevelsKind, 4>& out) {
    out = {LevelsKind::Scale, LevelsKind::Tune, LevelsKind::Velocity, LevelsKind::Filter};
    return kit ? 4 : 3;
}
// A track becoming Drums (K3 on Keys, or a drum plug-in loaded): its bank one of A-D, its 16 Levels in velocity.
inline void use_drum_pads(Track& track) {
    track.layout = PadLayout::Drums;
    track.bank = uint8_t(drum_bank(track));
    if (track.levels_kind != unsigned(LevelsKind::Loop)) track.levels_kind = uint8_t(LevelsKind::Velocity);
}
// A track's types: Drums pads have Velocity alone (another pitch is another drum).
inline unsigned levels_types(const Track& track, std::array<LevelsKind, 4>& out) {
    if (drum_pads(track)) { out = {LevelsKind::Velocity, LevelsKind::Velocity, LevelsKind::Velocity, LevelsKind::Velocity}; return 1; }
    return levels_types(track.engine == EngineKind::Sampler, out);
}
// Pad `pad` (0..15) of the way from `low` (pad 1) to `high` (pad 16), rounded.
inline int level_between(int low, int high, unsigned pad) {
    return low + int(std::lround(double(high - low) * double(pad % 16) / 15.0));
}
// A kit's sample counts as a C: the semitones to the track's key root (the project's moved by its transpose), the
// nearest way (−5 .. +6).
inline int kit_root_shift(const Project& project, const Track& track) {
    int root = ((int(project.root) + track.transpose) % 12 + 12) % 12;
    return root > 6 ? root - 12 : root;
}
// A kit's 16 Levels octave (the track's, level_octave() below): 2 as it is.
constexpr int kit_level_octave = 2, kit_lowest_octave = -1, kit_highest_octave = 7;
struct Level { int velocity = 0, semitones = 0, cutoff = -1, shown = 0, key = -1; bool none = false; }; // none: a pad past Zones' last key

// Zones: the instrument on the 16 Levels pad (a kit's), nullptr for none.
inline const SampleRef* levels_instrument(const Project& project, const Track& track) {
    if (track.engine != EngineKind::Sampler) return nullptr;
    const auto& pad = track.pads[track.levels_pad % sampler_pads];
    if (!pad.used()) return nullptr;
    const auto& ref = project.samples[pad.sample % sample_slots];
    return ref.used() && ref.instrument() ? &ref : nullptr;
}
inline unsigned zone_key_count(const SampleRef& ref) {
    unsigned n = 0;
    for (auto word : ref.keys) for (; word; word &= word - 1) ++n;
    return n;
}
// The index'th key its zones sound on, lowest first; -1 past the last.
inline int zone_key(const SampleRef& ref, unsigned index) {
    for (unsigned key = 0; key < 128; ++key)
        if ((ref.keys[key / 64] >> (key % 64)) & 1) { if (!index) return int(key); --index; }
    return -1;
}
// Its pages of sixteen and the one in view: − / + walk them as a kit's octaves, kit_level_octave the first, so at
// most as many as those octaves go up.
inline unsigned zone_pages(const SampleRef& ref) {
    return std::clamp((zone_key_count(ref) + 15) / 16, 1u, unsigned(kit_highest_octave - kit_level_octave + 1));
}
inline unsigned zone_page(const Track& track, unsigned pages) {
    return unsigned(std::clamp(int(track.octave) - kit_level_octave, 0, int(pages) - 1));
}
// The type 16 Levels opens in on a kit pad: Zones on a drum or percussion instrument's pad in place of a pitch type
// (Scale and Tune would play its hits from other keys and skip some), Scale in place of Zones on a pad of no
// instrument; Velocity, Filter and Loop as they were.
inline LevelsKind levels_opening(const Project& project, const Track& track) {
    auto kind = LevelsKind(track.levels_kind % unsigned(LevelsKind::Count));
    const SampleRef* instrument = levels_instrument(project, track);
    if (kind == LevelsKind::Zones && !instrument) return LevelsKind::Scale;
    if (instrument && instrument->one_key_zones && (kind == LevelsKind::Scale || kind == LevelsKind::Tune)) return LevelsKind::Zones;
    return kind;
}
// The types a track's K1 turns through with its pad in view: an instrument's pad has Zones after Tune.
inline unsigned levels_types(const Project& project, const Track& track, std::array<LevelsKind, 5>& out) {
    std::array<LevelsKind, 4> base {};
    unsigned count = levels_types(track, base);
    if (!drum_pads(track) && levels_instrument(project, track)) {
        out = {LevelsKind::Scale, LevelsKind::Tune, LevelsKind::Zones, LevelsKind::Velocity, LevelsKind::Filter};
        return 5;
    }
    for (unsigned n = 0; n < 4; ++n) out[n] = base[n];
    out[4] = base[0];
    return count;
}
// A kit pad's 16 Levels in pitch (Scale, Tune) an octave a step of the track's
// octave, 2 as it is (− / + step it there); a synth's pads keep their own.
inline int level_octave(const Track& track) { return track.engine == EngineKind::Sampler ? 12 * (track.octave - kit_level_octave) : 0; }
inline Level levels_of(const Project& project, const Track& track, unsigned pad) {
    pad %= 16;
    Level out;
    auto kind = LevelsKind(track.levels_kind % unsigned(LevelsKind::Count));
    if (kind == LevelsKind::Filter && track.engine != EngineKind::Sampler) kind = LevelsKind::Velocity; // a synth has no pad filter
    if (kind == LevelsKind::Loop || drum_pads(track)) kind = LevelsKind::Velocity;
    const SampleRef* instrument = kind == LevelsKind::Zones ? levels_instrument(project, track) : nullptr;
    if (kind == LevelsKind::Zones && !instrument) kind = LevelsKind::Scale; // its pad no instrument now
    const auto& ends = track.levels_ends[unsigned(kind) % 4];
    int at = level_between(ends[0], ends[1], pad);
    switch (kind) {
    case LevelsKind::Scale: // up the key from its root, pad 1 the root (no ends)
        if (track.engine == EngineKind::Sampler) out.semitones = kit_root_shift(project, track) + level_scale_step(project, pad) + level_octave(track);
        else out.key = std::clamp(12 * (track.octave + 1) + int(project.root) + track.transpose + level_scale_step(project, pad), 0, 127);
        out.shown = out.key >= 0 ? out.key : out.semitones;
        break;
    case LevelsKind::Tune: out.semitones = at + level_octave(track); out.shown = out.semitones; break;
    case LevelsKind::Zones: { // the key itself: a hit plays 60 + the pad's Semi + these
        const int key = zone_key(*instrument, 16 * zone_page(track, zone_pages(*instrument)) + pad);
        if (key < 0) { out.none = true; out.shown = -1; break; }
        out.semitones = std::clamp(key - 60 - int(track.pads[track.levels_pad % sampler_pads].pitch), -64, 63);
        out.shown = key;
        break;
    }
    case LevelsKind::Filter: out.cutoff = std::clamp(at, 0, 100); out.shown = out.cutoff; break;
    case LevelsKind::Velocity:
    default: out.velocity = std::clamp(at, 1, 127); out.shown = out.velocity; break;
    }
    return out;
}

} // namespace melodee
