// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <cstdint>

// One change for the audio thread. The shell (MIDI thread) is the only
// producer; the engine host drains them at the start of each render().
namespace melodee {

struct Command {
    enum class Type : uint8_t {
        NoteOn,      // track, index = key, value = velocity
        NoteOff,     // track, index = key
        AllNotesOff, // track; with track = 255, every track
        Engine,      // track, index = EngineKind (the parameters follow as Param)
        Sound,       // track, index = sound: sets every parameter from the engine's factory sound
        Param,       // track, index = parameter, value
        Wheel,       // track, value 0..1000
        Level, Pan, Send, Mute, Solo, // track, value
        SetStep,     // track, pattern, index = step, step
        Length,      // track, pattern, value = steps
        Pattern,     // track, value = pattern to play (on the next bar while playing patterns)
        Division,    // track, pattern, value = Division
        Play,        // value 1: the song from bar `index`; 0: the patterns
        Stop,
        SongClip,    // index, clip
        SongCount,   // value = clips in use
        SongLoop,    // index = first bar, value = bar after the last (equal: no loop)
        Tempo,       // value = tenths of a BPM
        Swing,       // value = percent
        MotionWrite, // motion: Motion::write(motion)
        MotionClear, // motion: Motion::clear(track, pattern, param)
        Mod,         // track, index = route, route
        FxKind,      // track = chain, index = slot, value = EffectKind (its initial values come with it)
        FxParam,     // track = chain, index = slot, pattern = parameter, value
        FxBypass,    // track = chain, index = slot, value
        SendBus,     // track, value = bus 0-3
        Return,      // index = bus, value = 0..100
        Pressure,    // track, index = key, value = 0..127 (poly aftertouch)
        Pad,         // track, index = kit pad, pad
        Audition,    // track, pad (sample, start, end), value = velocity: a region on no pad
        PadNote,     // track, index = kit pad, value = velocity, pattern = semitones + 64: 16 Levels
        RecordArm,   // track = source (0 mic, 1 line, 2 the mix), index = sample slot, value = threshold dB, pattern = bars (0: until stopped)
        RecordGain,  // value = dB
        RecordNow, RecordStop,
        Reload,      // pack: a whole project to switch to; handed back through the host's done queue
        Click,       // value: on; index: level 0..100
        Arp,         // track; index = 0 on, 1 mode, 2 rate, 3 octaves, 4 gate; value
        ArpKey,      // track, index = key, value = velocity (0: let go)
        // Punch: track = the target (255 the master), index = PunchKind | pad << 8
        // (pad 1..64: that pad alone on a sampler track, 0: the whole target),
        // value = depth 0..100, pattern = snap (0 1/4 .. 3 1/32) | tail << 4.
        PunchOn, PunchOff, // PunchOff: track and index's pad as PunchOn's; track 254: whatever punch is on
        PunchDepth         // index's pad as PunchOn's (0: the master or track punch), value = depth
    };
    Command() = default;
    Command(Type type, uint8_t track = 0, uint8_t pattern = 0, uint16_t index = 0, int16_t value = 0)
        : type(type), track(track), pattern(pattern), index(index), value(value) {}
    Type type = Type::NoteOn;
    uint8_t track = 0, pattern = 0;
    uint16_t index = 0;
    int16_t value = 0;
    Step step {};
    MotionEvent motion {};
    ModRoute route {};
    Clip clip {};
    PadSound pad {};
    const struct ProjectPack* pack = nullptr;
};

} // namespace melodee
