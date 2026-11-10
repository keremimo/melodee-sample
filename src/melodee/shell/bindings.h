// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include <array>
#include <cstdint>
#include <string>

// What K1-K3 hold on the page in view. Every page states its three cards as
// Bindings; the shell turns pot moves into values through them and the
// pages draw the cards from them, so both always agree.
namespace melodee {

struct Binding {
    enum class Kind : uint8_t {
        None, Param, Level, Pan, Send, Root, Scale, Layout, Engine, Jump, Audition, StepLength, StepVelocity, StepChance,
        ModSource, ModDest, ModAmount, // id: the route
        PatternBars, Division, Swing,  // Patterns: PatternBars any count (Shift + K1)
        PatternLength, TimeSignature,  // Patterns: K1 1, 2, 4 .. 128 bars; Shift + K2 the project's signature
        TcShift, TcSwing,              // Time Correct (Division is its Q)
        SongZoom, SongPattern, SongLength, // Song; id: the clip under the cursor, or -1
        FxParam,                           // id: chain * 64 + slot * 8 + parameter
        SendBus, BusReturn,                // Sends; BusReturn id: the bus
        PadPitch, PadLevel, PadPan,        // the last pad hit on a sampler track
        PadStart, PadEnd, TrimZoom,        // Trim, in milliseconds of the sample
        ChopMode, ChopAmount, ChopStart,   // Chop; ChopStart in milliseconds (id: the slice)
        FileJump,                          // Samples
        RecThreshold, RecLength, RecGain,  // Record and Input Config (RecLength: 0 Free, 1 Seq, 2.. 1..16 bars)
        ProjectJump, ProjectSort,          // Project
        Tempo, ClickLevel,                 // Tempo (Swing as on Length)
        ArpMode, ArpRate, ArpOctaves,      // Arp
        ChordType, ChordVoicing, ChordInvert, // Chord
        CaptureBars, CaptureQuantise, CaptureInto, // Capture
        PadFine, PadPlayMode, PadGroup, PadReverse, PadAttack, PadDecay, PadRelease, PadSustain, // sampler pages
        PunchDepth, PunchSnap, PunchTail,          // Punch
        PadPunchKind, PadPunchRate, PadPunchDepth, // Pad Punch: the last pad hit
        NameSuggest, NameChar,                     // the Name sheet
        Transpose,                                 // Keys, Shift + K3: the pads by semitones
        EventVelocity, EventLength,                // Step Edit: the selected event (length in steps of Q)
        EventChance,                               // Roll, K3: the note in focus, percent
        // Sample Mode (parity phase 4), the last pad hit; TrimZoom's id is the handle it closes in on (0 start, 1 end, 2 loop)
        PadLoopPoint, PadLoop, PadMono, PadLink, PadOffset, PadVelSense, PadDecayFrom,
        PadFilterType, PadCutoff, PadResonance, PadFAttack, PadFDecay, PadFSustain, PadFRelease, PadFAmount,
        PadWarp, PadBeats,                         // PadWarp: 0 Off, 1..151 50..200 %, 152 Seq
        ChopEnd, ChopZoom,                         // Chop (parity phase 5): K2 the slice's end (ms); Shift + K1 / K2 zoom (id 0 start, 1 end)
        RecSource,                                 // Record and Input Config (parity phase 6): InputSource
        KnobParam,                                 // Knob FX (parity phase 7), id: the effect's parameter 0..5
        CompAttack, CompRelease, CompAmount, CompBoost, // the master compressor (parity phase 7)
        PadFxParam,                                // Pad FX (parity phase 7b): id = the MPC's Pad FX * 4 + its K
        FlexMode, FlexMix,                         // Flex Beat
        ModWheel,                                  // the fader's mod wheel (parity phase 8), 0..127
        // Stage (shell/stage_map.h): PRESET's sound (its place in sound_list()) and list, the arp's gate and hold,
        // an empty track's choosers; the Tracks screen's track and its pattern.
        StageSound, SoundList, ArpGate, ArpHold,
        NewEngine, NewSound, NewSource, NewPort, NewChannel, // NewEngine: index into new_engines()
        TrackPick, TrackPattern,
        // The Looper (docs/LOOPER.md): the loop, its length (index into looper::lengths), the settings every loop
        // shares; on MIX the loop track's strip (id: the loop).
        LoopPick, LoopLength, LoopLevel, LoopFeedback, LoopQuantize, LoopThen, LoopPan, LoopSend, LoopBus
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
// A Motion lane: an engine parameter, one of the track chain's effects'
// (motion_fx_param()), or a kit pad's own value on one pad
// (model/pad_params.h): param | pad << 8.
constexpr int motion_lane(unsigned param, unsigned pad = 0) { return int(param | pad << 8); }
constexpr unsigned lane_param(int lane) { return unsigned(lane) & 0xff; }
constexpr unsigned lane_pad(int lane) { return unsigned(lane) >> 8; }
// The three lanes the Motion page shows: those holding motion on the
// track's playing pattern first (a kit's pad lanes of the last pad hit),
// then on a kit the last pad's tune, level and pan, else the favourites.
std::array<int, 3> motion_lanes(const Project& project, const UiState& ui);
// What a lane is: its description and the track's own value; info is
// nullptr when there is no such parameter (or a pad value phase 4 brings).
struct MotionParam { const struct ParamInfo* info = nullptr; int value = 0; };
MotionParam motion_param(const Track& track, int lane);
std::string lane_name(const Track& track, int lane); // "CUTOFF", "A03 LEVEL"
// The card for a lane on the Motion page.
Binding lane_binding(const Track& track, unsigned track_index, int lane);
// The chain a page shows: the selected track's, its bus's, or the master's.
unsigned page_chain(const Project& project, const UiState& ui);
// Patterns' K1: lengths in bars, as on the MPC (Shift + K1 any bar count).
constexpr unsigned pattern_lengths[] {1, 2, 4, 8, 16, 32, 64, 128};
constexpr unsigned pattern_length_count = sizeof(pattern_lengths) / sizeof(pattern_lengths[0]);
// A pattern's length in whole bars of the project's signature, rounded up.
inline unsigned pattern_bars(const Project& project, const Pattern& pattern) {
    return std::max(1u, unsigned((pattern.length + project.bar_ticks() - 1) / project.bar_ticks()));
}
// Warp on its card: 0 Off, 1..151 for 50..200 %, 152 Seq; and back.
constexpr int warp_card_seq = 152;
inline int warp_card(uint8_t warp) { return warp == warp_seq ? warp_card_seq : warp >= warp_min ? int(warp) - (warp_min - 1) : 0; }
inline uint8_t warp_of_card(int value) { return value >= warp_card_seq ? warp_seq : value <= 0 ? warp_off : uint8_t(value + warp_min - 1); }
// Song's zoom steps, in bars across the view.
constexpr unsigned song_zooms[] {4, 8, 16, 32, 64};
// Stage's cards for the selected track's page (shell/stage_map.h), and its Shift row.
std::array<Binding, 3> stage_bindings(const Project& project, const UiState& ui, bool shift_row);
// The engines an empty track can become on Stage's SYNTH page, in K1's order.
unsigned new_engines(std::array<EngineKind, size_t(EngineKind::Count)>& out);
// The track's three favourite parameters, for every page without its own.
std::array<Binding, 3> favourite_bindings(const Track& track);
// The Motion lane a card's value lands in when it moves during Seq Record
// (an engine parameter, a kit pad's own value on the last pad, the mod
// wheel), -1 where it lands in none.
int binding_lane(const Track& track, const Binding& b);

} // namespace melodee
