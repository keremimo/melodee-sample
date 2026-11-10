// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <array>
#include <cstdint>
#include <cmath>
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

enum class PadPlay : uint8_t { Gate, OneShot }; // Gate is the MPC's Note On (Shift + Chop)

// A pad's own filter (parity phase 4, the MPC's types); the kit filter stays the track's.
enum class PadFilter : uint8_t { Off, Classic, Lpf2, Lpf4, Hpf2, Hpf4, Bpf2, Bpf4, Count };
constexpr unsigned pad_filter_count = unsigned(PadFilter::Count);
inline const char* pad_filter_name(PadFilter f) {
    static const char* const names[] {"Off", "Classic", "LPF2", "LPF4", "HPF2", "HPF4", "BPF2", "BPF4"};
    return names[unsigned(f) % pad_filter_count];
}

// Warp (Tune page, K3): off, locked to the sequence's tempo, or 50..200 % of its length.
constexpr uint8_t warp_off = 0, warp_seq = 1, warp_min = 50, warp_max = 200;
constexpr unsigned max_pad_beats = 64; // # Beats: 0 works it out from the length

// Volume as the MPC shows it: 0 is -inf, 1..100 run -74..0 dB, 100..127
// run 0..+6 dB. Pad volume and kit volume both use it.
constexpr float level_floor_db = -74.0f, level_top_db = 6.0f;
inline float volume_db(int level) {
    if (level <= 0) return -INFINITY;
    if (level <= 100) return 37.0f * std::log10(float(level) / 100.0f); // 1 is -74 dB
    return level_top_db * float(std::min(level, 127) - 100) / 27.0f;
}
inline float volume_gain(int level) { return level <= 0 ? 0.0f : std::pow(10.0f, volume_db(level) / 20.0f); }

struct PadSound {
    uint16_t sample = no_sample;  // index into Project::samples
    uint32_t start = 0, end = 0;  // frames; end 0 means the sample's end
    int8_t pitch = 0;             // semitones
    int8_t fine = 0;              // cents
    uint8_t level = 100;          // 0..127, 100 is unity
    int8_t pan = 0;               // -50..50
    PadPlay play = PadPlay::OneShot;
    bool reverse = false;
    uint8_t group = 0;            // mute group 1..16, 0 none: pads in a group cut each other
    uint8_t attack = 0, decay = 100, sustain = 100, release = 20; // 0..100
    uint16_t own_tempo = 0;       // tenths of a BPM the region was played at, for Follow; 0 unknown
    uint8_t punch = 0;            // Pad Punch: 0 none, else PunchKind + 1
    uint8_t punch_rate = 2;       // its grid, 0 1/4 .. 3 1/32
    uint8_t punch_depth = 100;    // when pressure is not used
    // Parity phase 4, the MPC's Sample Mode (the file's PAD4 section).
    bool loop = false;            // Loop: after the end it plays on from the loop point
    bool loop_lock = true;        // Loop Lock (Shift + B1 on Trim): the loop point is the start
    uint32_t loop_start = 0;      // frames, when not locked
    bool mono = false;            // a new hit stops the pad's last one (else Poly: they overlap)
    uint8_t link = 0;             // Pad Link: 1..16 a pad of the same bank played with it, 0 none
    uint8_t offset = 0;           // 0..100: the start waits up to a sixteenth after the hit
    uint8_t velocity_sense = 100; // 0..100: 0 every hit full, 100 soft hits quietest
    bool decay_from_start = false; // One shot: decay from the start (ADSR), else over the decay before the end (MPC's default)
    PadFilter filter = PadFilter::Off;
    uint8_t cutoff = 100, resonance = 0;                                           // 0..100
    uint8_t filter_attack = 0, filter_decay = 50, filter_sustain = 0, filter_release = 20, filter_amount = 0; // 0..100
    uint8_t warp = warp_off;      // warp_off, warp_seq, or 50..200 %
    uint8_t beats = 0;            // # Beats for Warp's Seq, 0 from its length
    int16_t gain = 0;             // hundredths of a dB: Normalize (Shift + B2 on Trim)
    uint32_t loop_point() const { return loop_lock ? start : loop_start; }
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
