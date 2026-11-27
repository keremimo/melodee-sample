// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The shared memory between a 64-bit host (Vst2BridgeInstance) and the
// 32-bit vst2_bridge32 process that loads an armv7 VST2 plug-in.
//
// The host creates the block with memfd_create, starts the bridge with the
// descriptor, and from then on writes a command, bumps `request` and waits
// until `reply` equals it. The bridge waits on `request`, runs the command
// against the plug-in and stores the same sequence number in `reply`. Both
// words are futexes in shared (non-private) memory, so one block's round
// trip is two wake-ups, or none while either side is still spinning.
//
// Only fixed-width fields: the struct must lay out the same on armv7 and
// aarch64. `layoutSize` lets the bridge refuse a host built differently.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bridge
{
constexpr uint32_t maxChannels = 16, maxFrames = 4096, maxEvents = 512, maxText = 1024, maxData = 1 << 20;
// The host's audio thread's SCHED_FIFO priority (base drivers/audio.cpp): the
// bridge's worker takes it once the plug-in is open, as it answers that thread.
constexpr int audioPriority = 30;

enum class Command : uint32_t
{
    none,
    open,             // text: library path -> info, text: name, data: vendor
    setRateAndBlock,  // f0: rate, i0: block
    mains,            // i0: 1 on (and start), 0 off (and stop)
    process,          // frames, events, time, audioIn -> audioOut
    getParameter,     // i0 -> f0
    setParameter,     // i0, f0
    parameterName,    // i0 -> text
    parameterLabel,   // i0 -> text
    parameterDisplay, // i0 -> text
    getProgram,       // -> result
    setProgram,       // i0
    programName,      // i0 -> text
    tailSize,         // -> result
    getState,         // -> data, dataSize
    setState,         // data, dataSize, i0: 1 one program's chunk, 0 the bank's
    close,            // the bridge closes the plug-in and exits
};

struct Event
{
    int32_t deltaFrames;
    uint8_t data[4];
};

struct Info
{
    int32_t numInputs, numOutputs, numParams, numPrograms, flags, uniqueId, version;
    int32_t initialDelay; // roadmap P4: the latency it reports, frames at its rate (kept when it is switched on)
};

// The host's transport at the start of a process block (vst2::setTime).
struct Time
{
    double samplePos, ppqPos, barStartPos, tempo;
    int32_t playing, numerator, denominator, unused;
};

struct Shared
{
    uint32_t layoutSize;
    uint32_t request, reply;   // futex words, sequence numbers
    uint32_t bridgeAlive;      // set by the bridge once it has mapped this
    Command command;
    int32_t i0, result;
    float f0;
    uint32_t frames, numEvents, dataSize;
    Info info;
    Time time;
    Event events[maxEvents];
    char text[maxText];
    uint8_t data[maxData];
    float audioIn[maxChannels][maxFrames];
    float audioOut[maxChannels][maxFrames];
};

static_assert (offsetof (Shared, time) == 80, "Shared must lay out the same on armv7 and aarch64");

// A sequence number is complete when the bridge has echoed it.
inline uint32_t load (const uint32_t& word) { return __atomic_load_n (&word, __ATOMIC_ACQUIRE); }
inline void store (uint32_t& word, uint32_t value) { __atomic_store_n (&word, value, __ATOMIC_RELEASE); }
inline uint32_t take (uint32_t& word) { return __atomic_exchange_n (&word, 0u, __ATOMIC_ACQ_REL); }

// A plug-in's own screen (sdk/melodee-screen/melodee_screen.h) has a block of
// its own, a second memfd: a draw never waits behind the audio thread's
// block, nor that block behind a draw. The bridge answers it on a second
// thread of normal priority, with its own request and reply words, and runs
// it only while no other non-audio call into the plug-in does (`result` -1:
// busy, ask again later). `redraw` and `run` are the plug-in's asks (a draw;
// a block although silent), set by the bridge from any thread and taken by
// the host. The plug-in draws straight into `pixels`, which keep its last
// picture between draws.
enum class ScreenCommand : uint32_t
{
    none,
    draw,     // the frame's fields -> result: 1 it drew anything new
    input,    // inputKind, inputValue, inputShift -> result: 1 it used it
    controls, // -> controls: its K1-K3 parameters, MELODEE_SCREEN_NO_CONTROL none
};
constexpr uint32_t screenWidth = 320, screenRows = 218;

struct ScreenShared
{
    uint32_t layoutSize;
    uint32_t request, reply;   // futex words, sequence numbers
    uint32_t hasScreen;        // the bridge found one when the plug-in opened
    uint32_t redraw, run;      // the plug-in's asks
    ScreenCommand command;
    int32_t result;
    uint32_t width, height, area, redrawAll, colour, playing, shift, reserved;
    uint64_t timeMs;
    int32_t pots[3];
    uint32_t potsLive[3];
    uint32_t inputKind;
    int32_t inputValue;
    uint32_t inputShift;
    uint32_t controls[3];
    uint32_t pixels[screenWidth * screenRows];
};

static_assert (offsetof (ScreenShared, timeMs) == 64 && offsetof (ScreenShared, pixels) == 120,
               "ScreenShared must lay out the same on armv7 and aarch64");
} // namespace bridge
