// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/mod_matrix.h"
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
    Level,     // 0..127 shown in dB, as the MPC shows volume (model/sampler.h volume_db)
    Ms,        // milliseconds as they are
    Decibels,  // signed dB as they are
    Tenths,    // a tenth a step: 0.0 .. (Amp sim's DRIVE 0-11)
    Seconds,   // tenths of a second (Spring's TIME 1.0-10.0 s)
    Hertz,     // Hz as they are, kHz from 1000
    CentiHz,   // hundredths of a Hz (Pad FX's rates: 0.02-10.00 Hz)
    Bank,      // a MIDI bank select: 0 Off (not sent), 1..128 as 0..127
    Program,   // a MIDI program: 0 Off (not sent), 1..128 as numbered
    CcNumber,  // a MIDI controller number 0..127, "CC 74"
    MidiSlot,  // a MIDI track's slot address (engine/midi_engine.h): "CC 74", "CC14 1", "BEND", "PRESS", "NRPN 12"; -1 none
};

struct ParamInfo {
    const char* label; // capitals, as on the knob card
    int16_t min, max, initial;
    Format format = Format::Percent;
    const char* const* choices = nullptr;
};

// What the picture on an Edit page draws.
// Route: a MIDI track's notes leaving for its port (docs/MIDI_TRACKS.md).
// Pads: a MIDI track's Drum map, the bank's sixteen pads with their notes (roadmap P4, M3).
enum class Picture : uint8_t { Bars, Mod, Route, Pads };

struct EditPage {
    const char* title; // as the page and the page map name it
    const char* key;   // as a soft key names it: "FILTER ENV"
    Picture picture;
    std::array<std::array<int8_t, 3>, 3> rows; // parameter ids, -1 empty; rows of three
    unsigned row_count() const { unsigned n = 0; for (auto& r : rows) if (r[0] >= 0 || r[1] >= 0 || r[2] >= 0) ++n; return n; }
};

// Stage (pages/stage_page.cpp, docs/MELODEE.md): the selected track's quick
// pages. B1-B3 each hold a cycle of up to five pages (none: the key does
// nothing); pressing the lit button again steps to the next, another
// button jumps to the first of its own. Each engine states its pages here as
// data; StageKind says where a page's cards come from.
enum class StageKind : uint8_t {
    Params,     // engine parameters: `params` on K1-K3, `shift` on Shift + K1-K3
    Preset,     // the engine's sounds: K1 the sound (it loads once the turning stops), K2 the list, K3 the engine
    Mix,        // the track's level, pan and send A, the quick one
    Sends,      // the track's four sends: K1 the bus (A-D), K2 the send into it, K3 its return
    Arp,        // the track's arp: mode, rate, octaves; Shift + K1 gate, Shift + K3 hold
    Chord,      // one pad, one chord from the scale: type, voicing, inversion; Shift + K1 on or off, Shift + K3 hold
    Fx,         // a slot of the track's chain: its first row (Shift: the second); Push the next slot
    Trim, PadMix, AmpEnv, Tune, Play, Filter, FiltEnv, // a kit: the last pad's Sample Mode values
    Follow, Cloud, KitFilter,                          // a kit: Melodee's own kit-wide pages
    Layers,                                            // a kit: the last pad's four layers (roadmap P3)
    Colour,                                            // a kit: the last pad's colour (shell/pad_colour.h)
    PluginSound,       // a PLUGIN track's first page: its presets (K1-K3 its first row), or its face when it has none
    PluginEdit,        // a PLUGIN track's own parameters, three a row; the wheel moves the rows
    OpenEdit,          // a button that opens the Edit page instead of a Stage page (a MIDI track's B3: its every page)
    NewSynth, NewKit, NewMidi,                         // an empty track: what it becomes (Push)
};
// What the picture over the knobs draws.
enum class StagePicture : uint8_t {
    Bars,      // the cards' parameters as rows
    Preset,    // the sound before, the one loaded, the one after
    Filter,    // a filter curve: params[0] cutoff, params[1] resonance (0-100)
    Envelope,  // params[0..2] attack, decay, sustain; shift[0] release
    Lfo,       // params[0] rate, params[1] shape (Triangle, Saw, Square, Random), params[2] amount
    ShiftRows, // the Shift row as rows: what Shift + K1-K3 reach
    Mix, Sends, Arp, Chord, Fx, Wave, Route, Program, Cc, Engines, Sources, Ports,
    PluginSound, PluginEdit,
    Colours,   // a kit's bank as its pads sit, each in its colour, the last pad outlined
};
struct StagePage {
    const char* key = ""; // as the soft key names it: "AMP ENV"
    StageKind kind {};
    StagePicture picture {};
    std::array<int8_t, 3> params {{-1, -1, -1}};
    std::array<int8_t, 3> shift {{-1, -1, -1}};
};
struct StageGroup {
    std::array<StagePage, 5> pages;
    uint8_t count;
};
struct StageGroups {
    std::array<StageGroup, 3> groups; // B1, B2, B3
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
    // The mod matrix's destinations as this engine names them, one per
    // ModDest; nullptr where the engine has nothing there. Left out: every
    // destination under its common name.
    const char* const* mod_dests = nullptr;
    // How many parameters older project and sound files carry for this
    // engine (before phase 1d added pad expression): the ones after it load
    // at their initial values. 0: all of them.
    unsigned legacy_params = 0;
    // Edit ends with the mod matrix page (MIDI has nothing to modulate yet).
    bool mod_matrix = true;
    // Stage's pages for a track of this engine; nullptr: a synth's default.
    const StageGroups* stage = nullptr;
};

const EngineInfo& engine_info(EngineKind kind);
// Stage's pages for an engine (None: the empty track's chooser).
const StageGroups& stage_groups(EngineKind kind);
// Stage's shared pages. Every track gives B1-B3 the same jobs: B1 the
// source (a kit's sample, a plug-in, a MIDI device), B2 how it plays (a kit's
// play pages, ARP and CHORD), B3 where it goes (MIX, SENDS, then a kit's
// filters or a plug-in's FX).
constexpr StagePage stage_mix {"MIX", StageKind::Mix, StagePicture::Mix};
constexpr StagePage stage_sends {"SENDS", StageKind::Sends, StagePicture::Sends};
constexpr StagePage stage_arp {"ARP", StageKind::Arp, StagePicture::Arp};
constexpr StagePage stage_chord {"CHORD", StageKind::Chord, StagePicture::Chord};
constexpr StagePage stage_fx {"FX", StageKind::Fx, StagePicture::Fx};
// Edit's pages for an engine: its own, then the mod matrix, which every
// engine shares ("a page like any other").
unsigned edit_page_count(const EngineInfo& info);
const EditPage& edit_page_at(const EngineInfo& info, unsigned index);
bool is_mod_page(const EngineInfo& info, unsigned index);
// The mod matrix's destinations on an engine: its name for one (nullptr
// where it has none), and the ones it has, in order (None first).
const char* mod_dest_label(const EngineInfo& info, ModDest dest);
unsigned mod_dest_list(const EngineInfo& info, std::array<uint8_t, size_t(ModDest::Count)>& out);
// Values from a file of an older params_version (limits.h): the parameters
// added since take their initial values instead of the zeros the file holds.
void upgrade_params(EngineKind kind, unsigned version, std::array<int16_t, max_params>& params);
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
std::string decibels(int level); // Format::Level: "-inf", "-74.0" .. "+6.0"
float rate_hertz(int value);   // Format::Rate

} // namespace melodee
