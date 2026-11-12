// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/scales.h"
#include <algorithm>
#include <array>
#include <string>

// Chords from the scale: a pad's note is the root, the rest are stacked
// from the scale's own degrees (so they stay in key). Type picks the stack,
// voicing spreads it, invert moves its lowest notes up an octave.
namespace melodee {

inline const char* chord_type_name(unsigned t) { static const char* n[] {"Triad", "Dia 7", "Sus2", "Sus4", "Power", "Dia 9"}; return n[t % 6]; }
inline const char* chord_voicing_name(unsigned v) { static const char* n[] {"Close", "Open", "Drop 2", "Spread"}; return n[v % 4]; }
inline const char* chord_invert_name(unsigned i) { static const char* n[] {"Root", "1st", "2nd", "3rd"}; return n[i % 4]; }

// Up to four notes in order (a plain insertion sort; std::sort's inlining
// trips GCC's bounds check on an array this small).
inline void sort_notes(std::array<int, notes_per_step>& keys, unsigned n) {
    for (unsigned i = 1; i < n && i < keys.size(); ++i)
        for (unsigned j = i; j > 0 && keys[j - 1] > keys[j]; --j) std::swap(keys[j - 1], keys[j]);
}

// The chord on pad `pad` of a track; returns how many notes (up to four).
inline unsigned chord_notes(const Project& project, const Track& track, unsigned pad, std::array<int, notes_per_step>& out) {
    if (track.layout == PadLayout::Drums) { out[0] = pad_note(project, track, pad).key; return 1; } // a drum is no chord's root
    std::array<uint8_t, 12> degrees {};
    unsigned count = scale_degrees(project.scale, degrees);
    int base = 12 * (track.octave + 1) + project.root + track.transpose; // as the pads' (pad_note()): the pad's note is the root
    // Chromatic has no thirds to stack: each pad is its own root with the major scale above it.
    std::array<uint8_t, 12> major {};
    unsigned major_count = count == 12 ? scale_degrees(0, major) : 0;
    auto degree = [&](unsigned step) {
        if (major_count) return base + int(pad) + 12 * int(step / major_count) + major[step % major_count];
        unsigned i = pad + step;
        return base + 12 * int(i / count) + degrees[i % count];
    };
    static const std::array<std::array<int, 4>, 6> stacks {{{0, 2, 4, -1}, {0, 2, 4, 6}, {0, 1, 4, -1}, {0, 3, 4, -1}, {0, 4, 7, -1}, {0, 2, 4, 8}}};
    const auto& stack = stacks[track.chord_type % 6];
    unsigned n = 0;
    for (int step : stack) if (step >= 0) out[n++] = degree(unsigned(step));
    if (track.chord_type % 6 == 4) out[2] = out[0] + 12; // power: root, fifth, octave
    switch (track.chord_voicing % 4) {
    case 1: if (n > 2) out[1] += 12; break;                       // open: the second note up
    case 2: if (n > 2) out[n - 2] -= 12; break;                   // drop 2: the second from the top down
    case 3: for (unsigned k = 1; k < n; k += 2) out[k] += 12; break; // spread
    default: break;
    }
    sort_notes(out, n);
    for (unsigned k = 0; k < std::min<unsigned>(track.chord_invert % 4, n - 1); ++k) { // the lowest goes up an octave
        out[0] += 12;
        sort_notes(out, n);
    }
    return n;
}

// A name for it: the root's letter, m for a minor third, 7 or maj7, sus, 5, add9.
inline std::string chord_name(const Project& project, const Track& track, unsigned pad) {
    std::array<int, notes_per_step> keys {};
    unsigned n = chord_notes(project, track, pad, keys);
    int root = pad_note(project, track, pad).key;
    bool has[12] {};
    for (unsigned k = 0; k < n; ++k) has[((keys[k] - root) % 12 + 12) % 12] = true;
    std::string name = note_letter(unsigned(root) % 12);
    unsigned type = track.chord_type % 6;
    if (type == 4) return name + "5";
    if (type == 2) return name + "sus2";
    if (type == 3) return name + "sus4";
    bool minor = has[3] && !has[4], diminished = minor && has[6] && !has[7];
    if (diminished) name += "dim"; else if (minor) name += "m";
    if (type == 1 || type == 5) name += has[11] ? "maj7" : has[10] ? "7" : "";
    if (type == 5) name += "(9)";
    return name;
}

// Any keys held together, named (round 2, stage-preset B, as the FM-1 firmware names a chord): the shape their pitch
// classes make over a root, the lowest key's first, with a slash and the bass where another root explains them;
// "" for fewer than two pitch classes or a shape it does not know.
inline std::string name_chord(const int* keys, unsigned count) {
    unsigned mask = 0, classes = 0;
    int lowest = 128;
    for (unsigned n = 0; n < count; ++n) {
        if (keys[n] < 0) continue;
        mask |= 1u << (unsigned(keys[n]) % 12);
        lowest = std::min(lowest, keys[n]);
    }
    for (unsigned b = 0; b < 12; ++b) classes += (mask >> b) & 1;
    if (classes < 2) return "";
    struct Shape { unsigned bits; const char* suffix; };
    static const Shape shapes[] {
        {0x091, ""}, {0x089, "m"}, {0x491, "7"}, {0x891, "maj7"}, {0x489, "m7"}, {0x049, "dim"}, {0x111, "aug"},
        {0x085, "sus2"}, {0x0A1, "sus4"}, {0x081, "5"}, {0x291, "6"}, {0x289, "m6"}, {0x249, "dim7"}, {0x449, "m7b5"},
        {0x095, "add9"}, {0x08D, "m(add9)"}, {0x495, "9"}, {0x895, "maj9"}, {0x48D, "m9"}, {0x4A1, "7sus4"}, {0x889, "m(maj7)"},
    };
    unsigned bass = unsigned(lowest) % 12;
    for (unsigned k = 0; k < 12; ++k) {
        unsigned root = (bass + k) % 12;
        unsigned rotated = ((mask >> root) | (mask << (12 - root))) & 0xFFF;
        for (const auto& shape : shapes)
            if (rotated == shape.bits) {
                std::string name = std::string(note_letter(root)) + shape.suffix;
                return root == bass ? name : name + "/" + note_letter(bass);
            }
    }
    return "";
}

} // namespace melodee
