// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/compressor.h"
#include "melodee/model/knob_fx.h"
#include "melodee/model/limits.h"
#include "melodee/model/pad_fx.h"
#include "melodee/model/pattern.h"
#include "melodee/model/sampler.h"
#include <algorithm>
#include <array>
#include <iterator>
#include <cstdint>
#include <cstring>

// A Melodee project as plain data: sixteen tracks, each with an engine and
// its sound, a mix strip and sixteen patterns. No threads, no I/O; the shell
// owns one copy on the MIDI thread and the engine host mirrors the patterns.
namespace melodee {

// The engines a track can load. Only those with `available` set in their
// EngineInfo can be picked yet.
enum class EngineKind : uint8_t { None, Prophet, Fm6, Cz, Sid, Kit909, Sampler, Count };

// Every pattern of every track (their events on the heap).
struct PatternBank {
    std::array<std::array<Pattern, pattern_count>, track_count> at;
};

struct ModRoute {
    uint8_t source = 0, destination = 0; // ModSource, ModDest (model/mod_matrix.h); 0: none
    int8_t amount = 0;                   // -100..100
    bool used() const { return source && destination; }
};

// One effect in a chain: its kind (EffectKind; 0 none) and its six values.
struct EffectSlot {
    uint8_t kind = 0;
    bool bypass = false;
    std::array<int16_t, 6> params {};
    bool used() const { return kind != 0; }
};
// A chain's slots stay packed from the front; `size` of them may be used.
struct Chain {
    std::array<EffectSlot, max_slots> slots {};
    uint8_t size = max_slots;
    unsigned count() const { unsigned n = 0; while (n < size && slots[n].used()) ++n; return n; }
};
struct Bus {
    Chain fx;
    uint8_t level = 70; // the return, 0..100
};
// Chains are numbered for commands: tracks 0-15, buses 16-19, master 20.
constexpr unsigned bus_chain(unsigned bus) { return track_count + bus; }
constexpr unsigned master_chain = track_count + bus_count;
// Each sampler pad's own chain follows: pad_chain(track, pad).
constexpr unsigned pad_chain_base = chain_count;
constexpr unsigned pad_chain(unsigned track, unsigned pad) { return pad_chain_base + track * sampler_pads + pad; }
// The kit's pad chains, four slots each.
struct PadChains {
    std::array<Chain, sampler_pads> at;
    PadChains() { for (auto& c : at) c.size = pad_slots; }
};

struct Mix {
    uint8_t level = 100; // 0..127, 100 is unity
    int8_t pan = 0;      // -50 (left) .. 50 (right)
    uint8_t send = 0;    // 0..100, into `bus`
    uint8_t bus = 0;     // 0..3: A-D
    bool mute = false, solo = false;
};

// A pot move recorded over a pattern: from `step` on (a sixteenth of the
// pattern, whatever its Q), `param` holds `value` on that track while that
// pattern plays. A kit pad's own values (param from motion_pad_base on,
// model/pad_params.h) are a lane per pad: `pad` says which (0 otherwise).
struct MotionEvent {
    uint8_t track = 0, pattern = 0;
    uint16_t step = 0; // sixteenths from the pattern's start
    uint8_t param = 0;
    int16_t value = 0;
    uint8_t pad = 0;   // a pad lane's kit pad, 0..127
    bool same_lane(uint8_t t, uint8_t p, uint8_t q, uint8_t k) const { return track == t && pattern == p && param == q && pad == k; }
};
// Every motion event of the project, at most motion_events (open question 2:
// per project, as the FM-1 counts them). Both the shell and the engine host
// change it only through write() and clear(), so their copies stay equal.
struct Motion {
    std::array<MotionEvent, motion_events> events {};
    uint16_t count = 0;
    // Replaces the event at the same place, or adds one; false when full.
    bool write(const MotionEvent& e) {
        for (unsigned n = 0; n < count; ++n) {
            auto& o = events[n];
            if (o.same_lane(e.track, e.pattern, e.param, e.pad) && o.step == e.step) { o.value = e.value; return true; }
        }
        if (count == events.size()) return false;
        events[count++] = e;
        return true;
    }
    // Removes one lane: a parameter's events on a track's pattern (a pad lane's on that pad).
    void clear(uint8_t track, uint8_t pattern, uint8_t param, uint8_t pad = 0) {
        unsigned w = 0;
        for (unsigned n = 0; n < count; ++n) {
            const auto& o = events[n];
            if (!o.same_lane(track, pattern, param, pad)) events[w++] = o;
        }
        count = uint16_t(w);
    }
    bool has(uint8_t track, uint8_t pattern, uint8_t param, uint8_t pad = 0) const {
        for (unsigned n = 0; n < count; ++n)
            if (events[n].same_lane(track, pattern, param, pad)) return true;
        return false;
    }
};

// The arrange view: clips of patterns on track lanes over bars, named
// markers, and a loop brace.
struct Clip {
    uint8_t track = 0, pattern = 0;
    uint16_t bar = 0, bars = 1; // from `bar`, for `bars` bars; the pattern repeats inside
    bool muted = false;
    uint16_t end() const { return uint16_t(bar + bars); }
};
struct Marker {
    uint16_t bar = 0;
    char name[12] {};
};
// A punch written into the song: where (a bar and its sixteenth), for how
// many sixteenths, which punch (PunchKind), on what (255 the master, else a
// track) and how deep.
struct SongPunch {
    uint16_t bar = 0;
    uint8_t step = 0, length = 1, kind = 0, target = 255, depth = 100;
};
struct Song {
    std::array<Clip, song_clips> clips {};
    uint16_t count = 0;
    std::array<Marker, song_markers> markers {};
    uint8_t marker_count = 0;
    uint16_t loop_start = 0, loop_end = 0; // bars; no loop when equal
    std::array<SongPunch, song_punches> punches {};
    uint16_t punch_count = 0;
    bool add_punch(const SongPunch& p) { if (punch_count == punches.size()) return false; punches[punch_count++] = p; return true; }
    int clip_at(unsigned track, unsigned bar) const {
        for (unsigned n = 0; n < count; ++n)
            if (clips[n].track == track && bar >= clips[n].bar && bar < clips[n].end()) return int(n);
        return -1;
    }
    // Adds a clip, first cutting away whatever it covers on its lane; false when full.
    bool place(const Clip& c) {
        cut(c.track, c.bar, c.end());
        if (count == clips.size()) return false;
        clips[count++] = c;
        return true;
    }
    void erase(unsigned index) {
        if (index >= count) return;
        for (unsigned n = index + 1; n < count; ++n) clips[n - 1] = clips[n];
        --count;
    }
    // Clears bars [from, to) on a lane, shortening or splitting the clips there.
    void cut(unsigned track, unsigned from, unsigned to) {
        for (unsigned n = 0; n < count;) {
            Clip c = clips[n];
            if (c.track != track || c.end() <= from || c.bar >= to) { ++n; continue; }
            erase(n);
            if (c.bar < from) { Clip head = c; head.bars = uint16_t(from - c.bar); clips[count++] = head; }
            if (c.end() > to && count < clips.size()) { Clip tail = c; tail.bar = uint16_t(to); tail.bars = uint16_t(c.end() - to); clips[count++] = tail; }
        }
    }
    unsigned length() const { unsigned end = 0; for (unsigned n = 0; n < count; ++n) end = std::max<unsigned>(end, clips[n].end()); return end; }
    bool looping() const { return loop_end > loop_start; }

    // Markers: names come from a fixed list until there is a way to type
    // one (open question 9). step_marker() adds one at `bar`, or moves an
    // existing one to the next name; after the last name it goes.
    static constexpr const char* marker_names[] {"INTRO", "VERSE", "CHORUS", "BRIDGE", "DROP", "BREAK", "OUTRO"};
    static constexpr unsigned marker_name_count = sizeof(marker_names) / sizeof(marker_names[0]);
    int marker_at(unsigned bar) const {
        for (unsigned m = 0; m < marker_count; ++m) if (markers[m].bar == bar) return int(m);
        return -1;
    }
    bool step_marker(unsigned bar) {
        auto set = [](Marker& m, unsigned name) {
            std::fill(std::begin(m.name), std::end(m.name), '\0');
            for (unsigned k = 0; k + 1 < sizeof(m.name) && marker_names[name][k]; ++k) m.name[k] = marker_names[name][k];
        };
        auto name_of = [](const Marker& m) {
            for (unsigned n = 0; n < marker_name_count; ++n) {
                const char* a = marker_names[n]; const char* b = m.name;
                while (*a && *a == *b) { ++a; ++b; }
                if (!*a && !*b) return int(n);
            }
            return -1;
        };
        int at = marker_at(bar);
        if (at >= 0) {
            int next = name_of(markers[size_t(at)]) + 1;
            if (next >= int(marker_name_count)) { // past the last name: the marker goes
                for (unsigned m = unsigned(at) + 1; m < marker_count; ++m) markers[m - 1] = markers[m];
                --marker_count;
            } else {
                set(markers[size_t(at)], unsigned(next));
            }
            return true;
        }
        if (marker_count == markers.size()) return false;
        // A new marker takes the name after the marker before it, or the first.
        int before = -1;
        for (unsigned m = 0; m < marker_count; ++m)
            if (markers[m].bar < bar && (before < 0 || markers[m].bar > markers[size_t(before)].bar)) before = int(m);
        int name = before >= 0 ? name_of(markers[size_t(before)]) + 1 : 0;
        Marker added;
        added.bar = uint16_t(bar);
        set(added, unsigned(std::clamp(name, 0, int(marker_name_count) - 1)));
        markers[marker_count++] = added;
        return true;
    }
};

enum class PadLayout : uint8_t { Scale, Chromatic, Chords, Levels, Count };

struct Track {
    EngineKind engine = EngineKind::None;
    uint16_t preset = 0;
    char name[name_length] {};
    std::array<int16_t, max_params> params {};
    Mix mix;
    uint8_t pattern = 0;  // the pattern playing
    PadLayout layout = PadLayout::Scale;
    int8_t octave = 2;    // of the lowest pad, C4 = MIDI 60
    uint8_t last_note = 50;
    std::array<ModRoute, mod_routes> mods {};
    Chain fx {{}, track_slots};
    PadChains pad_fx;     // a sampler track: each pad's chain (PadFX)
    int8_t transpose = 0; // Shift + Minus / Plus: the pads moved by semitones, -12..12
    // A sampler track's kit (unused by the synth engines).
    std::array<PadSound, sampler_pads> pads {};
    uint8_t bank = 0;     // the bank the pads play, 0..7: A-H
    uint8_t last_pad = 0; // the kit pad hit last (0..127): what the pots and Trim edit
    // Mute (mode) on a kit page: kit pads muted, a bit each. A muted pad is not
    // heard, played or sequenced, and keeps its notes.
    std::array<uint64_t, (sampler_pads + 63) / 64> pad_mutes {};
    bool pad_muted(unsigned pad) const { pad %= sampler_pads; return (pad_mutes[pad / 64] >> (pad % 64)) & 1; }
    void set_pad_muted(unsigned pad, bool on) {
        pad %= sampler_pads;
        uint64_t bit = uint64_t(1) << (pad % 64);
        pad_mutes[pad / 64] = on ? pad_mutes[pad / 64] | bit : pad_mutes[pad / 64] & ~bit;
    }
    bool any_pad_muted() const { for (auto m : pad_mutes) if (m) return true; return false; }
    // 16 Levels on a sampler track: one kit pad on all sixteen pads.
    bool levels = false;
    uint8_t levels_pad = 0;
    uint8_t levels_kind = 0; // 0 velocity (soft to loud), 1 up the project's scale, 2 by semitones
    // Chord: one pad, one chord from the scale (up to four notes).
    bool chord = false, chord_hold = false;
    uint8_t chord_type = 1, chord_voicing = 0, chord_invert = 0;
    // Arp (note repeat on a sampler track): held notes in turn at a rate.
    bool arp = false, arp_latch = false;
    uint8_t arp_mode = 0, arp_rate = 4, arp_octaves = 1, arp_gate = 60; // arp_rate: engine/arp.h, 4 is 1/16
    bool used() const { return engine != EngineKind::None; }
    // The note a new step gets on Steps: on a kit the pad hit last, else the last note played.
    uint8_t step_note() const { return engine == EngineKind::Sampler ? uint8_t(sampler_first_note + last_pad % sampler_pads) : last_note; }
    void set_name(const char* text) {
        unsigned n = 0;
        for (; n + 1 < name_length && text[n]; ++n) name[n] = text[n];
        std::fill(name + n, name + name_length, '\0');
    }
};

// Time signatures the project can take (Shift + K2 on Patterns), as on the
// MPC: every pattern shares it.
struct TimeSignature { uint8_t beats = 4, unit = 4; };
constexpr TimeSignature time_signatures[] {
    {1, 4}, {2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 4}, {7, 4}, {8, 4}, {9, 4},
    {3, 8}, {5, 8}, {6, 8}, {7, 8}, {9, 8}, {11, 8}, {12, 8}, {13, 8}, {15, 8},
    {5, 16}, {7, 16}, {9, 16}, {11, 16}, {13, 16}, {15, 16},
};
constexpr unsigned time_signature_count = sizeof(time_signatures) / sizeof(time_signatures[0]);
constexpr unsigned common_time = 3; // 4/4
constexpr unsigned bar_ticks_of(TimeSignature t) { return t.beats * ticks_per_quarter * 4 / t.unit; }

struct Project {
    char name[32] = "Untitled";
    uint16_t tempo = 1200;  // tenths of a BPM; the global tempo
    // BPM per pattern (B2 on Patterns): with seq_tempo on, launching pattern n
    // (any track, or the column) plays at pattern_tempo[n]; 0 is not set yet,
    // which plays at the global tempo.
    bool seq_tempo = false;
    std::array<uint16_t, pattern_count> pattern_tempo {};
    uint8_t signature = common_time; // index into time_signatures
    uint8_t swing = 50;     // percent, 50 is straight
    uint8_t root = 2;       // 0 C .. 11 B
    uint8_t scale = 0;      // index into scales()
    uint8_t click = 0;      // the metronome: 0 off, 1 on, 2 only while Seq Record (Shift + Tap Tempo)
    uint8_t click_level = 40;
    // Punch (Pad FX): what the punches hit (0 the master, 1 the selected
    // track), the bank in view, and K1-K3: depth, snap grid (0 1/4 .. 3 1/32), tail (cut, fade, ring).
    // punch_bank: 0 and 1 Melodee's own banks, 2 the MPC's sixteen (parity phase 7b; new projects start there).
    uint8_t punch_target = 0, punch_bank = 2, punch_depth = 100, punch_snap = 2, punch_tail = 0;
    PadFxParams pad_fx_params = pad_fx_defaults; // the MPC's Pad FX, K1-K3 each (parity phase 7b)
    FlexSettings flex;                           // Flex Beat (Shift + Pad FX)
    std::array<Track, track_count> tracks {};
    KnobFx knob_fx;         // Knob FX (parity phase 7)
    MasterComp compressor;  // Shift + Pad 5 (parity phase 7)
    Motion motion;
    Song song;
    std::array<Bus, bus_count> buses {};
    std::array<SampleRef, sample_slots> samples {};
    Chain master {{}, master_slots};
    Chain& chain(unsigned n) {
        if (n >= pad_chain_base) { n -= pad_chain_base; return tracks[(n / sampler_pads) % track_count].pad_fx.at[n % sampler_pads]; }
        return n < track_count ? tracks[n].fx : n < master_chain ? buses[n - track_count].fx : master;
    }
    const Chain& chain(unsigned n) const { return const_cast<Project*>(this)->chain(n); }
    unsigned used_tracks() const { unsigned n = 0; for (auto& t : tracks) n += t.used(); return n; }
    TimeSignature time_signature() const { return time_signatures[signature % time_signature_count]; }
    unsigned bar_ticks() const { return bar_ticks_of(time_signature()); }
    unsigned beat_ticks() const { return ticks_per_quarter * 4 / time_signature().unit; }
    uint32_t max_length() const { return max_bars * bar_ticks(); }
    // The tempo pattern n plays at (tenths of a BPM).
    unsigned tempo_of(unsigned pattern) const {
        unsigned own = pattern_tempo[pattern % pattern_count];
        return seq_tempo && own ? own : tempo;
    }
};

} // namespace melodee
