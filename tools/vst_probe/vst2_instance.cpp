// SPDX-License-Identifier: AGPL-3.0-only
#include "vst2_instance.h"

#include <cstring>
#include <dlfcn.h>

using namespace juce;

namespace
{
// The instance being opened: VSTPluginMain and effOpen call the host before
// the effect pointer is known to us.
thread_local Vst2Instance* opening = nullptr;
thread_local double openingRate = 0.0;
thread_local int openingBlock = 0;

vst2::EntryPoint entryPointOf (void* library)
{
    if (auto* main = dlsym (library, "VSTPluginMain"))
        return reinterpret_cast<vst2::EntryPoint> (main);
    return reinterpret_cast<vst2::EntryPoint> (dlsym (library, "main"));
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

class Vst2Instance::Parameter final : public AudioPluginInstance::HostedParameter
{
public:
    Parameter (Vst2Instance& owner, int index)
        : owner (owner), index (index), initial (owner.effect->getParameter (owner.effect, index)) {}

    float getValue() const override { return owner.effect->getParameter (owner.effect, index); }
    void setValue (float value) override { owner.effect->setParameter (owner.effect, index, value); }
    float getDefaultValue() const override { return initial; }
    String getName (int maximum) const override { return owner.dispatchString (vst2::op::getParamName, index).substring (0, maximum); }
    String getLabel() const override { return owner.dispatchString (vst2::op::getParamLabel, index); }
    String getParameterID() const override { return String (index); }
    float getValueForText (const String& text) const override { return text.getFloatValue(); }

    // VST2 can only display the current value.
    String getText (float value, int maximum) const override
    {
        if (std::abs (value - getValue()) < 1.0e-6f)
            return owner.dispatchString (vst2::op::getParamDisplay, index).trim().substring (0, maximum);
        return String (value, 3);
    }

private:
    Vst2Instance& owner;
    const int index;
    const float initial;
};

AudioProcessor::BusesProperties Vst2Instance::busesOf (const vst2::Effect& effect)
{
    BusesProperties buses;
    if (effect.numInputs > 0)
        buses = buses.withInput ("Input", channelSet (effect.numInputs), true);
    if (effect.numOutputs > 0)
        buses = buses.withOutput ("Output", channelSet (effect.numOutputs), true);
    return buses;
}

bool Vst2Instance::exportsEntryPoint (const File& file, String& error)
{
    void* library = dlopen (file.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        error = dlerror();
        return false;
    }
    const bool found = entryPointOf (library) != nullptr;
    if (! found)
        error = "no VSTPluginMain";
    dlclose (library);
    return found;
}

std::unique_ptr<Vst2Instance> Vst2Instance::load (const File& file, double rate, int block, String& error)
{
    void* library = dlopen (file.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        error = dlerror();
        return {};
    }
    auto entry = entryPointOf (library);
    if (entry == nullptr)
    {
        error = "no VSTPluginMain";
        dlclose (library);
        return {};
    }
    openingRate = rate;
    openingBlock = block;
    auto* effect = entry (&Vst2Instance::hostCallback);
    if (effect == nullptr || effect->magic != vst2::magic)
    {
        error = "VSTPluginMain returned no effect";
        dlclose (library);
        return {};
    }
    if ((effect->flags & vst2::flag::canReplace) == 0 || effect->processReplacing == nullptr)
    {
        error = "plug-in has no processReplacing";
        effect->dispatcher (effect, vst2::op::close, 0, 0, nullptr, 0.0f);
        dlclose (library);
        return {};
    }
    return std::unique_ptr<Vst2Instance> (new Vst2Instance (library, effect, file, rate, block));
}

Vst2Instance::Vst2Instance (void* libraryToOwn, vst2::Effect* effectToOwn, const File& fileLoaded, double r, int b)
    : AudioPluginInstance (busesOf (*effectToOwn)), library (libraryToOwn), effect (effectToOwn),
      file (fileLoaded), rate (r), block (b)
{
    effect->reserved2 = reinterpret_cast<intptr_t> (this);
    opening = this;
    dispatch (vst2::op::open);
    dispatch (vst2::op::setSampleRate, 0, 0, nullptr, (float) rate);
    dispatch (vst2::op::setBlockSize, 0, block);
    opening = nullptr;

    name = dispatchString (vst2::op::getEffectName);
    if (name.isEmpty())
        name = dispatchString (vst2::op::getProductString);
    if (name.isEmpty())
        name = file.getFileNameWithoutExtension();
    vendor = dispatchString (vst2::op::getVendorString);
    midiIn = (effect->flags & vst2::flag::isSynth) != 0
          || dispatch (vst2::op::canDo, 0, 0, (void*) "receiveVstMidiEvent") > 0;

    for (int i = 0; i < effect->numParams; ++i)
        addHostedParameter (std::make_unique<Parameter> (*this, i));

    timeInfo.tempo = 120.0;
    timeInfo.timeSigNumerator = timeInfo.timeSigDenominator = 4;
    midiEvents.resize (std::size (events.events));
    for (size_t i = 0; i < midiEvents.size(); ++i)
        events.events[i] = &midiEvents[i];
}

Vst2Instance::~Vst2Instance()
{
    if (active)
        releaseResources();
    dispatch (vst2::op::close);
    dlclose (library);
}

intptr_t Vst2Instance::dispatch (int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) const
{
    return effect->dispatcher (effect, opcode, index, value, ptr, opt);
}

String Vst2Instance::dispatchString (int32_t opcode, int32_t index) const
{
    char text[256] = {};
    dispatch (opcode, index, 0, text);
    text[sizeof (text) - 1] = 0;
    return String::fromUTF8 (text).trim();
}

intptr_t Vst2Instance::hostCallback (vst2::Effect* effect, int32_t opcode, int32_t, intptr_t, void* ptr, float)
{
    auto* self = effect != nullptr && effect->reserved2 != 0 ? reinterpret_cast<Vst2Instance*> (effect->reserved2) : opening;
    const auto copy = [ptr] (const char* text) { if (ptr != nullptr) std::strcpy ((char*) ptr, text); return (intptr_t) 1; };

    switch (opcode)
    {
        case vst2::host::version: return vst2::hostVersion;
        case vst2::host::currentId: return effect != nullptr ? effect->uniqueId : 0;
        case vst2::host::getSampleRate: return (intptr_t) (self != nullptr ? self->rate : openingRate);
        case vst2::host::getBlockSize: return self != nullptr ? self->block : openingBlock;
        case vst2::host::getTime: return self != nullptr ? (intptr_t) &self->timeInfo : 0;
        case vst2::host::getVendorString: return copy ("Melodee");
        case vst2::host::getProductString: return copy ("Melodee");
        case vst2::host::getVendorVersion: return 1;
        case vst2::host::getLanguage: return 1;
        case vst2::host::getCurrentProcessLevel: return 0;
        case vst2::host::canDo:
        {
            const char* asked = (const char*) ptr;
            for (const char* yes : { "sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents", "receiveVstMidiEvent" })
                if (asked != nullptr && std::strcmp (asked, yes) == 0)
                    return 1;
            return 0;
        }
        case vst2::host::automate:
        case vst2::host::idle:
        case vst2::host::ioChanged:
        case vst2::host::updateDisplay:
        case vst2::host::beginEdit:
        case vst2::host::endEdit:
            return 0;
        default:
            if (self != nullptr && opcode >= 0 && opcode < (int32_t) self->unanswered.size())
                self->unanswered[(size_t) opcode] = true;
            return 0;
    }
}

String Vst2Instance::unansweredHostOpcodes() const
{
    StringArray list;
    for (size_t i = 0; i < unanswered.size(); ++i)
        if (unanswered[i])
            list.add (String ((int) i));
    return list.joinIntoString (", ");
}

void Vst2Instance::fillInPluginDescription (PluginDescription& description) const
{
    description.name = description.descriptiveName = name;
    description.pluginFormatName = "VST";
    description.category = (effect->flags & vst2::flag::isSynth) != 0 ? "Synth" : "Effect";
    description.manufacturerName = vendor;
    description.version = String (effect->version);
    description.fileOrIdentifier = file.getFullPathName();
    description.uniqueId = description.deprecatedUid = effect->uniqueId;
    description.isInstrument = (effect->flags & vst2::flag::isSynth) != 0;
    description.numInputChannels = effect->numInputs;
    description.numOutputChannels = effect->numOutputs;
}

void Vst2Instance::prepareToPlay (double newRate, int newBlock)
{
    if (active)
        releaseResources();
    rate = newRate;
    block = newBlock;
    setRateAndBufferSizeDetails (rate, block);
    dispatch (vst2::op::setSampleRate, 0, 0, nullptr, (float) rate);
    dispatch (vst2::op::setBlockSize, 0, block);

    inputs.assign ((size_t) std::max (1, effect->numInputs) * (size_t) block, 0.0f);
    outputs.assign ((size_t) std::max (1, effect->numOutputs) * (size_t) block, 0.0f);
    inputPointers.resize ((size_t) std::max (1, effect->numInputs));
    outputPointers.resize ((size_t) std::max (1, effect->numOutputs));
    for (size_t c = 0; c < inputPointers.size(); ++c)
        inputPointers[c] = inputs.data() + c * (size_t) block;
    for (size_t c = 0; c < outputPointers.size(); ++c)
        outputPointers[c] = outputs.data() + c * (size_t) block;

    timeInfo.sampleRate = rate;
    timeInfo.samplePos = timeInfo.ppqPos = 0.0;
    dispatch (vst2::op::mainsChanged, 0, 1);
    dispatch (vst2::op::startProcess);
    active = true;
}

void Vst2Instance::releaseResources()
{
    if (! active)
        return;
    dispatch (vst2::op::stopProcess);
    dispatch (vst2::op::mainsChanged, 0, 0);
    active = false;
}

void Vst2Instance::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    const int frames = std::min (buffer.getNumSamples(), block);

    int count = 0;
    for (const auto metadata : midi)
    {
        if (count == (int) midiEvents.size() || metadata.numBytes > 3 || metadata.samplePosition >= frames)
            continue;
        auto& event = midiEvents[(size_t) count++];
        event = {};
        event.type = vst2::midiEventType;
        event.byteSize = sizeof (vst2::MidiEvent);
        event.deltaFrames = metadata.samplePosition;
        std::memcpy (event.data, metadata.data, (size_t) metadata.numBytes);
    }
    if (count > 0)
    {
        events.count = count;
        dispatch (vst2::op::processEvents, 0, 0, &events);
    }

    timeInfo.flags = vst2::time::playing | vst2::time::tempoValid | vst2::time::ppqValid | vst2::time::timeSigValid;
    for (int c = 0; c < effect->numInputs; ++c)
    {
        if (c < buffer.getNumChannels())
            std::memcpy (inputPointers[(size_t) c], buffer.getReadPointer (c), sizeof (float) * (size_t) frames);
        else
            std::memset (inputPointers[(size_t) c], 0, sizeof (float) * (size_t) frames);
    }
    effect->processReplacing (effect, inputPointers.data(), outputPointers.data(), frames);
    for (int c = 0; c < buffer.getNumChannels(); ++c)
    {
        if (c < effect->numOutputs)
            buffer.copyFrom (c, 0, outputPointers[(size_t) c], frames);
        else
            buffer.clear (c, 0, frames);
    }

    timeInfo.samplePos += frames;
    timeInfo.ppqPos = timeInfo.samplePos / rate * timeInfo.tempo / 60.0;
}

double Vst2Instance::getTailLengthSeconds() const
{
    const auto samples = dispatch (vst2::op::getTailSize);
    return samples > 1 ? (double) samples / rate : 0.0;
}

int Vst2Instance::getCurrentProgram()
{
    return (int) dispatch (vst2::op::getProgram);
}

void Vst2Instance::setCurrentProgram (int index)
{
    dispatch (vst2::op::setProgram, 0, index);
}

const String Vst2Instance::getProgramName (int index)
{
    char text[256] = {};
    if (dispatch (vst2::op::getProgramNameIndexed, index, -1, text) == 0 && index == getCurrentProgram())
        dispatch (vst2::op::getProgramName, 0, 0, text);
    text[sizeof (text) - 1] = 0;
    return String::fromUTF8 (text).trim();
}

// Saved as "MVS2", a kind byte, then the plug-in's own chunk or every
// parameter's value as floats.
void Vst2Instance::getStateInformation (MemoryBlock& state)
{
    state.reset();
    state.append (stateTag, sizeof (stateTag));
    if ((effect->flags & vst2::flag::programChunks) != 0)
    {
        void* data = nullptr;
        const auto size = dispatch (vst2::op::getChunk, 0, 0, &data);
        state.append (&stateChunk, 1);
        if (data != nullptr && size > 0)
            state.append (data, (size_t) size);
        return;
    }
    state.append (&stateParameters, 1);
    for (int i = 0; i < effect->numParams; ++i)
    {
        const float value = effect->getParameter (effect, i);
        state.append (&value, sizeof (value));
    }
}

void Vst2Instance::setStateInformation (const void* data, int size)
{
    if (size < 5 || std::memcmp (data, stateTag, sizeof (stateTag)) != 0)
        return;
    const auto* bytes = static_cast<const uint8*> (data);
    const auto kind = bytes[4];
    const auto* body = bytes + 5;
    const auto bodySize = size - 5;
    if (kind == stateChunk)
    {
        dispatch (vst2::op::setChunk, 0, bodySize, const_cast<uint8*> (body));
        return;
    }
    for (int i = 0; i < std::min (effect->numParams, bodySize / (int) sizeof (float)); ++i)
    {
        float value;
        std::memcpy (&value, body + i * (int) sizeof (float), sizeof (value));
        effect->setParameter (effect, i, value);
    }
}
