// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

// A sampler track's kit: 128 pads in eight banks of sixteen (A-H, as the
// MPC's Pad Bank steps them), each a region of one of the project's samples
// (pads share files; nothing is copied when a sound is chopped). Plain data,
// like the rest of the model.
namespace melodee {

constexpr unsigned sampler_banks = 8, sampler_pads = 16 * sampler_banks;
constexpr unsigned legacy_pads = 64;     // banks A-D: the pads a project file's TRAK section holds
constexpr unsigned sample_slots = 64;     // samples a project holds
constexpr uint16_t no_sample = 0xffff;
constexpr unsigned sampler_first_note = 36; // pad 1 of bank A; pad n of the kit is note 36 + n
// Kit notes run past MIDI's 127 (bank H pad 16 is 163): they stay inside Melodee.
constexpr unsigned sampler_last_key = sampler_first_note + sampler_pads - 1;

constexpr unsigned punch_kinds = 27;       // PunchKind::Count (engine/punch.h)

enum class PadPlay : uint8_t { Gate, OneShot };

struct PadSound {
    uint16_t sample = no_sample;  // index into Project::samples
    uint32_t start = 0, end = 0;  // frames; end 0 means the sample's end
    int8_t pitch = 0;             // semitones
    int8_t fine = 0;              // cents
    uint8_t level = 100;          // 0..127, 100 is unity
    int8_t pan = 0;               // -50..50
    PadPlay play = PadPlay::OneShot;
    bool reverse = false;
    uint8_t group = 0;            // mute group 1..8, 0 none: pads in a group cut each other
    uint8_t attack = 0, decay = 100, sustain = 100, release = 20; // 0..100
    uint16_t own_tempo = 0;       // tenths of a BPM the region was played at, for Follow; 0 unknown
    uint8_t punch = 0;            // Pad Punch: 0 none, else PunchKind + 1
    uint8_t punch_rate = 2;       // its grid, 0 1/4 .. 3 1/32
    uint8_t punch_depth = 100;    // when pressure is not used
    bool used() const { return sample != no_sample; }
};

struct SampleRef {
    char path[160] {};  // on the card; empty: free
    char name[28] {};
    uint32_t frames = 0; // at 44.1 kHz, once loaded
    bool used() const { return path[0] != 0; }
};

// The pad's label as the boards print it: A01 .. D16.
inline void pad_label(unsigned pad, char out[4]) {
    out[0] = char('A' + (pad / 16) % sampler_banks);
    out[1] = char('0' + (pad % 16 + 1) / 10);
    out[2] = char('0' + (pad % 16 + 1) % 10);
    out[3] = 0;
}

} // namespace melodee
