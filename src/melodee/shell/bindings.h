// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include <array>
#include <cstdint>

// What K1-K3 hold on the page in view. Every page states its three cards as
// Bindings; the shell turns pot moves into values through them and the
// pages draw the cards from them, so both always agree.
namespace melodee {

struct Binding {
    enum class Kind : uint8_t {
        None, Param, Level, Pan, Send, Root, Scale, Layout, Engine, Jump, Audition, StepLength, StepVelocity, StepChance,
        ModSource, ModDest, ModAmount, // id: the route
        PatternBars, Division, Swing,  // Length
        SongZoom, SongPattern, SongLength, // Song; id: the clip under the cursor, or -1
        FxParam,                           // id: chain * 64 + slot * 8 + parameter
        SendBus, BusReturn,                // Sends; BusReturn id: the bus
        PadPitch, PadLevel, PadPan,        // the last pad hit on a sampler track
        PadStart, PadEnd, TrimZoom,        // Trim, in milliseconds of the sample
        ChopMode, ChopAmount, ChopStart,   // Chop; ChopStart in milliseconds
        FileJump,                          // Samples
        RecThreshold, RecBars, RecGain,    // Record
        ProjectJump, ProjectSort,          // Project
        Tempo, ClickLevel,                 // Tempo (Swing as on Length)
        ArpMode, ArpRate, ArpOctaves,      // Arp
        ChordType, ChordVoicing, ChordInvert, // Chord
        CaptureBars, CaptureQuantise, CaptureInto, // Capture
        PadFine, PadPlayMode, PadGroup, PadReverse, PadAttack, PadDecay, PadRelease, PadSustain, // sampler pages
        PunchDepth, PunchSnap, PunchTail,          // Punch
        PadPunchKind, PadPunchRate, PadPunchDepth, // Pad Punch: the last pad hit
        NameSuggest, NameChar,                     // the Name sheet
        Transpose                                  // Keys, Shift + K3: the pads by semitones
    };
    Kind kind = Kind::None;
    int32_t id = 0;      // Param: the parameter
    int min = 0, max = 0, value = 0;
    // Which control this is, for pickup; a song clip's index may move as clips change, so it is left out.
    uint32_t key(unsigned track) const {
        bool clip = kind == Kind::SongPattern || kind == Kind::SongLength;
        return uint32_t(kind) << 24 | uint32_t(clip ? 0 : uint16_t(id)) << 8 | track;
    }
    bool bound() const { return kind != Kind::None && max > min; }
    float fraction() const { return max > min ? float(value - min) / float(max - min) : 0; }
    // The value a pot at `position` (0..127) asks for.
    int at(int position) const { return min + (position * (max - min) + 63) / 127; }
};

// The cards for a page; `pattern` is the selected track's playing pattern.
// While Shift is held a card with a second parameter (shift_bindings())
// turns that one instead, as on the MPC; the card shows a Shift mark.
std::array<Binding, 3> page_bindings(const Project& project, const UiState& ui, const Pattern& pattern);
std::array<Binding, 3> page_bindings(const Project& project, const UiState& ui, const Pattern& pattern, bool shift);
// The page's own cards, and its Shift row (unbound where a card has none).
std::array<Binding, 3> base_bindings(const Project& project, const UiState& ui, const Pattern& pattern);
std::array<Binding, 3> shift_bindings(const Project& project, const UiState& ui, const Pattern& pattern);
// The three parameters the Motion page shows as lanes: those holding motion
// on the track's playing pattern first, then the track's favourites.
std::array<int, 3> motion_lanes(const Project& project, const UiState& ui);
// What a motion lane's parameter is (an engine parameter, or one of the track
// chain's effects', see motion_fx_param()): its description and the
// track's own value; info is nullptr when there is no such parameter.
struct MotionParam { const struct ParamInfo* info = nullptr; int value = 0; };
MotionParam motion_param(const Track& track, int param);
// The card for a lane's parameter on the Motion page.
Binding lane_binding(const Track& track, unsigned track_index, int param);
// The chain a page shows: the selected track's, its bus's, or the master's.
unsigned page_chain(const Project& project, const UiState& ui);
// Song's zoom steps, in bars across the view.
constexpr unsigned song_zooms[] {4, 8, 16, 32, 64};
// The track's three favourite parameters, for every page without its own.
std::array<Binding, 3> favourite_bindings(const Track& track);

} // namespace melodee
