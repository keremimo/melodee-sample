// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/vst2_bridge_instance.h"
#include "melodee/plugin/vst2_abi.h"
#include "melodee/plugin/host_time.h"

#include <melodee_screen.h>

#include <algorithm>
#include <chrono>
#include <vector>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <linux/futex.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace melodee {
using namespace juce;

namespace
{
void futexWait (uint32_t* word, uint32_t expected, int timeoutMs)
{
    timespec timeout { timeoutMs / 1000, (long) (timeoutMs % 1000) * 1000000L };
    syscall (SYS_futex, word, FUTEX_WAIT, expected, &timeout, nullptr, 0);
}

void futexWake (uint32_t* word)
{
    syscall (SYS_futex, word, FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

AudioChannelSet channelSet (int channels)
{
    return channels == 1 ? AudioChannelSet::mono()
         : channels == 2 ? AudioChannelSet::stereo()
                         : AudioChannelSet::discreteChannels (channels);
}

} // namespace

class Vst2BridgeInstance::Parameter final : public AudioPluginInstance::HostedParameter
{
public:
    Parameter (Vst2BridgeInstance& owner, int index) : owner (owner), index (index), initial (owner.parameter (index)) {}

    float getValue() const override { return owner.parameter (index); }
    void setValue (float value) override
    {
        const std::lock_guard<InheritingMutex> held (owner.connection->lock);
        owner.shared->i0 = index;
        owner.shared->f0 = value;
        owner.call (bridge::Command::setParameter);
    }
    float getDefaultValue() const override { return initial; }
    String getName (int maximum) const override { return owner.callText (bridge::Command::parameterName, index).substring (0, maximum); }
    String getLabel() const override { return owner.callText (bridge::Command::parameterLabel, index); }
    String getParameterID() const override { return String (index); }
    float getValueForText (const String& text) const override { return text.getFloatValue(); }
    String getText (float value, int maximum) const override
    {
        if (std::abs (value - getValue()) < 1.0e-6f)
            return owner.callText (bridge::Command::parameterDisplay, index).substring (0, maximum);
        return String (value, 3);
    }

private:
    Vst2BridgeInstance& owner;
    const int index;
    const float initial;
};

bool Vst2BridgeInstance::isArm32 (const File& file)
{
    FileInputStream in (file);
    uint8 header[20] = {};
    if (! in.openedOk() || in.read (header, sizeof (header)) != (int) sizeof (header))
        return false;
    // ELF magic, class 1 (32-bit), e_machine 40 (ARM), little-endian.
    return std::memcmp (header, "\x7f" "ELF", 4) == 0 && header[4] == 1 && header[18] == 40 && header[19] == 0;
}

std::unique_ptr<Vst2BridgeInstance> Vst2BridgeInstance::load (const File& library, const File& bridgeFolder,
                                                              double rate, int block, String& error)
{
    const auto runtime = bridgeFolder.getChildFile ("armhf");
    const auto loader = runtime.getChildFile ("ld-linux-armhf.so.3");
    const auto program = bridgeFolder.getChildFile ("vst2_bridge32");
    if (! loader.existsAsFile() || ! program.existsAsFile())
    {
        error = "no 32-bit bridge in " + bridgeFolder.getFullPathName();
        return {};
    }

    const int fd = (int) syscall (SYS_memfd_create, "vst2-bridge", 0);
    if (fd < 0 || ftruncate (fd, sizeof (bridge::Shared)) != 0)
    {
        error = "memfd: " + String (std::strerror (errno));
        return {};
    }
    void* mapped = mmap (nullptr, sizeof (bridge::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED)
    {
        close (fd);
        error = "mmap: " + String (std::strerror (errno));
        return {};
    }
    auto* shared = static_cast<bridge::Shared*> (mapped);
    shared->layoutSize = sizeof (bridge::Shared);
    // The screen's own block (bridge::ScreenShared): without it the plug-in simply has no screen here.
    bridge::ScreenShared* screen = nullptr;
    const int screenFd = (int) syscall (SYS_memfd_create, "vst2-bridge-screen", 0);
    if (screenFd >= 0 && ftruncate (screenFd, sizeof (bridge::ScreenShared)) == 0)
    {
        void* screenMapped = mmap (nullptr, sizeof (bridge::ScreenShared), PROT_READ | PROT_WRITE, MAP_SHARED, screenFd, 0);
        if (screenMapped != MAP_FAILED)
        {
            screen = static_cast<bridge::ScreenShared*> (screenMapped);
            screen->layoutSize = sizeof (bridge::ScreenShared);
        }
    }
    const auto unmapScreen = [&] { if (screen != nullptr) munmap (screen, sizeof (bridge::ScreenShared)); };

    const auto fdText = String (fd).toStdString();
    const auto screenFdText = String (screenFd).toStdString();
    const auto loaderPath = loader.getFullPathName().toStdString();
    const auto runtimePath = runtime.getFullPathName().toStdString();
    const auto programPath = program.getFullPathName().toStdString();
    // posix_spawn, not fork(): a fork copies Melodee's page tables under the memory lock and makes each page the audio
    // thread writes next a copy-on-write fault until the child execs; glibc's posix_spawn shares the memory until the
    // exec. The memfd is inherited, as with fork.
    const char* args[] = { loaderPath.c_str(), "--library-path", runtimePath.c_str(), programPath.c_str(), fdText.c_str(),
                           screen != nullptr ? screenFdText.c_str() : nullptr, nullptr };
    pid_t pid = -1;
    const int spawned = posix_spawn (&pid, args[0], nullptr, nullptr, const_cast<char* const*> (args), environ);
    close (fd);
    if (screenFd >= 0)
        close (screenFd);
    if (spawned != 0)
    {
        munmap (mapped, sizeof (bridge::Shared));
        unmapScreen();
        error = "spawn: " + String (std::strerror (spawned));
        return {};
    }

    // Wait for the bridge to map the block, or to die trying.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (3);
    while (bridge::load (shared->bridgeAlive) == 0)
    {
        int status = 0;
        if (waitpid (pid, &status, WNOHANG) == pid || std::chrono::steady_clock::now() > deadline)
        {
            if (std::chrono::steady_clock::now() > deadline)
                kill (pid, SIGKILL), waitpid (pid, &status, 0);
            munmap (mapped, sizeof (bridge::Shared));
            unmapScreen();
            error = "the 32-bit bridge did not start (status " + String (status) + ")";
            return {};
        }
        futexWait (&shared->bridgeAlive, 0, 50);
    }

    auto connection = std::make_unique<Connection>();
    connection->shared = shared;
    connection->pid = pid;
    connection->screen = screen;
    std::snprintf (shared->text, bridge::maxText, "%s", library.getFullPathName().toRawUTF8());
    if (! connection->call (bridge::Command::open, 10000) || shared->result == 0)
    {
        error = connection->dead ? "the bridge died opening the plug-in" : String::fromUTF8 (shared->text);
        return {};
    }
    // Buses come from the opened plug-in, so the instance is built after.
    return std::unique_ptr<Vst2BridgeInstance> (new Vst2BridgeInstance (std::move (connection), library, rate, block));
}

AudioProcessor::BusesProperties Vst2BridgeInstance::busesOf (const bridge::Info& info)
{
    BusesProperties buses;
    if (info.numInputs > 0)
        buses = buses.withInput ("Input", channelSet (info.numInputs), true);
    if (info.numOutputs > 0)
        buses = buses.withOutput ("Output", channelSet (info.numOutputs), true);
    return buses;
}

Vst2BridgeInstance::Vst2BridgeInstance (std::unique_ptr<Connection> opened, const File& library, double newRate, int newBlock)
    : AudioPluginInstance (busesOf (opened->shared->info)), connection (std::move (opened)),
      shared (connection->shared), file (library), rate (newRate), block (newBlock)
{
    name = String::fromUTF8 (shared->text);
    if (name.isEmpty())
        name = file.getFileNameWithoutExtension();
    vendor = String::fromUTF8 ((const char*) shared->data);
    for (int i = 0; i < shared->info.numParams; ++i)
        addHostedParameter (std::make_unique<Parameter> (*this, i));
    shared->f0 = (float) rate;
    shared->i0 = block;
    call (bridge::Command::setRateAndBlock);
}

Vst2BridgeInstance::~Vst2BridgeInstance()
{
    if (active)
        releaseResources();
}

Vst2BridgeInstance::Connection::~Connection()
{
    if (! dead)
        call (bridge::Command::close, 1000);
    int status = 0;
    for (int i = 0; i < 20 && waitpid (pid, &status, WNOHANG) == 0; ++i)
        usleep (10000);
    if (waitpid (pid, &status, WNOHANG) == 0)
    {
        kill (pid, SIGKILL);
        waitpid (pid, &status, 0);
    }
    munmap (shared, sizeof (bridge::Shared));
    if (screen != nullptr)
        munmap (screen, sizeof (bridge::ScreenShared));
}

// One screen command on the screen's own block: the bridge's screen thread answers it, apart from the connection.
bool Vst2BridgeInstance::Connection::screenCall (bridge::ScreenCommand command, int32_t& result, int timeoutMs)
{
    if (screen == nullptr || dead)
        return false;
    screen->command = command;
    const auto request = ++screenSequence;
    bridge::store (screen->request, request);
    futexWake (&screen->request);
    for (int spin = 0; spin < 2000; ++spin)
        if (bridge::load (screen->reply) == request)
        {
            result = screen->result;
            return true;
        }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
    for (;;)
    {
        const auto reply = bridge::load (screen->reply);
        if (reply == request)
        {
            result = screen->result;
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        futexWait (&screen->reply, reply, 5);
    }
}

bool Vst2BridgeInstance::hasScreen() const
{
    return connection->screen != nullptr && bridge::load (connection->screen->hasScreen) != 0;
}

bool Vst2BridgeInstance::screenDraw (const melodee_screen_frame& frame, bool& changed)
{
    if (! hasScreen())
        return false;
    auto& c = *connection;
    const std::lock_guard<std::mutex> held (c.screenLock);
    auto* s = c.screen;
    const uint32_t width = std::min (frame.width, bridge::screenWidth), height = std::min (frame.height, bridge::screenRows);
    s->width = width;
    s->height = height;
    s->area = frame.area;
    s->redrawAll = frame.redraw_all;
    s->colour = frame.colour;
    s->timeMs = frame.time_ms;
    s->playing = frame.playing;
    s->shift = frame.shift;
    for (int k = 0; k < 3; ++k)
    {
        s->pots[k] = frame.pots[k];
        s->potsLive[k] = frame.pots_live[k];
    }
    int32_t result = 0;
    if (! c.screenCall (bridge::ScreenCommand::draw, result) || result < 0)
        return false;
    changed = result == 1;
    if (changed || frame.redraw_all != 0) // its picture, out of the block
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy (frame.pixels + (size_t) row * frame.stride, s->pixels + (size_t) row * bridge::screenWidth, sizeof (uint32_t) * width);
    return true;
}

bool Vst2BridgeInstance::screenInput (const melodee_screen_input& input, bool& used)
{
    if (! hasScreen())
        return false;
    auto& c = *connection;
    const std::lock_guard<std::mutex> held (c.screenLock);
    c.screen->inputKind = input.kind;
    c.screen->inputValue = input.value;
    c.screen->inputShift = input.shift;
    int32_t result = 0;
    if (! c.screenCall (bridge::ScreenCommand::input, result) || result < 0)
        return false;
    used = result == 1;
    return true;
}

bool Vst2BridgeInstance::screenControls (std::array<int, 3>& indices)
{
    if (! hasScreen())
        return false;
    auto& c = *connection;
    const std::lock_guard<std::mutex> held (c.screenLock);
    int32_t result = 0;
    if (! c.screenCall (bridge::ScreenCommand::controls, result) || result < 0)
        return false;
    for (size_t k = 0; k < 3; ++k) // a VST2 parameter's ID is its index
    {
        const uint32_t id = c.screen->controls[k];
        indices[k] = id != MELODEE_SCREEN_NO_CONTROL && id < (uint32_t) shared->info.numParams ? (int) id : -1;
    }
    return true;
}

bool Vst2BridgeInstance::takeRedraw()
{
    return connection->screen != nullptr && bridge::take (connection->screen->redraw) != 0;
}

bool Vst2BridgeInstance::wantsProcess() const
{
    return connection->screen != nullptr && bridge::load (connection->screen->run) != 0;
}

// One command, one round trip. The host spins for the reply first (an empty
// command is back within microseconds), then sleeps on the futex. A process
// block that outlasts the spin means the plug-in's own work takes longer than
// a spin, and the next block's will too: the next blocks sleep at once,
// leaving the core to the bridge and the other tracks, and every so often one
// spins again in case the plug-in has become quicker.
bool Vst2BridgeInstance::Connection::call (bridge::Command command, int timeoutMs)
{
    constexpr int spinLoads = 20000, blocksAfterSlowReply = 64;
    const std::lock_guard<InheritingMutex> held (lock);
    if (dead)
        return false;
    shared->command = command;
    const auto request = ++sequence;
    bridge::store (shared->request, request);
    futexWake (&shared->request);

    const bool process = command == bridge::Command::process;
    if (process && unspunBlocks > 0)
        --unspunBlocks;
    else
    {
        for (int spin = 0; spin < spinLoads; ++spin)
            if (bridge::load (shared->reply) == request)
                return true;
        if (process)
            unspunBlocks = blocksAfterSlowReply;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
    for (;;)
    {
        const auto reply = bridge::load (shared->reply);
        if (reply == request)
            return true;
        if (std::chrono::steady_clock::now() > deadline || waitpid (pid, nullptr, WNOHANG) == pid)
        {
            dead = true;
            return false;
        }
        futexWait (&shared->reply, reply, 5);
    }
}

String Vst2BridgeInstance::callText (bridge::Command command, int index) const
{
    const std::lock_guard<InheritingMutex> held (connection->lock);
    shared->i0 = index;
    shared->text[0] = 0;
    if (! call (command))
        return {};
    shared->text[bridge::maxText - 1] = 0;
    return String::fromUTF8 (shared->text).trim();
}

float Vst2BridgeInstance::parameter (int index) const
{
    const std::lock_guard<InheritingMutex> held (connection->lock);
    shared->i0 = index;
    return call (bridge::Command::getParameter) ? shared->f0 : 0.0f;
}

void Vst2BridgeInstance::fillInPluginDescription (PluginDescription& description) const
{
    const auto& info = shared->info;
    const bool synth = (info.flags & vst2::flag::isSynth) != 0;
    description.name = description.descriptiveName = name;
    description.pluginFormatName = "VST (armv7 bridge)";
    description.category = synth ? "Synth" : "Effect";
    description.manufacturerName = vendor;
    description.version = String (info.version);
    description.fileOrIdentifier = file.getFullPathName();
    description.uniqueId = description.deprecatedUid = info.uniqueId;
    description.isInstrument = synth;
    description.numInputChannels = info.numInputs;
    description.numOutputChannels = info.numOutputs;
}

void Vst2BridgeInstance::prepareToPlay (double newRate, int newBlock)
{
    const std::lock_guard<InheritingMutex> held (connection->lock);
    if (active)
        releaseResources();
    rate = newRate;
    block = std::min (newBlock, (int) bridge::maxFrames);
    setRateAndBufferSizeDetails (rate, block);
    shared->f0 = (float) rate;
    shared->i0 = block;
    call (bridge::Command::setRateAndBlock);
    shared->i0 = 1;
    call (bridge::Command::mains);
    active = true;
    setLatencySamples (std::max (0, (int) shared->info.initialDelay)); // roadmap P4: delay compensation
}

void Vst2BridgeInstance::releaseResources()
{
    const std::lock_guard<InheritingMutex> held (connection->lock);
    if (! active)
        return;
    shared->i0 = 0;
    call (bridge::Command::mains);
    active = false;
}

void Vst2BridgeInstance::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    const std::lock_guard<InheritingMutex> held (connection->lock);
    if (connection->screen != nullptr) // this block is the one it asked for
        bridge::store (connection->screen->run, 0);
    const int frames = std::min ({ buffer.getNumSamples(), block, (int) bridge::maxFrames });
    const auto& info = shared->info;

    uint32_t count = 0;
    for (const auto metadata : midi)
    {
        if (count == bridge::maxEvents || metadata.numBytes > 3 || metadata.samplePosition >= frames)
            continue;
        auto& event = shared->events[count++];
        event.deltaFrames = metadata.samplePosition;
        std::memset (event.data, 0, sizeof (event.data));
        std::memcpy (event.data, metadata.data, (size_t) metadata.numBytes);
    }
    shared->numEvents = count;
    shared->frames = (uint32_t) frames;
    const auto time = hostTime (getPlayHead());
    shared->time = { (double) time.samples, time.ppq, time.barStart, time.bpm,
                     time.playing ? 1 : 0, time.numerator, time.denominator, 0 };
    for (int c = 0; c < info.numInputs; ++c)
    {
        if (c < buffer.getNumChannels())
            std::memcpy (shared->audioIn[c], buffer.getReadPointer (c), sizeof (float) * (size_t) frames);
        else
            std::memset (shared->audioIn[c], 0, sizeof (float) * (size_t) frames);
    }

    const bool ok = call (bridge::Command::process, processTimeoutMs);
    for (int c = 0; c < buffer.getNumChannels(); ++c)
    {
        if (ok && c < info.numOutputs)
            buffer.copyFrom (c, 0, shared->audioOut[c], frames);
        else
            buffer.clear (c, 0, frames);
    }
}

double Vst2BridgeInstance::getTailLengthSeconds() const
{
    const std::lock_guard<InheritingMutex> held (connection->lock);
    if (! call (bridge::Command::tailSize))
        return 0.0;
    return shared->result > 1 ? shared->result / rate : 0.0;
}

int Vst2BridgeInstance::getCurrentProgram()
{
    const std::lock_guard<InheritingMutex> held (connection->lock);
    return call (bridge::Command::getProgram) ? shared->result : 0;
}

void Vst2BridgeInstance::setCurrentProgram (int index)
{
    const std::lock_guard<InheritingMutex> held (connection->lock);
    shared->i0 = index;
    call (bridge::Command::setProgram);
}

const String Vst2BridgeInstance::getProgramName (int index)
{
    return callText (bridge::Command::programName, index);
}

// Same layout as Vst2Instance: "MVS2", a kind byte, then the chunk or the
// parameter values, so a project can move between the two hosts.
void Vst2BridgeInstance::getStateInformation (MemoryBlock& state)
{
    state.reset();
    state.append (vst2::state::tag, sizeof (vst2::state::tag));
    {
        const std::lock_guard<InheritingMutex> held (connection->lock);
        if ((shared->info.flags & vst2::flag::programChunks) != 0 && call (bridge::Command::getState) && shared->result == 1)
        {
            state.append (&vst2::state::chunk, 1);
            state.append (shared->data, shared->dataSize);
            return;
        }
    }
    // One parameter a round trip, each under the lock alone: the audio thread's blocks pass in between.
    state.append (&vst2::state::parameters, 1);
    for (int i = 0; i < shared->info.numParams; ++i)
    {
        const float value = parameter (i);
        state.append (&value, sizeof (value));
    }
}

void Vst2BridgeInstance::setStateInformation (const void* data, int size)
{
    if (size < 5 || std::memcmp (data, vst2::state::tag, sizeof (vst2::state::tag)) != 0)
        return;
    const auto* bytes = static_cast<const uint8*> (data);
    const auto* body = bytes + 5;
    const auto bodySize = size - 5;
    if (bytes[4] == vst2::state::chunk || bytes[4] == vst2::state::programChunk)
    {
        const std::lock_guard<InheritingMutex> held (connection->lock);
        if (bodySize <= (int) bridge::maxData)
        {
            std::memcpy (shared->data, body, (size_t) bodySize);
            shared->dataSize = (uint32_t) bodySize;
            shared->i0 = bytes[4] == vst2::state::programChunk ? 1 : 0;
            call (bridge::Command::setState);
        }
        return;
    }
    if (bytes[4] != vst2::state::parameters)
        return;
    for (int i = 0; i < std::min ((int) shared->info.numParams, bodySize / (int) sizeof (float)); ++i)
    {
        const std::lock_guard<InheritingMutex> held (connection->lock); // one at a time, as getStateInformation
        shared->i0 = i;
        std::memcpy (&shared->f0, body + i * (int) sizeof (float), sizeof (float));
        call (bridge::Command::setParameter);
    }
}

std::array<double, 3> Vst2BridgeInstance::measureRoundTrips (int count)
{
    std::vector<double> times;
    times.reserve ((size_t) count);
    for (int i = 0; i < count; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        call (bridge::Command::getProgram);
        times.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
    }
    std::sort (times.begin(), times.end());
    double total = 0.0;
    for (auto t : times)
        total += t;
    return { total / (double) times.size(), times[(size_t) (0.99 * (double) (times.size() - 1))], times.back() };
}

} // namespace melodee
