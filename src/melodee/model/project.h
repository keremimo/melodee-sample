// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include <array>
#include <cstdint>
#include <cstring>

// A Melodee project as plain data: sixteen tracks, each with an engine and
// its sound, a mix strip and sixteen patterns. No threads, no I/O; the shell
// owns one copy on the MIDI thread and the engine host mirrors the patterns.
namespace melodee {

// The engines a track can load. Only those with `available` set in their
// EngineInfo can be picked yet.
enum class EngineKind : uint8_t { None, Prophet, Fm6, Cz, Sid, Kit909, Sampler, Count };

struct Note {
    uint8_t key = 0, velocity = 0; // velocity 0: no note
};
enum StepFlag : uint8_t { step_accent = 1, step_slide = 2, step_tie = 4 };
struct Step {
    std::array<Note, notes_per_step> notes {};
    uint8_t length = 1;    // in steps, 1..max_steps
    uint8_t chance = 100;  // percent
    uint8_t flags = 0;
    bool empty() const { for (auto n : notes) if (n.velocity) return false; return true; }
    const Note* first() const { for (auto& n : notes) if (n.velocity) return &n; return nullptr; }
};
struct Pattern {
    uint16_t length = 16; // steps, 1..max_steps
    std::array<Step, max_steps> steps {};
    bool empty() const { for (unsigned s = 0; s < length; ++s) if (!steps[s].empty()) return false; return true; }
};
// Every pattern of every track: big, so it lives on the heap.
struct PatternBank {
    std::array<std::array<Pattern, pattern_count>, track_count> at;
};

struct ModRoute {
    uint8_t source = 0, destination = 0; // 0: none
    int8_t amount = 0;                   // -64..63
};

struct Mix {
    uint8_t level = 100; // 0..127, 100 is unity
    int8_t pan = 0;      // -50 (left) .. 50 (right)
    uint8_t send = 0;    // 0..100, into the bus picked on Sends
    bool mute = false, solo = false;
};

// A pot move recorded over a pattern: from `step` on, `param` holds `value`
// on that track while that pattern plays.
struct MotionEvent {
    uint8_t track = 0, pattern = 0;
    uint16_t step = 0;
    uint8_t param = 0;
    int16_t value = 0;
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
            if (o.track == e.track && o.pattern == e.pattern && o.step == e.step && o.param == e.param) { o.value = e.value; return true; }
        }
        if (count == events.size()) return false;
        events[count++] = e;
        return true;
    }
    // Removes one lane: a parameter's events on a track's pattern.
    void clear(uint8_t track, uint8_t pattern, uint8_t param) {
        unsigned w = 0;
        for (unsigned n = 0; n < count; ++n) {
            const auto& o = events[n];
            if (!(o.track == track && o.pattern == pattern && o.param == param)) events[w++] = o;
        }
        count = uint16_t(w);
    }
    bool has(uint8_t track, uint8_t pattern, uint8_t param) const {
        for (unsigned n = 0; n < count; ++n)
            if (events[n].track == track && events[n].pattern == pattern && events[n].param == param) return true;
        return false;
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
    bool used() const { return engine != EngineKind::None; }
    void set_name(const char* text) { std::strncpy(name, text, name_length - 1); name[name_length - 1] = 0; }
};

struct Project {
    char name[32] = "Untitled";
    uint16_t tempo = 1200;  // tenths of a BPM
    uint8_t swing = 50;     // percent, 50 is straight
    uint8_t root = 2;       // 0 C .. 11 B
    uint8_t scale = 0;      // index into scales()
    std::array<Track, track_count> tracks {};
    Motion motion;
    unsigned used_tracks() const { unsigned n = 0; for (auto& t : tracks) n += t.used(); return n; }
};

} // namespace melodee
