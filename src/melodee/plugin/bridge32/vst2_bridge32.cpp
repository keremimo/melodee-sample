// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// vst2_bridge32: runs one armv7 VST2 plug-in for a 64-bit host. Built for
// armhf and started through the shipped armhf loader:
//   ld-linux-armhf.so.3 --library-path RUNTIME vst2_bridge32 FD [SCREENFD]
// FD is the host's memfd holding a bridge::Shared; SCREENFD one holding a
// bridge::ScreenShared, for a plug-in's own screen (answered on a second
// thread). No JUCE here: only the plug-in, vst2_abi.h, the screen header and
// libc, so the 32-bit side stays small.
#include "melodee/plugin/vst2_abi.h"
#include "melodee/plugin/vst2_bridge_protocol.h"
#include <melodee_screen.h>

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
#include <pthread.h>
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
// Its own screen: the second block, the plug-in's table, and the lock that keeps the screen thread's calls apart from
// the command thread's other non-audio ones (priority inheriting: the command thread runs at the audio priority).
bridge::ScreenShared* screen = nullptr;
const melodee_screen* screenTable = nullptr;
pthread_mutex_t mainLock;

void initMainLock()
{
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init (&attributes);
    pthread_mutexattr_setprotocol (&attributes, PTHREAD_PRIO_INHERIT);
    if (pthread_mutex_init (&mainLock, &attributes) != 0)
    {
        pthread_mutexattr_setprotocol (&attributes, PTHREAD_PRIO_NONE);
        pthread_mutex_init (&mainLock, &attributes);
    }
    pthread_mutexattr_destroy (&attributes);
}

// No timeout: the host wakes the bridge for every command (and screen call),
// and PR_SET_PDEATHSIG ends it if the host dies, so an idle bridge has no
// reason to wake.
void futexWait (uint32_t* word, uint32_t expected)
{
    syscall (SYS_futex, word, FUTEX_WAIT, expected, nullptr, nullptr, 0);
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

intptr_t hostCallback (vst2::Effect* e, int32_t opcode, int32_t index, intptr_t value, void* ptr, float)
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
            if (ptr != nullptr && screen != nullptr && std::strcmp ((const char*) ptr, MELODEE_SCREEN_VST2_CANDO) == 0)
                return 1;
            return 0;
        case vst2::host::vendorSpecific: // its own screen: a draw asked for, and maybe a block (any thread)
            if (index != MELODEE_SCREEN_VST2 || screen == nullptr)
                return 0;
            bridge::store (screen->redraw, 1);
            if ((value & MELODEE_SCREEN_RUN) != 0)
                bridge::store (screen->run, 1);
            return 1;
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
    // Its own screen: asked for only from a plug-in that says it has one (effCanDo), so a vendor call's answer is
    // never taken on trust.
    if (screen != nullptr && dispatch (vst2::op::canDo, 0, 0, (void*) MELODEE_SCREEN_VST2_CANDO) == 1)
    {
        auto* table = reinterpret_cast<const melodee_screen*> (dispatch (vst2::op::vendorSpecific, MELODEE_SCREEN_VST2));
        if (table != nullptr && table->draw != nullptr && table->input != nullptr && table->controls != nullptr)
            screenTable = table;
    }
    if (screen != nullptr)
        bridge::store (screen->hasScreen, screenTable != nullptr ? 1u : 0u);
    return true;
}

// One screen command, on the screen thread: -1 while the command thread is in the plug-in.
int32_t runScreen (bridge::ScreenCommand command)
{
    if (screenTable == nullptr)
        return 0;
    if (pthread_mutex_trylock (&mainLock) != 0)
        return -1;
    if (effect == nullptr) // closed meanwhile
    {
        pthread_mutex_unlock (&mainLock);
        return 0;
    }
    int32_t result = 0;
    switch (command)
    {
        case bridge::ScreenCommand::draw:
        {
            melodee_screen_frame_t frame {};
            frame.pixels = screen->pixels;
            frame.width = bridge::screenWidth;
            frame.height = screen->height <= bridge::screenRows ? screen->height : bridge::screenRows;
            frame.stride = bridge::screenWidth;
            frame.area = screen->area;
            frame.redraw_all = screen->redrawAll;
            frame.colour = screen->colour;
            frame.time_ms = screen->timeMs;
            frame.playing = (uint8_t) screen->playing;
            frame.shift = (uint8_t) screen->shift;
            for (int k = 0; k < 3; ++k)
            {
                frame.pots[k] = (int16_t) screen->pots[k];
                frame.pots_live[k] = (uint8_t) screen->potsLive[k];
            }
            result = screenTable->draw (effect, &frame) ? 1 : 0;
            break;
        }
        case bridge::ScreenCommand::input:
        {
            melodee_screen_input_t input { screen->inputKind, screen->inputValue, screen->inputShift };
            result = screenTable->input (effect, &input) ? 1 : 0;
            break;
        }
        case bridge::ScreenCommand::controls:
        {
            uint32_t ids[3] { MELODEE_SCREEN_NO_CONTROL, MELODEE_SCREEN_NO_CONTROL, MELODEE_SCREEN_NO_CONTROL };
            screenTable->controls (effect, ids);
            for (int k = 0; k < 3; ++k)
                screen->controls[k] = ids[k];
            result = 1;
            break;
        }
        default: break;
    }
    pthread_mutex_unlock (&mainLock);
    return result;
}

// The screen thread: waits on the screen block's own request word, at normal priority (made before the command thread
// takes the audio priority).
void* screenThread (void*)
{
    uint32_t handled = bridge::load (screen->reply);
    for (;;)
    {
        const uint32_t request = bridge::load (screen->request);
        if (request == handled)
        {
            futexWait (&screen->request, handled);
            continue;
        }
        screen->result = runScreen (screen->command);
        handled = request;
        bridge::store (screen->reply, request);
        futexWake (&screen->reply);
    }
    return nullptr;
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

    // The plug-in's non-audio calls run under the lock its screen's calls try (the audio block's need not).
    const bool locked = command != Command::process && command != Command::getParameter && command != Command::setParameter;
    if (locked)
        pthread_mutex_lock (&mainLock);
    struct Unlock { bool on; ~Unlock() { if (on) pthread_mutex_unlock (&mainLock); } } unlock { locked };

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
            dispatch (vst2::op::setChunk, shared->i0 != 0 ? 1 : 0, (intptr_t) shared->dataSize, shared->data);
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
    initMainLock();
    if (argc >= 3) // the screen's block: mapped, checked, and its thread started now, at normal priority
    {
        const int screenFd = std::atoi (argv[2]);
        void* screenMapped = mmap (nullptr, sizeof (bridge::ScreenShared), PROT_READ | PROT_WRITE, MAP_SHARED, screenFd, 0);
        close (screenFd);
        if (screenMapped != MAP_FAILED && static_cast<bridge::ScreenShared*> (screenMapped)->layoutSize == sizeof (bridge::ScreenShared))
        {
            screen = static_cast<bridge::ScreenShared*> (screenMapped);
            pthread_t thread;
            if (pthread_create (&thread, nullptr, &screenThread, nullptr) == 0)
                pthread_detach (thread);
            else
                screen = nullptr;
        }
    }
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
        // Spin briefly first, for a command that follows the last at once (an
        // instrument rendered in parts, a run of parameter reads). The next
        // block comes a period later, far past any spin worth making, so the
        // spin stays short and the thread then sleeps until the host wakes it.
        uint32_t request = bridge::load (shared->request);
        for (int spin = 0; request == handled && spin < 1000; ++spin)
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
