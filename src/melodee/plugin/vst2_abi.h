// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The VST 2.4 plug-in binary interface, as far as a headless host needs it:
// the effect struct a plug-in's VSTPluginMain returns, the opcodes a host
// sends it, the ones a plug-in sends back, and the MIDI event layout.
//
// Written for Melodee from the interface's public, long-documented layout
// (as reimplemented by VeSTige and FST). Steinberg's VST2 SDK is not used:
// it was withdrawn and may not be redistributed. Names are our own.
#pragma once

#include <cstdint>

namespace vst2
{
struct Effect;

using HostCallback = intptr_t (*) (Effect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using Dispatcher = intptr_t (*) (Effect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using ProcessFloat = void (*) (Effect*, float** inputs, float** outputs, int32_t frames);
using ProcessDouble = void (*) (Effect*, double** inputs, double** outputs, int32_t frames);
using SetParameter = void (*) (Effect*, int32_t index, float value);
using GetParameter = float (*) (Effect*, int32_t index);
using EntryPoint = Effect* (*) (HostCallback);

constexpr int32_t magic = ('V' << 24) | ('s' << 16) | ('t' << 8) | 'P';

struct Effect
{
    int32_t magic;
    Dispatcher dispatcher;
    void* processAccumulating;  // deprecated in 2.4
    SetParameter setParameter;
    GetParameter getParameter;
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t reserved1, reserved2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueId, version;
    ProcessFloat processReplacing;
    ProcessDouble processDoubleReplacing;
    char future[56];
};

namespace flag
{
constexpr int32_t hasEditor = 1 << 0, canReplace = 1 << 4, programChunks = 1 << 5, isSynth = 1 << 8,
                  noSoundInStop = 1 << 9, canDoubleReplace = 1 << 12;
}

// Host to plug-in (Effect::dispatcher).
namespace op
{
constexpr int32_t open = 0, close = 1, setProgram = 2, getProgram = 3, getProgramName = 5,
                  getParamLabel = 6, getParamDisplay = 7, getParamName = 8, setSampleRate = 10,
                  setBlockSize = 11, mainsChanged = 12, getChunk = 23, setChunk = 24, processEvents = 25,
                  getProgramNameIndexed = 29, getEffectName = 45, getVendorString = 47,
                  getProductString = 48, getVendorVersion = 49, canDo = 51, getTailSize = 52,
                  getVstVersion = 58, startProcess = 71, stopProcess = 72;
}

// Plug-in to host (HostCallback).
namespace host
{
constexpr int32_t automate = 0, version = 1, currentId = 2, idle = 3, getTime = 7, processEvents = 8,
                  ioChanged = 13, sizeWindow = 15, getSampleRate = 16, getBlockSize = 17,
                  getCurrentProcessLevel = 23, getAutomationState = 24, getVendorString = 32,
                  getProductString = 33, getVendorVersion = 34, canDo = 37, getLanguage = 38,
                  updateDisplay = 42, beginEdit = 43, endEdit = 44;
}

constexpr int32_t midiEventType = 1;

struct MidiEvent
{
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    char data[4];
    char detune, noteOffVelocity, reserved1, reserved2;
};

// Events handed to op::processEvents: a count, then that many pointers.
template <int capacity>
struct Events
{
    int32_t count;
    intptr_t reserved;
    MidiEvent* events[capacity];
};

struct TimeInfo
{
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};

namespace time
{
constexpr int32_t changed = 1 << 0, playing = 1 << 1, nanosValid = 1 << 8, ppqValid = 1 << 9,
                  tempoValid = 1 << 10, barsValid = 1 << 11, timeSigValid = 1 << 13;
}

// The host's transport for one block, as the plug-in reads it (audioMasterGetTime): `changed` set when it starts or
// stops.
inline void setTime (TimeInfo& t, bool isPlaying, double samplePos, double ppq, double barStart, double tempo,
                     int32_t numerator, int32_t denominator)
{
    const bool was = (t.flags & time::playing) != 0;
    t.samplePos = samplePos;
    t.ppqPos = ppq;
    t.barStartPos = barStart;
    t.tempo = tempo;
    t.timeSigNumerator = numerator;
    t.timeSigDenominator = denominator;
    t.flags = time::tempoValid | time::ppqValid | time::barsValid | time::timeSigValid
            | (isPlaying ? time::playing : 0) | (isPlaying != was ? time::changed : 0);
}

constexpr int hostVersion = 2400;
} // namespace vst2
