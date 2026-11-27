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
        PatternSteps, Division, Swing, // Patterns: PatternSteps the length in sixteenths (Shift + K1, issue 27)
        PatternLength, TimeSignature,  // Patterns: K1 1, 2, 3 .. 16, 24 .. 128 bars; Shift + K2 the project's signature
        TcShift, TcSwing,              // Time Correct (Division is its Q)
        SongZoom, SongPattern, SongLength, // Song; id: the clip under the cursor, or -1
        FxParam,                           // id: chain * 64 + slot * 8 + parameter
        SendBus, BusReturn,                // Sends; BusReturn id: the bus
        PadPitch, PadLevel, PadPan,        // the last pad hit on a sampler track
        PadStart, PadEnd, TrimZoom,        // Trim, in milliseconds of the sample
        ChopMode, ChopAmount, ChopStart,   // Chop; ChopStart in milliseconds (id: the slice)
        FileJump, PluginJump,              // Samples, the plug-in picker
        RecThreshold, RecLength, RecGain,  // Record and Input Config (RecLength: 0 Free, 1 Seq, 2.. bars: rec_bars())
        ProjectJump, ProjectSort,          // Project
        Tempo, ClickLevel,                 // Tempo (Swing as on Length)
        Features,                          // Settings' K1: 0 Basic, 1 Full (shell/features.h)
        UsageLog,                          // Settings' K2 on the doors: 0 off, 1 on (shell/usage_log.h)
        SettingRow, MidiRow,               // Settings' and MIDI Config's K1: the row under the cursor (id), its value as a list
        ArpMode, ArpRate, ArpOctaves,      // Arp
        ChordType, ChordVoicing, ChordInvert, // Chord
        PadFine, PadPlayMode, PadGroup, PadReverse, PadAttack, PadDecay, PadRelease, PadSustain, // sampler pages
        PunchDepth, PunchSnap, PunchTail,          // Punch
        NameSuggest, NameChar,                     // the Name sheet
        Transpose,                                 // Keys, Shift + K3: the pads by semitones
        EventVelocity, EventLength,                // Step Edit: the selected event (length in steps of Q)
        EventChance,                               // Roll, K3: the note in focus, percent
        // Sample Mode (parity phase 4), the last pad hit; TrimZoom's id is the handle it closes in on (0 start, 1 end, 2 loop)
        PadLoopPoint, PadLoop, PadMono, PadLink, PadOffset, PadVelSense, PadDecayFrom,
        PadFilterType, PadCutoff, PadResonance, PadFAttack, PadFDecay, PadFSustain, PadFRelease, PadFAmount,
        PadWarp, PadBeats,                         // PadWarp: 0 Off, 1..151 50..200 %, 152 Seq
        ChopEnd, ChopZoom,                         // Chop (parity phase 5): K2 the slice's end (ms); Shift + K1 / K2 zoom (id 0 start, 1 end)
        RecSource,                                 // Record and Input Config (parity phase 6): index into input_choices()
        RecTap,                                    // Shift + K1 there, the Track source: 0 Pre FX, 1 Post FX
        KnobParam,                                 // Knob FX (parity phase 7), id: the effect's parameter 0..5
        CompAttack, CompRelease, CompAmount, CompBoost, // the master compressor (parity phase 7)
        PadFxParam,                                // Pad FX (parity phase 7b): id = the MPC's Pad FX * 4 + its K
        FlexMode, FlexMix,                         // Flex Beat
        ModWheel,                                  // the fader's mod wheel (parity phase 8), 0..127
        // Stage (shell/stage_map.h): PRESET's sound (its place in sound_list()) and list, the arp's gate, hold and on,
        // chords on or off, an empty track's choosers; the Tracks screen's track and its pattern.
        StageSound, SoundList, ArpGate, ArpHold, ArpOn, ChordOn,
        NewEngine, NewSound, NewSource, NewPort, NewChannel, // NewEngine: index into new_engines()
        TrackPick, TrackPattern,
        LevelsType, LevelsLow, LevelsHigh, // 16 Levels: the type (index into levels_types()), pad 1's end and pad 16's
        // The Looper (docs/LOOPER.md): the loop, its length (index into looper::lengths), the settings every loop
        // shares; on MIX the loop track's strip (id: the loop).
        LoopPick, LoopLength, LoopLevel, LoopFeedback, LoopQuantize, LoopThen, LoopPan, LoopSend, LoopBus,
        PluginParam,                               // a plug-in's parameter (docs/MELODEE.md), id: plugin_binding(), 0..plugin_param_max
        // Ratchets (roadmap P1), on the TRIG row of Steps (B1 again) and Roll (B2 again): Steps' step in hand (on a
        // kit its lane's notes), Roll's note in focus; the pitch ramp a kit pad's (Shift + K2).
        StepRatchet, StepRamp, StepPitchRamp, EventRatchet, EventRamp, EventPitchRamp,
        StepCondition, EventCondition, // the TRIG row's K3: a trig condition (model/trig.h), 0..trig_count - 1
        // Step Edit's SOUND row (kit, keygroup): the step's own sample (Elektron's sample lock; value 0 its pad's, n
        // the nth of step_sound_choices(), id its Event::sample), slice (0 PAD, 1..id the chop's, id + 1 RND) and way.
        StepSound, StepSlice, StepPlay,
        Lock, // a step held on Steps or a pad held on Roll's note in focus: a Motion lane (id) locked there (roadmap P1)
        LaneLength, // Steps on a kit, Shift + K1: the last pad's lane's own length in steps of Q (the pattern's: none)
        // Step Edit as step entry (round 2, step-edit2 C): what a pad writes at the caret, length in steps of Q,
        // velocity (0: as played), chance; Roll's Shift + K1 NUDGE: the note in focus off its grid line, in ticks.
        EntryLength, EntryVelocity, EntryChance, EventNudge, MarksRamp, // MarksRamp: Roll's Shift + K2 over marked notes, percent
        MarksVelocity, // Roll's K2 over marked notes: the lowest velocity among them, the rest moved by as much (shell/roll.h)
        // A pattern's feel (roadmap P1, model/pattern.h Feel), heard live: strength toward Q 0..100, its swing 50..75,
        // humanize timing and velocity 0..100, a groove taken from a pattern (0 none, else 1 + track * 16 + pattern)
        // and how much of it 0..100. The selected track's pattern playing.
        FeelStrength, FeelSwing, FeelHumanize, FeelVelocity, GrooveFrom, GrooveAmount,
        // Generators (roadmap P1): Steps' EUCLID row on a kit (the last pad's lane: hits, steps, rotation) and the VARY
        // sheet's amount (percent).
        EuclidHits, EuclidSteps, EuclidRotate, VaryAmount,
        TrackSpeed, // Patterns, Shift + K3: the track's playback speed (roadmap P1), id the track, an index of speeds
        LoopSource, // the Looper's loop sheet, K1: the loop's source, an index into loop_sources() (shell/loop_sheet.h), id the loop
        // Pad layers (roadmap P3), the last pad hit: the layer in hand 0-3, its velocities, its tune and level over the
        // pad's (hundredths of a dB in steps of a tenth), and how a hit picks its layers (model/sampler.h LayerPlay).
        LayerPick, LayerLow, LayerHigh, LayerPitch, LayerGain, LayerPlayMode,
        // Round 3 "instrument C", Stage's ZONES on a pad holding an instrument: the zone in hand (UiState::zone), its
        // trim's tune (cents) and level (tenths of a dB) on the last pad (model/sampler.h ZoneTrim).
        ZonePick, ZoneTune, ZoneLevel,
        PadLoopXfade, // roadmap P3, PLAY's Shift + K3: the last pad's loop seam crossfade, ms (0 none)
        FxKey,        // roadmap P4, a Compressor's or Gate's third row: what keys it (model/project.h fx_key_*), id as FxParam's
        // Roadmap P4, the MOD page (model/modulators.h): the modulator in hand, its kind, an LFO's shape, clock and rate
        // (free: 0..100; on the clock: a division), a follower's source (tracks, buses) and rise and fall, the target in
        // hand's depth; a macro, its value (every target with it), the target in hand's low and high.
        ModPick, ModKind, ModShape, ModSync, ModRate, ModInput, ModAttack, ModRelease, ModDepth,
        MacroPick, MacroValue, MacroLow, MacroHigh,
        MidiDrumNote, // roadmap P4 (M3), a MIDI track's Drum pads: the note pad `id` (16 bank + pad) plays, Edit's PADS K2
        MidiPort,     // roadmap P4 (M4), DEVICE's K1: External, USB-C, each USB device by name, All USB (shell/midi_track_jobs.h)
        MidiSysexFile, // roadmap P4 (M4), SYSEX's K1: a .syx of the card's SysEx folder (0 none)
        // Roadmap P5, SCENES (Push on Patterns): the selected track's pattern's follow action (model/follow.h): the first
        // and (Shift + K1) the second action, after so many (0..63: 1..64 bars, 64..127: 1..64 plays), the first's odds.
        FollowAction, FollowOther, FollowAfter, FollowChance, SceneTempo,
        // Roadmap P5, Song's Shift + K1 / K2: the tempo lane's point at the cursor's bar, its tempo in whole BPM (29: none
        // there; with none, the lane's tempo there) and its shape (0 a step on its bar, 1 a ramp from the point before).
        SongTempo, SongTempoShape,
        PitchBend, // a PLUGIN track's pitch bend (the fader's Pitch Bend), -64..63 around the centre
        PatternProgram, // a MIDI track's PROGRAM K3: the program its pattern in hand sends as it starts (0: the track's)
        // The AUTO SAMPLE sheet's rows: KEYS (lowest and highest root, MIDI notes; semitones between roots), NOTE
        // (velocity layers 1-4; hold and tail in tenths of a second), SOUND (0 Pre FX, 1 Post FX; loop off or on;
        // 0 the level as played, 1 normalized to -1 dB).
        AutoLow, AutoHigh, AutoStep, AutoLayers, AutoHold, AutoTail, AutoTap, AutoLoop, AutoLevel,
        PrintTapPick, PrintTailPick, // freeze plan F2c, the PRINT sheet: K1 where prints are taken (PrintTap), K2 how they end (PrintTail)
        PadSend,  // Stage's SENDS on a kit, K2: the last pad's own send into bus id (PadSound::sends)
        PadColour, // Stage's COLOUR on a kit, K1: the last pad's colour, a place on shell/pad_colour.h's list
        PadCloud,  // Stage's CLOUD on a kit: the last pad's cloud; id 0 on or off, 1-5 position, size, density, scan, spread
        ProgKey, ProgVoicing, // Keys on the Prog layout: K1 the progression's key (0..11), K2 its voicing (model/scales.h ProgVoicing)
        BuildRoot, BuildChord, BuildInvert, // 16 Levels' SET (shell/levels_keys.h): the chord being made, its root (0..11), type and inversion
        // Freeze plan F3, the Record page on an audio track: K1 its source (an index into audio_sources()), K2 its monitor.
        AudioSource, AudioMonitorMode,
        // Stage on an audio track, the take of the pattern in hand: CLIP its level (tenths of a dB), start and end in the
        // take (ms); FADES fade in and out (ms) and reverse.
        RegionGain, RegionStart, RegionEnd, RegionFadeIn, RegionFadeOut, RegionReverse,
        // WARP (B1's third page): K1 0 Off, 1 Stretch, 2 Repitch; K2 the tempo it was recorded at (tenths of a BPM); K3
        // its semitones, Shift + K3 its cents (Stretch moves its pitch by them).
        RegionWarp, RegionBpm, RegionSemi, RegionFine,
        SceneColour, // Patterns' SEQUENCE K2 (issue 19): the sequence in view's colour, 0 none, 1-16 a track colour
        JamFrom, JamTo, // JAMS on Song (shell/jam_jobs.cpp): the first and last bar of the picked jam to play or take
        PadMute, // issue 32, Motion's card of a kit pad's mute lane: pad id muted (1) or not, as Mute + pad
        // The mixer (docs/MIXER_PLAN.md): one value of a strip, id = strip number << 8 | StripPart (model/strip.h); a
        // track's output, id the track, 0 the master or out_sub(s); a submix's level, pan or send, id = submix << 8 |
        // Command::SubPart (engine/command.h).
        Strip, TrackOut, SubMix,
        // The mixer's input (its B1 INPUT): Input Config's monitor (0 Off, 1 Auto, 2 On) and what takes record, 0 the
        // input dry, 1 after its strip (Project::input_wet).
        InputMonitor, InputWet,
        PadOut, // Stage's SENDS on a kit, Shift + K2: the last pad's output (PadSound::out), 0 the kit, 1-8 a submix
        // Samples' trim sheet (shell/browse_trim.h): the file under the cursor's START and END in frames (id 1: not read
        // yet, the card shows no value), Shift + K2 / K3 its zoom 0..100 (id 0 at START, 1 at END).
        BrowseStart, BrowseEnd, BrowseZoom,
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
    // The value a pot at `position` (0..127) asks for. A range across 0 gives 0 to the position nearest it: a
    // range wider than the pot's 128 places would otherwise step over it (ZONES' TUNE went -1, +1), and a card
    // whose middle is "none" (a trim, a tune, a pan) must turn back to it by hand.
    int at(int position) const {
        if (min < 0 && max > 0 && position == (-min * 127 + (max - min) / 2) / (max - min)) return 0;
        return min + (position * (max - min) + 63) / 127;
    }
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
std::array<int, 3> motion_lanes(const Project& project, const UiState& ui, const Pattern& pattern);
// What a lane is: its description and the track's own value; info is
// nullptr when there is no such parameter (or a pad value phase 4 brings).
struct MotionParam { const struct ParamInfo* info = nullptr; int value = 0; };
// Roadmap P4: the rows of cards an effect in chain `chain` has (Minus / Plus step them): its parameters' one or two,
// and a Compressor's or Gate's KEY after them (not on a kit pad's chain).
unsigned effect_rows(const EffectSlot& s, unsigned chain);
// Roadmap P4: the mix's lanes (model/limits.h motion_mix_base on) need `project` for the master's chain and the
// returns; without it they are none.
MotionParam motion_param(const Track& track, int lane, const Project* project = nullptr);
std::string lane_name(const Track& track, int lane, const Project* project = nullptr); // "CUTOFF", "A03 LEVEL", "SEND B"
// The card for a lane on the Motion page.
Binding lane_binding(const Track& track, unsigned track_index, int lane, const Project* project = nullptr);
// Roadmap P1: the cards while a step (Steps) or a note (Roll, its pad held) is held: the Motion page's three lanes,
// each showing its lock at `tick` (else the track's own value); a turn writes the lock (Context::write_lock).
std::array<Binding, 3> lock_cards(const Project& project, const UiState& ui, const Pattern& pattern, uint32_t tick);
// Roadmap P1, a pattern's feel (model/pattern.h Feel) as cards, for Time Correct's page (round 2) to lay out: row 0
// strength, swing, humanize; row 1 humanize's velocity, the groove (where from) and how much of it.
std::array<Binding, 3> feel_cards(const Pattern& pattern, unsigned row);
// The tick a held step or note locks at and how long the lock holds; false when nothing is held.
bool lock_place(const Project& project, const UiState& ui, const Pattern& pattern, uint32_t& tick, uint32_t& hold);
// The mixer's cards (docs/MIXER_PLAN.md): the lit block's three values of the selected track's strip (SENDS: sends A-C;
// EQ: the band group's three; COMP: amount, ratio, speed), or `shift_row`'s (pan, output, send D; none; key, mix,
// gain). Basic: send A alone, and pan on Shift.
std::array<Binding, 3> mixer_cards(const Project& project, const UiState& ui, bool shift_row);
inline Binding strip_card(unsigned n, StripPart part, const Strip& s) {
    static constexpr int lo[] {-trim_range, 0, 0, -eq_range, -eq_range, -eq_range, -eq_range, -eq_range, -eq_range, -eq_range,
                               -eq_range, -eq_range, 0, 0, int(comp_ratio_min), 0, 0, 0, comp_gain_min};
    static constexpr int hi[] {trim_range, int(width_max), 1, eq_range, eq_range, eq_range, eq_range, eq_range, eq_range, eq_range,
                               eq_range, eq_range, 1, int(comp_amount_max), int(comp_ratio_max), int(CompSpeed::Count) - 1,
                               strip_key_most, 100, comp_gain_max};
    static_assert(sizeof(lo) / sizeof(lo[0]) == size_t(StripPart::Count) && sizeof(hi) / sizeof(hi[0]) == size_t(StripPart::Count));
    const unsigned p = unsigned(part) % unsigned(StripPart::Count);
    return {Binding::Kind::Strip, int32_t(n << 8 | p), lo[p], hi[p], strip_get(s, part)};
}
// The chain a page shows: the selected track's, its bus's, or the master's.
unsigned page_chain(const Project& project, const UiState& ui);
// Patterns' K1: lengths in bars, as on the MPC: every bar to 16 (issue 52: 3, 5, 7 ..), then longer steps to 128 so
// the pot's travel still finds each; Shift + K1 in sixteenths, 1 to pattern_step_max (issue 27: 5/16 against a
// 16-step pattern on another track).
constexpr unsigned pattern_lengths[] {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24, 32, 48, 64, 96, 128};
constexpr unsigned pattern_step_max = 64;
constexpr unsigned pattern_length_count = sizeof(pattern_lengths) / sizeof(pattern_lengths[0]);
// A pattern's length in whole bars of the project's signature, rounded up.
inline unsigned pattern_bars(const Project& project, const Pattern& pattern) {
    return std::max(1u, unsigned((pattern.length + project.bar_ticks() - 1) / project.bar_ticks()));
}
// K1's pattern length card: 1, 2, 3 .. 16, 24 .. 128 bars by index, and -1 Free below them on an empty pattern whose sequence
// has no notes on another track (shell/free_length.h). A Free pattern shows the length a take on it would get.
Binding length_card(const Project& project, const UiState& ui, const Pattern& pattern);
// Warp on its card: 0 Off, 1..151 for 50..200 %, 152 Seq; and back.
constexpr int warp_card_seq = 152;
inline int warp_card(uint8_t warp) { return warp == warp_seq ? warp_card_seq : warp >= warp_min ? int(warp) - (warp_min - 1) : 0; }
inline uint8_t warp_of_card(int value) { return value >= warp_card_seq ? warp_seq : value <= 0 ? warp_off : uint8_t(value + warp_min - 1); }
// Song's zoom steps, in bars across the view.
constexpr unsigned song_zooms[] {4, 8, 16, 32, 64};
// Stage's cards for the selected track's page (shell/stage_map.h), and its Shift row.
std::array<Binding, 3> stage_bindings(const Project& project, const UiState& ui, const Pattern& pattern, bool shift_row);
// The engines an empty track can become on Stage's SYNTH page, in K1's order.
unsigned new_engines(std::array<EngineKind, size_t(EngineKind::Count)>& out);
// The track's three favourite parameters, for every page without its own.
std::array<Binding, 3> favourite_bindings(const Track& track);
// The Motion lane a card's value lands in when it moves during Seq Record
// (an engine parameter, a kit pad's own value on the last pad, the mod
// wheel), -1 where it lands in none.
int binding_lane(const Track& track, const Binding& b);
// Erase + pot on a kit pad's own value (Tune, Mix, Play's offset, the envelopes, the filter, its sends, cloud and loop
// crossfade): the card's value on a new pad, false for Trim's handles, a toggle or a choice.
bool pad_reset_value(const Binding& b, int& value);
// Whether Erase + pot sends the card back to its initial value: an engine's, Knob FX's or a plug-in's parameter, a
// kit pad's own value, or a lock card (the lock out: the lane back to what it holds there).
bool erase_resets(const Binding& b);
// Roll's Shift + K1 NUDGE (round 2): how far `e` sits off its nearest grid line of Q, in ticks (-q/2 .. q/2 - 1);
// turning it moves the note to that line plus the offset.
Binding nudge_card(const Pattern& pattern, const Event& e);
inline uint32_t nudge_base(const Pattern& pattern, const Event& e) { unsigned q = pattern.q(); return (e.tick + q / 2) / q * q; }
// Roadmap P5: a follow action's AFTER on its card (0..63 bars, 64..127 plays) and back.
inline int follow_after_card(const PatternFollow& f) { return (f.plays ? 64 : 0) + std::clamp<int>(f.count, 1, 64) - 1; }
inline void follow_after_of_card(PatternFollow& f, int value) { f.plays = value >= 64; f.count = uint8_t(value % 64 + 1); }

} // namespace melodee
