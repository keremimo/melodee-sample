// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/compressor.h"
#include "melodee/model/knob_fx.h"
#include "melodee/model/limits.h"
#include "melodee/model/looper.h"
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
// Midi plays an external instrument (docs/MIDI_TRACKS.md); files keep it in
// MIDT, TRAK says None, so older builds load the track empty.
// The numbers are what project files keep. 1-5 were the built-in synths and
// drum kit (PROPHET, FM6, CZ, SID, the 909), gone since the synths became
// plug-ins of their own: a file's track of one opens as that plug-in or empty
// (legacy_engine below, model/project_file.cpp).
enum class EngineKind : uint8_t { None = 0, Sampler = 6, Midi = 7, Plugin = 8, Count = 9 };

// What TRAK's engine number meant before the synths left Melodee. PROPHET,
// CZ and SID tracks open as PLUGIN tracks on Melodee Poly, PD and Chip (the
// same voices, now Open MPC plug-ins of their own), their values as the
// plug-in's state; FM6 and 909 tracks open empty, their patterns kept.
namespace legacy_engine {
constexpr uint8_t prophet = 1, fm6 = 2, cz = 3, sid = 4, kit909 = 5;
}

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
// Chains are numbered for commands: tracks 0-15, buses 16-19, master 20, the Looper's L1-L4 21-24.
constexpr unsigned bus_chain(unsigned bus) { return track_count + bus; }
constexpr unsigned master_chain = track_count + bus_count;
constexpr unsigned loop_chain(unsigned loop) { return master_chain + 1 + loop; }
constexpr bool is_loop_chain(unsigned n) { return n > master_chain && n < master_chain + 1 + loop_tracks; }
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
    uint8_t send = 0;    // a loop track's one send, 0..100, into `bus` (a track has one into each bus: `sends`)
    uint8_t bus = 0;     // 0..3: A-D
    bool mute = false, solo = false;
    std::array<uint8_t, 4> sends {}; // a track's send into each bus A-D, 0..100 (round 2: four sends a track)
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
    // The first marker after `bar`, or 0 (no marker can come after a bar and sit at bar 0).
    unsigned next_marker(unsigned bar) const {
        unsigned next = 0;
        for (unsigned m = 0; m < marker_count; ++m)
            if (markers[m].bar > bar && (!next || markers[m].bar < next)) next = markers[m].bar;
        return next;
    }
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

constexpr unsigned max_chain = 8; // a track's pattern chain (seq-sheet2 B), patterns in the order played
// A chained pattern in its last bar (`position` ticks into its `length`), when the next is queued for the bar line,
// so it starts as this one ends (a pattern of whole bars, begun on a bar).
inline bool chain_due(int position, uint32_t length, uint32_t bar) { return position >= 0 && length && uint32_t(position) + bar >= length; }

// Fader C (round 2): what the fader learned from the pot turned last on its
// track (model/fader.h). Kept in the file (FADR) by these numbers: a Motion
// lane (an engine parameter, an effect's on the track's chain, a kit pad's
// own value, the mod wheel: shell/bindings.h) or a value of the track's mix.
enum class FaderLearn : uint8_t { None, Lane, Level, Pan, Send };
struct FaderLearned {
    FaderLearn what = FaderLearn::None;
    uint16_t lane = 0;  // Lane: the Motion lane, a kit pad's value on pad 0 (it follows the last pad hit); Send: the bus
    uint8_t engine = 0; // the track's EngineKind as it learned: an engine parameter means nothing to another engine
    uint8_t pot = 0;    // the pot it came from, K1-K3 as 0..2 (the Fader page draws it in that pot's shade)
    bool learned() const { return what != FaderLearn::None; }
};

struct Track {
    EngineKind engine = EngineKind::None;
    uint16_t preset = 0;
    char name[name_length] {};
    std::array<int16_t, max_params> params {};
    Mix mix;
    uint8_t pattern = 0;  // the pattern playing
    PadLayout layout = PadLayout::Scale;
    int8_t octave = 2;    // of the lowest pad, C4 = MIDI 60; a kit: its 16 Levels in pitch, 2 as it is (-1..7)
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
    uint8_t levels_kind = 1; // LevelsKind (model/scales.h): Scale on a new track, as the most played (Kerem)
    // 16 Levels' two ends, pad 1's and pad 16's, for Velocity, Scale, Tune and Filter in LevelsKind's order
    // (velocity 1..127, scale steps, semitones, cutoff 0..100); the pads between go evenly from one to the other.
    // Scale's are kept for older files but unused: it goes up the key from its root.
    std::array<std::array<int8_t, 2>, 4> levels_ends {{{8, 127}, {0, 15}, {-3, 12}, {0, 100}}};
    // Chord: one pad, one chord from the scale (up to four notes).
    bool chord = false, chord_hold = false;
    uint8_t chord_type = 1, chord_voicing = 0, chord_invert = 0;
    // Arp (note repeat on a sampler track): held notes in turn at a rate.
    bool arp = false, arp_latch = false;
    uint8_t arp_mode = 0, arp_rate = 4, arp_octaves = 1, arp_gate = 60; // arp_rate: engine/arp.h, 4 is 1/16
    // The fader (Shift + Pad 9, parity phase 8): its target (model/fader.h; 255
    // until one is picked: the engine's default) and whether it has one at all.
    uint8_t fader_target = 255;
    bool fader_on = true;
    // seq-sheet2 B (round 2): a pattern chain, the patterns tapped in order while Seq is held: each plays once
    // through and the next follows at its end, round again (fewer than two: no chain). chain_at: the one playing.
    std::array<uint8_t, max_chain> chain {};
    uint8_t chain_count = 0, chain_at = 0;
    bool chained() const { return chain_count >= 2; }
    uint8_t chain_ahead(unsigned steps) const { return chain[(chain_at + steps) % std::max<unsigned>(chain_count, 1)]; } // 0: the one playing
    FaderLearned fader_learned; // fader C: the pot it follows, over the target above while there is one
    bool fader_pin = false;     // PIN: what it has stays, whatever pot turns next
    uint8_t wheel = 0;    // the mod wheel as the fader left it, 0..127 (played, not saved)
    uint8_t midi_channel = 0; // MIDI Config: 0 none, 1..16 notes on it play this track; its notes go out on it (parity phase 9)
    uint8_t plugin = no_plugin; // a PLUGIN track: the entry of Project::plugins it plays
    uint8_t speed = speed_normal; // its patterns' playback speed (roadmap P1), an index into speeds: x1/2 .. x2
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

// A plug-in a project uses (Project::plugins): where it is on the card. Its
// state (what the plug-in saves of itself) travels beside the project, in the
// file's PLUG section and the rack (plugin/plugin_rack.h), not in here.
struct PluginRef {
    char path[plugin_path_length] {}; // a .vst3 bundle, a VST2 .so, or an Open MPC plug-in folder; empty: free
    int32_t id = 0;                   // which plug-in of a bundle holding several (its JUCE unique id); 0: its first
    char name[40] {};                 // as the picker named it, for the cards while it is not open
    bool used() const { return path[0] != 0; }
};

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
    // Punch (Pad FX): what the punches hit (0 the master, 1 the selected
    // track), the bank in view, and K1-K3: depth, snap grid (0 1/4 .. 3 1/32), tail (cut, fade, ring).
    // punch_bank: 0 and 1 Melodee's own banks, 2 the MPC's sixteen (parity phase 7b; new projects start there).
    uint8_t punch_target = 0, punch_bank = 2, punch_depth = 100, punch_snap = 2, punch_tail = 0;
    PadFxParams pad_fx_params = pad_fx_defaults; // the MPC's Pad FX, K1-K3 each (parity phase 7b)
    FlexSettings flex;                           // Flex Beat (Shift + Pad FX)
    std::array<Track, track_count> tracks {};
    KnobFx knob_fx;         // Knob FX (parity phase 7)
    MasterComp compressor;  // Shift + Pad 5 (parity phase 7)
    Song song;              // (Motion is each pattern's own since roadmap P1: Pattern::motion)
    std::array<Bus, bus_count> buses {};
    std::array<SampleRef, sample_slots> samples {};
    std::array<PluginRef, max_plugins> plugins {};
    Chain master {{}, master_slots};
    // The Looper (docs/LOOPER.md): its settings, and its four tracks' strips and chains.
    LooperSettings looper;
    std::array<Mix, loop_tracks> loop_mix {};
    std::array<Chain, loop_tracks> loop_fx = [] { std::array<Chain, loop_tracks> c {}; for (auto& x : c) x.size = track_slots; return c; }();
    Chain& chain(unsigned n) {
        if (n >= pad_chain_base) { n -= pad_chain_base; return tracks[(n / sampler_pads) % track_count].pad_fx.at[n % sampler_pads]; }
        if (is_loop_chain(n)) return loop_fx[n - loop_chain(0)];
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

// Whether entry `entry` of Project::plugins is played by a PLUGIN track or sits in a chain (a sampler pad's own chain
// takes none: plugin effects go on tracks, buses, the master and the loop tracks).
inline bool plugin_used_by(const Project& project, unsigned entry) {
    for (const auto& t : project.tracks)
        if (t.engine == EngineKind::Plugin && t.plugin == entry) return true;
    for (unsigned n = 0; n < chain_count; ++n)
        for (const auto& s : project.chain(n).slots)
            if (s.kind == plugin_effect_kind && unsigned(s.params[0]) == entry) return true;
    return false;
}

} // namespace melodee
