// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <array>
#include <cstdint>
#include <string>

// What the user interface knows about an engine without touching its DSP:
// parameters with ranges and formats, the Edit pages they sit on, the three
// favourites for Stage, and the factory sounds.
namespace melodee {

enum class Format : uint8_t {
    Number,    // plain integer
    Percent,   // 0..100 "%"
    Semitones, // signed, "st"
    Cents,     // signed, "ct"
    Time,      // 0..100 on an exponential 1 ms .. 10 s scale
    Rate,      // 0..100 on an exponential 0.05 .. 30 Hz scale
    Switch,    // Off / On
    Choice,    // one of `choices`
};

struct ParamInfo {
    const char* label; // capitals, as on the knob card
    int16_t min, max, initial;
    Format format = Format::Percent;
    const char* const* choices = nullptr;
};

// What the picture on an Edit page draws.
enum class Picture : uint8_t { Bars, Wave, WaveB, Mixer, Filter, Envelope, Lfo, Mod };

struct EditPage {
    const char* title; // as the page and the page map name it
    const char* key;   // as a soft key names it: "FILTER ENV"
    Picture picture;
    std::array<std::array<int8_t, 3>, 3> rows; // parameter ids, -1 empty; rows of three
    unsigned row_count() const { unsigned n = 0; for (auto& r : rows) if (r[0] >= 0 || r[1] >= 0 || r[2] >= 0) ++n; return n; }
};

struct Sound {
    const char* name;
    // (id, value) pairs over the initial values, ended by id -1.
    std::array<std::array<int16_t, 2>, 40> changes;
};

struct EngineInfo {
    EngineKind kind;
    const char* name; // capitals, as in the track tag
    bool available;   // built yet
    const ParamInfo* params = nullptr;
    unsigned param_count = 0;
    const EditPage* pages = nullptr;
    unsigned page_count = 0;
    std::array<int8_t, 3> favourites {-1, -1, -1};
    const Sound* sounds = nullptr;
    unsigned sound_count = 0;
};

const EngineInfo& engine_info(EngineKind kind);
// Edit's pages for an engine: its own, then the mod matrix, which every
// engine shares ("a page like any other").
unsigned edit_page_count(const EngineInfo& info);
const EditPage& edit_page_at(const EngineInfo& info, unsigned index);
bool is_mod_page(const EngineInfo& info, unsigned index);
// The engines that can be loaded now, in the order K1 steps through them.
unsigned available_engines(std::array<EngineKind, size_t(EngineKind::Count)>& out);

// A sound's full parameter values.
void sound_values(const EngineInfo& info, unsigned sound, std::array<int16_t, max_params>& out);
// Loads an engine and one of its sounds onto a track (model only).
void load_sound(Track& track, EngineKind kind, unsigned sound);

// The value as a knob card shows it: the number and its unit.
struct Shown { std::string value, unit; };
Shown show(const ParamInfo& p, int value);
// The value's place along the gauge, 0..1, and whether it is drawn from the centre.
float gauge_of(const ParamInfo& p, int value);
bool bipolar(const ParamInfo& p);

// Exponential scales shared by the UI and the DSP.
float time_seconds(int value); // Format::Time
float rate_hertz(int value);   // Format::Rate

} // namespace melodee
