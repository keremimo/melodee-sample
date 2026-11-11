// SPDX-License-Identifier: AGPL-3.0-only
#include "vst2_bridge_instance.h"
#include "vst2_abi.h"

#include <algorithm>
#include <chrono>
#include <vector>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

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

constexpr char stateTag[4] = { 'M', 'V', 'S', '2' };
constexpr uint8 stateChunk = 1, stateParameters = 2;
} // namespace

class Vst2BridgeInstance::Parameter final : public AudioPluginInstance::HostedParameter
{
public:
    Parameter (Vst2BridgeInstance& owner, int index) : owner (owner), index (index), initial (owner.parameter (index)) {}

    float getValue() const override { return owner.parameter (index); }
    void setValue (float value) override
    {
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

    const auto fdText = String (fd).toStdString();
    const auto loaderPath = loader.getFullPathName().toStdString();
    const auto runtimePath = runtime.getFullPathName().toStdString();
    const auto programPath = program.getFullPathName().toStdString();
    const pid_t pid = fork();
    if (pid == 0)
    {
        const char* args[] = { loaderPath.c_str(), "--library-path", runtimePath.c_str(), programPath.c_str(), fdText.c_str(), nullptr };
        execv (args[0], const_cast<char* const*> (args));
        _exit (127);
    }
    close (fd);
    if (pid < 0)
    {
        munmap (mapped, sizeof (bridge::Shared));
        error = "fork failed";
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
            error = "the 32-bit bridge did not start (status " + String (status) + ")";
            return {};
        }
        futexWait (&shared->bridgeAlive, 0, 50);
    }

    auto connection = std::make_unique<Connection>();
    connection->shared = shared;
    connection->pid = pid;
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
}

// One command, one round trip. The host spins for the reply first (a block
// is usually back within tens of microseconds), then sleeps on the futex.
bool Vst2BridgeInstance::Connection::call (bridge::Command command, int timeoutMs)
{
    if (dead)
        return false;
    shared->command = command;
    const auto request = ++sequence;
    bridge::store (shared->request, request);
    futexWake (&shared->request);

    for (int spin = 0; spin < 20000; ++spin)
        if (bridge::load (shared->reply) == request)
            return true;
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
    shared->i0 = index;
    shared->text[0] = 0;
    if (! call (command))
        return {};
    shared->text[bridge::maxText - 1] = 0;
    return String::fromUTF8 (shared->text).trim();
}

float Vst2BridgeInstance::parameter (int index) const
{
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
}

void Vst2BridgeInstance::releaseResources()
{
    if (! active)
        return;
    shared->i0 = 0;
    call (bridge::Command::mains);
    active = false;
}

void Vst2BridgeInstance::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
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
    for (int c = 0; c < info.numInputs; ++c)
    {
        if (c < buffer.getNumChannels())
            std::memcpy (shared->audioIn[c], buffer.getReadPointer (c), sizeof (float) * (size_t) frames);
        else
            std::memset (shared->audioIn[c], 0, sizeof (float) * (size_t) frames);
    }

    // A real-time host would give up within the period; the probe allows
    // 200 ms so a slow block shows up as an overrun rather than a death.
    const bool ok = call (bridge::Command::process, 200);
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
    if (! call (bridge::Command::tailSize))
        return 0.0;
    return shared->result > 1 ? shared->result / rate : 0.0;
}

int Vst2BridgeInstance::getCurrentProgram()
{
    return call (bridge::Command::getProgram) ? shared->result : 0;
}

void Vst2BridgeInstance::setCurrentProgram (int index)
{
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
    state.append (stateTag, sizeof (stateTag));
    if ((shared->info.flags & vst2::flag::programChunks) != 0 && call (bridge::Command::getState) && shared->result == 1)
    {
        state.append (&stateChunk, 1);
        state.append (shared->data, shared->dataSize);
        return;
    }
    state.append (&stateParameters, 1);
    for (int i = 0; i < shared->info.numParams; ++i)
    {
        const float value = parameter (i);
        state.append (&value, sizeof (value));
    }
}

void Vst2BridgeInstance::setStateInformation (const void* data, int size)
{
    if (size < 5 || std::memcmp (data, stateTag, sizeof (stateTag)) != 0)
        return;
    const auto* bytes = static_cast<const uint8*> (data);
    const auto* body = bytes + 5;
    const auto bodySize = size - 5;
    if (bytes[4] == stateChunk)
    {
        if (bodySize <= (int) bridge::maxData)
        {
            std::memcpy (shared->data, body, (size_t) bodySize);
            shared->dataSize = (uint32_t) bodySize;
            call (bridge::Command::setState);
        }
        return;
    }
    for (int i = 0; i < std::min ((int) shared->info.numParams, bodySize / (int) sizeof (float)); ++i)
    {
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
