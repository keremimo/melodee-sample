// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
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
// One more slot on the engine's side only: the browser's preview of a file
// (B3 on Samples), never a pad's sample and never in the project.
constexpr unsigned preview_slot = sample_slots;
constexpr uint16_t no_sample = 0xffff;
constexpr unsigned sampler_first_note = 36; // pad 1 of bank A; pad n of the kit is note 36 + n
// Kit notes run past MIDI's 127 (bank H pad 16 is 163): they stay inside Melodee.
constexpr unsigned sampler_last_key = sampler_first_note + sampler_pads - 1;

constexpr unsigned punch_kinds = 47;       // PunchKind::Count (engine/punch.h); 27 before parity phase 7b

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
constexpr unsigned max_loop_xfade = 250; // ms: a loop's seam crossfade (roadmap P3)

// Volume as the MPC shows it: 0 is -inf, 1..100 run -74..0 dB, 100..127
// run 0..+6 dB. Pad volume and kit volume both use it.
constexpr float level_floor_db = -74.0f, level_top_db = 6.0f;
inline float volume_db(int level) {
    if (level <= 0) return -INFINITY;
    if (level <= 100) return 37.0f * std::log10(float(level) / 100.0f); // 1 is -74 dB
    return level_top_db * float(std::min(level, 127) - 100) / 27.0f;
}
inline float volume_gain(int level) { return level <= 0 ? 0.0f : std::pow(10.0f, volume_db(level) / 20.0f); }

// Pad layers (roadmap P3): up to four samples on a pad. Layer 1 is the pad's
// own sample and region; layers 2-4 are each a region of another sample with
// its tune, level and pan over the pad's. A hit plays every layer its velocity
// reaches (Velocity, the MPC's default), the next in turn (Cycle) or one at
// random (Random), as the MPC's layers play. Every other value of the pad
// (envelope, filter, play, loop, warp) is the pad's and goes over each.
constexpr unsigned pad_layers = 4;
enum class LayerPlay : uint8_t { Velocity, Cycle, Random, Count };
inline const char* layer_play_name(LayerPlay l) {
    static const char* const names[] {"VELOCITY", "CYCLE", "RANDOM"};
    return names[unsigned(l) % unsigned(LayerPlay::Count)];
}
struct PadLayer {
    uint16_t sample = no_sample;  // index into Project::samples
    uint32_t start = 0, end = 0;  // 44.1 kHz frames; end 0 means the sample's end
    int8_t pitch = 0, fine = 0;   // semitones and cents over the pad's
    int16_t gain = 0;             // hundredths of a dB over the pad's
    int8_t pan = 0;               // over the pad's, -50..50
    uint8_t low = 1, high = 127;  // the velocities it plays under Velocity
    bool used() const { return sample != no_sample; }
};

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
    bool mono = true;             // a new hit stops the pad's last one at the same pitch, so 16 Levels chords sound (else Poly: they overlap)
    uint8_t link = 0;             // Pad Link: 1..16 a pad of the same bank played with it, 0 none (it links on: roadmap P3)
    uint8_t offset = 0;           // 0..100: the start waits up to a sixteenth after the hit
    uint8_t velocity_sense = 100; // 0..100: 0 every hit full, 100 soft hits quietest
    bool decay_from_start = false; // One shot: decay from the start (ADSR), else over the decay before the end (MPC's default)
    PadFilter filter = PadFilter::Off;
    uint8_t cutoff = 100, resonance = 0;                                           // 0..100
    uint8_t filter_attack = 0, filter_decay = 50, filter_sustain = 0, filter_release = 20, filter_amount = 0; // 0..100
    uint8_t warp = warp_off;      // warp_off, warp_seq, or 50..200 %
    uint8_t beats = 0;            // # Beats for Warp's Seq, 0 from its length
    int16_t gain = 0;             // hundredths of a dB: Normalize (Shift + B2 on Trim)
    // Roadmap P3: layers 2-4 (the file's LAYR section), layer 1's velocities, and how a hit picks.
    std::array<PadLayer, pad_layers - 1> layers {};
    uint8_t low_velocity = 1, high_velocity = 127;
    LayerPlay layer_play = LayerPlay::Velocity;
    // Roadmap P3: a loop's seam crossfaded over this many milliseconds (0 none), the same time at any rate (PXFD).
    uint8_t loop_xfade = 0;
    uint32_t loop_point() const { return loop_lock ? start : loop_start; }
    bool used() const { return sample != no_sample; }
    bool warped() const { return warp == warp_seq || warp >= warp_min; }
    // Layer n (0-3) as a PadLayer: 0 is the pad's own sample and region, at no offset.
    PadLayer layer(unsigned n) const {
        if (n) return layers[(n - 1) % (pad_layers - 1)];
        PadLayer own;
        own.sample = sample; own.start = start; own.end = end; own.low = low_velocity; own.high = high_velocity;
        return own;
    }
    bool layered() const {
        for (const auto& l : layers) if (l.used()) return true;
        return false;
    }
    // Whether any layer plays sample slot `slot`.
    bool plays(uint16_t slot) const {
        if (sample == slot) return true;
        for (const auto& l : layers) if (l.used() && l.sample == slot) return true;
        return false;
    }
    // Whether the layer and play values are as a new pad has them (LAYR need not hold this pad).
    bool plain_layers() const { return !layered() && low_velocity == 1 && high_velocity == 127 && layer_play == LayerPlay::Velocity; }
};
// Pad `sound` as layer n of it plays: that layer's sample and region, its offsets over the pad's.
inline PadSound layer_sound(const PadSound& sound, unsigned n) {
    if (!n) return sound;
    const PadLayer& l = sound.layers[(n - 1) % (pad_layers - 1)];
    PadSound one = sound;
    one.sample = l.sample; one.start = l.start; one.end = l.end;
    one.pitch = int8_t(std::clamp(sound.pitch + l.pitch, -48, 48));
    one.fine = int8_t(std::clamp(sound.fine + l.fine, -99, 99));
    one.gain = int16_t(std::clamp(sound.gain + l.gain, -9600, 6000));
    one.pan = int8_t(std::clamp(sound.pan + l.pan, -50, 50));
    one.loop_lock = true; // a layer loops from its own start
    one.layers = {};
    return one;
}

// Chop's slice [start, end) of `parent`'s sample as a pad of its own: the parent pad's values (tune, Warp and the
// tempo it was played at, mute group, play, envelope, filter, level, pan), with what belongs to the parent's region
// alone left out (its layers, Pad Link, a loop point inside it, # Beats).
inline PadSound slice_sound(const PadSound& parent, uint32_t start, uint32_t end) {
    PadSound one = parent;
    one.start = start; one.end = end;
    one.layers = {};
    one.low_velocity = 1; one.high_velocity = 127;
    one.layer_play = LayerPlay::Velocity;
    one.link = 0;
    one.loop_lock = true; one.loop_start = 0;
    one.beats = 0;
    return one;
}

// Round 3 "instrument C": one zone of the instrument on a kit pad, tuned or turned up or down from its file's own
// (Stage's ZONES, K2 and K3), so one sample out of tune or too loud is put right without touching the file. A track
// keeps up to max_zone_trims (the file's ZTRM section); pad 0xff is a free one. `slot` is the instrument it was made
// on: once the pad holds another, the trim is not heard (and is dropped at the next trim or save).
constexpr unsigned max_zone_trims = 64;
constexpr int zone_trim_cents = 100;                  // TUNE: -100..100 cents
constexpr int zone_trim_low = -240, zone_trim_high = 120; // LEVEL: tenths of a dB
struct ZoneTrim {
    uint8_t pad = 0xff;  // kit pad 0..127
    uint8_t slot = 0;    // the project's sample slot holding the instrument
    uint16_t zone = 0;   // the zone's place in its instrument
    int8_t tune = 0;     // cents
    int16_t level = 0;   // tenths of a dB
    bool used() const { return pad != 0xff; }
};
using ZoneTrims = std::array<ZoneTrim, max_zone_trims>;
inline const ZoneTrim* find_trim(const ZoneTrims& trims, unsigned pad, unsigned zone, unsigned slot) {
    for (const auto& t : trims) if (t.used() && t.pad == pad && t.zone == zone && t.slot == slot) return &t;
    return nullptr;
}
// Zone `zone` of pad `pad`'s instrument (sample slot `slot`) at `tune` and `level`: a trim of none is taken away;
// false when every trim is in use.
inline bool set_trim(ZoneTrims& trims, unsigned pad, unsigned zone, unsigned slot, int tune, int level) {
    ZoneTrim* free = nullptr;
    for (auto& t : trims) {
        if (t.used() && t.pad == pad && t.zone == zone) { free = &t; break; }
        if (!t.used() && !free) free = &t;
    }
    const bool none = !tune && !level;
    if (!free) return none;
    if (none) { if (free->used()) *free = ZoneTrim {}; return true; }
    free->pad = uint8_t(pad); free->zone = uint16_t(zone); free->slot = uint8_t(slot);
    free->tune = int8_t(std::clamp(tune, -zone_trim_cents, zone_trim_cents));
    free->level = int16_t(std::clamp(level, zone_trim_low, zone_trim_high));
    return true;
}
inline void clear_trims(ZoneTrims& trims, unsigned pad) { for (auto& t : trims) if (t.used() && t.pad == pad) t = ZoneTrim {}; }

struct SampleRef {
    char path[160] {};  // on the card; empty: free
    char name[28] {};
    uint32_t frames = 0; // at 44.1 kHz, once loaded
    uint16_t zones = 0;  // a multisampled instrument's zones, once read (round 3 "instrument C": Stage's ZONES walks them)
    bool used() const { return path[0] != 0; }
    // A multisampled instrument, by what it is called: an .sfz, an MPC keygroup
    // program (.xpm, the MPC 3's .xty), a Logic EXS24 instrument (.exs), one
    // pad of an MPC drum program or project, "<file>#<pad>", an .sf2's preset
    // "<file>.sf2#<preset>" or a DecentSampler preset (.dspreset; roadmap P3).
    bool instrument() const {
        const char* end = path + std::strlen(path);
        const char* mark = std::strrchr(path, '#');
        const bool pad = mark && mark + 1 < end;
        if (pad) end = mark;
        if (!pad && dspreset()) return true;
        if (end - path < 4 || end[-4] != '.') return false;
        const char kind[3] = {char(end[-3] | 32), char(end[-2] | 32), char(end[-1] | 32)};
        const bool xpm = kind[0] == 'x' && kind[1] == 'p' && kind[2] == 'm';
        const bool xty = kind[0] == 'x' && kind[1] == 't' && kind[2] == 'y';
        const bool sf2 = kind[0] == 's' && kind[1] == 'f' && kind[2] == '2';
        if (pad) return xpm || xty || sf2 || (kind[0] == 'x' && kind[1] == 'p' && kind[2] == 'j'); // sf2: "<file>.sf2#<preset>"
        const bool exs = kind[0] == 'e' && kind[1] == 'x' && kind[2] == 's';
        return (kind[0] == 's' && kind[1] == 'f' && kind[2] == 'z') || xpm || xty || exs;
    }
    // A DecentSampler preset (roadmap P3).
    bool dspreset() const {
        const size_t n = std::strlen(path);
        static const char tail[] = ".dspreset";
        if (n < sizeof(tail) - 1) return false;
        for (size_t k = 0; k < sizeof(tail) - 1; ++k) if (char(path[n - (sizeof(tail) - 1) + k] | 32) != tail[k]) return false;
        return true;
    }
};

// A note of a pad, with the semitones it plays at from the pad's root over the key.
constexpr int pad_note(int key, int semitones) { return semitones ? key | (int(uint8_t(int8_t(semitones))) << 8) : key; }
// Roadmap P3: the same with 16 Levels' cutoff (0..100) above them, as the sampler's note_on takes it from a pattern
// (a cutoff + 1 in bits 16-23; -1 none). Note offs need only the key and semitones.
constexpr int pad_note(int key, int semitones, int cutoff) { return pad_note(key, semitones) | (cutoff >= 0 ? (cutoff + 1) << 16 : 0); }

// The pad's label as the boards print it: A01 .. D16.
inline void pad_label(unsigned pad, char out[4]) {
    out[0] = char('A' + (pad / 16) % sampler_banks);
    out[1] = char('0' + (pad % 16 + 1) / 10);
    out[2] = char('0' + (pad % 16 + 1) % 10);
    out[3] = 0;
}

} // namespace melodee
