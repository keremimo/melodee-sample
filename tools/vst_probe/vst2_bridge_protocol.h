// SPDX-License-Identifier: AGPL-3.0-only
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

#include <cstdint>

namespace bridge
{
constexpr uint32_t maxChannels = 16, maxFrames = 4096, maxEvents = 512, maxText = 1024, maxData = 1 << 20;

enum class Command : uint32_t
{
    none,
    open,             // text: library path -> info, text: name, data: vendor
    setRateAndBlock,  // f0: rate, i0: block
    mains,            // i0: 1 on (and start), 0 off (and stop)
    process,          // frames, events, audioIn -> audioOut
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
    setState,         // data, dataSize
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
    Event events[maxEvents];
    char text[maxText];
    uint8_t data[maxData];
    float audioIn[maxChannels][maxFrames];
    float audioOut[maxChannels][maxFrames];
};

// A sequence number is complete when the bridge has echoed it.
inline uint32_t load (const uint32_t& word) { return __atomic_load_n (&word, __ATOMIC_ACQUIRE); }
inline void store (uint32_t& word, uint32_t value) { __atomic_store_n (&word, value, __ATOMIC_RELEASE); }
} // namespace bridge
