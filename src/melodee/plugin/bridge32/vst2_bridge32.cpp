// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// vst2_bridge32: runs one armv7 VST2 plug-in for a 64-bit host. Built for
// armhf and started through the shipped armhf loader:
//   ld-linux-armhf.so.3 --library-path RUNTIME vst2_bridge32 FD
// FD is the host's memfd holding a bridge::Shared. No JUCE here: only the
// plug-in, vst2_abi.h and libc, so the 32-bit side stays small.
#include "melodee/plugin/vst2_abi.h"
#include "melodee/plugin/vst2_bridge_protocol.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <linux/futex.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <initializer_list>
#include <signal.h>

namespace
{
bridge::Shared* shared = nullptr;
vst2::Effect* effect = nullptr;
float rate = 48000.0f;
int32_t block = 128;
vst2::TimeInfo timeInfo {};
vst2::MidiEvent midiEvents[bridge::maxEvents];
vst2::Events<bridge::maxEvents> events {};
float* inputs[bridge::maxChannels];
float* outputs[bridge::maxChannels];

void futexWait (uint32_t* word, uint32_t expected)
{
    timespec timeout { 0, 100 * 1000 * 1000 };
    syscall (SYS_futex, word, FUTEX_WAIT, expected, &timeout, nullptr, 0);
}

void futexWake (uint32_t* word)
{
    syscall (SYS_futex, word, FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

intptr_t dispatch (int32_t opcode, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0.0f)
{
    return effect->dispatcher (effect, opcode, index, value, ptr, opt);
}

void dispatchText (int32_t opcode, int32_t index, char* out, size_t size)
{
    char text[256] = {};
    dispatch (opcode, index, 0, text);
    text[sizeof (text) - 1] = 0;
    std::snprintf (out, size, "%s", text);
}

intptr_t hostCallback (vst2::Effect* e, int32_t opcode, int32_t, intptr_t, void* ptr, float)
{
    const auto copy = [ptr] (const char* text) { if (ptr != nullptr) std::strcpy ((char*) ptr, text); return (intptr_t) 1; };
    switch (opcode)
    {
        case vst2::host::version: return vst2::hostVersion;
        case vst2::host::currentId: return e != nullptr ? e->uniqueId : 0;
        case vst2::host::getSampleRate: return (intptr_t) rate;
        case vst2::host::getBlockSize: return block;
        case vst2::host::getTime: return (intptr_t) &timeInfo;
        case vst2::host::getVendorString: return copy ("Melodee");
        case vst2::host::getProductString: return copy ("Melodee");
        case vst2::host::getVendorVersion: return 1;
        case vst2::host::getLanguage: return 1;
        case vst2::host::canDo:
            for (const char* yes : { "sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents", "receiveVstMidiEvent" })
                if (ptr != nullptr && std::strcmp ((const char*) ptr, yes) == 0)
                    return 1;
            return 0;
        default: return 0;
    }
}

bool open (const char* path)
{
    void* library = dlopen (path, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        std::snprintf (shared->text, bridge::maxText, "%s", dlerror());
        return false;
    }
    auto* entry = dlsym (library, "VSTPluginMain");
    if (entry == nullptr)
        entry = dlsym (library, "main");
    if (entry == nullptr)
    {
        std::snprintf (shared->text, bridge::maxText, "no VSTPluginMain");
        return false;
    }
    effect = reinterpret_cast<vst2::EntryPoint> (entry) (&hostCallback);
    if (effect == nullptr || effect->magic != vst2::magic || effect->processReplacing == nullptr)
    {
        std::snprintf (shared->text, bridge::maxText, "VSTPluginMain returned no usable effect");
        effect = nullptr;
        return false;
    }
    if (effect->numInputs > (int32_t) bridge::maxChannels || effect->numOutputs > (int32_t) bridge::maxChannels)
    {
        std::snprintf (shared->text, bridge::maxText, "more than %u channels", bridge::maxChannels);
        effect = nullptr;
        return false;
    }
    timeInfo.sampleRate = rate;
    vst2::setTime (timeInfo, false, 0.0, 0.0, 0.0, 120.0, 4, 4); // stopped at the top until the first block says
    dispatch (vst2::op::open);
    shared->info = { effect->numInputs, effect->numOutputs, effect->numParams, effect->numPrograms,
                     effect->flags, effect->uniqueId, effect->version, effect->initialDelay };
    dispatchText (vst2::op::getEffectName, 0, shared->text, bridge::maxText);
    if (shared->text[0] == 0)
        dispatchText (vst2::op::getProductString, 0, shared->text, bridge::maxText);
    dispatchText (vst2::op::getVendorString, 0, (char*) shared->data, 256);
    for (uint32_t i = 0; i < bridge::maxEvents; ++i)
        events.events[i] = &midiEvents[i];
    for (uint32_t c = 0; c < bridge::maxChannels; ++c)
    {
        inputs[c] = shared->audioIn[c];
        outputs[c] = shared->audioOut[c];
    }
    return true;
}

void process()
{
    const auto frames = (int32_t) shared->frames;
    const auto count = shared->numEvents < bridge::maxEvents ? shared->numEvents : bridge::maxEvents;
    if (count > 0)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            auto& event = midiEvents[i];
            event = {};
            event.type = vst2::midiEventType;
            event.byteSize = sizeof (vst2::MidiEvent);
            event.deltaFrames = shared->events[i].deltaFrames;
            std::memcpy (event.data, shared->events[i].data, 4);
        }
        events.count = (int32_t) count;
        dispatch (vst2::op::processEvents, 0, 0, &events);
    }
    const auto& time = shared->time;
    timeInfo.sampleRate = rate;
    vst2::setTime (timeInfo, time.playing != 0, time.samplePos, time.ppqPos, time.barStartPos, time.tempo,
                   time.numerator, time.denominator);
    effect->processReplacing (effect, inputs, outputs, frames);
}

#ifndef SCHED_RESET_ON_FORK
#define SCHED_RESET_ON_FORK 0x40000000
#endif

// This thread answers the host's audio thread every block, so once the plug-in
// is open it runs at that thread's priority: a block never waits behind normal
// work. Not while opening (a plug-in's set-up must not hold a core from the
// audio threads), and threads the plug-in starts later go back to normal
// (SCHED_RESET_ON_FORK). Best effort: without the right to, it stays as it was.
void raisePriority()
{
    sched_param priority {};
    priority.sched_priority = bridge::audioPriority;
    if (sched_setscheduler (0, SCHED_FIFO | SCHED_RESET_ON_FORK, &priority) != 0)
        std::fprintf (stderr, "vst2_bridge32: no real-time priority (%s)\n", std::strerror (errno));
}

// Runs one command; false once the host asked the bridge to quit.
bool run (bridge::Command command)
{
    using bridge::Command;
    shared->result = 0;
    if (command == Command::open)
    {
        shared->result = open (shared->text) ? 1 : 0;
        if (shared->result != 0)
            raisePriority();
        return true;
    }
    if (effect == nullptr)
        return command != Command::close;

    switch (command)
    {
        case Command::setRateAndBlock:
            rate = shared->f0;
            block = shared->i0;
            dispatch (vst2::op::setSampleRate, 0, 0, nullptr, rate);
            dispatch (vst2::op::setBlockSize, 0, block);
            break;
        case Command::mains:
            if (shared->i0 != 0)
            {
                dispatch (vst2::op::mainsChanged, 0, 1);
                dispatch (vst2::op::startProcess);
                shared->info.initialDelay = effect->initialDelay; // a plug-in may set it as it is switched on
            }
            else
            {
                dispatch (vst2::op::stopProcess);
                dispatch (vst2::op::mainsChanged, 0, 0);
            }
            break;
        case Command::process: process(); break;
        case Command::getParameter: shared->f0 = effect->getParameter (effect, shared->i0); break;
        case Command::setParameter: effect->setParameter (effect, shared->i0, shared->f0); break;
        case Command::parameterName: dispatchText (vst2::op::getParamName, shared->i0, shared->text, bridge::maxText); break;
        case Command::parameterLabel: dispatchText (vst2::op::getParamLabel, shared->i0, shared->text, bridge::maxText); break;
        case Command::parameterDisplay: dispatchText (vst2::op::getParamDisplay, shared->i0, shared->text, bridge::maxText); break;
        case Command::getProgram: shared->result = (int32_t) dispatch (vst2::op::getProgram); break;
        case Command::setProgram: dispatch (vst2::op::setProgram, 0, shared->i0); break;
        case Command::programName:
            shared->text[0] = 0;
            dispatch (vst2::op::getProgramNameIndexed, shared->i0, -1, shared->text);
            shared->text[bridge::maxText - 1] = 0;
            break;
        case Command::tailSize: shared->result = (int32_t) dispatch (vst2::op::getTailSize); break;
        case Command::getState:
        {
            void* data = nullptr;
            const auto size = dispatch (vst2::op::getChunk, 0, 0, &data);
            const bool fits = data != nullptr && size > 0 && (uint32_t) size <= bridge::maxData;
            shared->dataSize = fits ? (uint32_t) size : 0;
            if (fits)
                std::memcpy (shared->data, data, (size_t) size);
            shared->result = fits ? 1 : 0;
            break;
        }
        case Command::setState:
            dispatch (vst2::op::setChunk, 0, (intptr_t) shared->dataSize, shared->data);
            break;
        case Command::close:
            dispatch (vst2::op::close);
            effect = nullptr;
            return false;
        default: break;
    }
    return true;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: vst2_bridge32 FD\n");
        return 2;
    }
    // Die with the host rather than linger if it crashes.
    prctl (PR_SET_PDEATHSIG, SIGKILL);
    const int fd = std::atoi (argv[1]);
    void* mapped = mmap (nullptr, sizeof (bridge::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED)
    {
        std::perror ("vst2_bridge32: mmap");
        return 1;
    }
    close (fd);
    shared = static_cast<bridge::Shared*> (mapped);
    if (shared->layoutSize != sizeof (bridge::Shared))
    {
        std::fprintf (stderr, "vst2_bridge32: layout %u, host has %u\n", (unsigned) sizeof (bridge::Shared), shared->layoutSize);
        return 1;
    }
    bridge::store (shared->bridgeAlive, 1);
    futexWake (&shared->bridgeAlive);

    uint32_t handled = bridge::load (shared->reply);
    for (;;)
    {
        // Spin briefly first: in steady playback the next block arrives
        // within a period, and a wake-up costs more than the spin.
        uint32_t request = bridge::load (shared->request);
        for (int spin = 0; request == handled && spin < 4000; ++spin)
            request = bridge::load (shared->request);
        if (request == handled)
        {
            futexWait (&shared->request, handled);
            continue;
        }
        const bool keepGoing = run (shared->command);
        handled = request;
        bridge::store (shared->reply, request);
        futexWake (&shared->reply);
        if (! keepGoing)
            return 0;
    }
}
