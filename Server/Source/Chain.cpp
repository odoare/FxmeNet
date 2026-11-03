/*
  ------------------------------------------------------------------------------
    Chain.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Chain.h"
#include "Log.h"

#include <FxmeNet/catalog/Buses.h>

namespace fxmenet::server
{

//==============================================================================
void SessionPlayHead::set (const wire::Transport& t, double sampleRate)
{
    info.setBpm (t.bpm);
    info.setTimeSignature (TimeSignature { t.timeSigNumerator, t.timeSigDenominator });
    info.setPpqPosition (t.ppqPosition);
    info.setPpqPositionOfLastBarStart (t.ppqLastBarStart);
    info.setTimeInSamples (t.timeInSamples);
    info.setTimeInSeconds (sampleRate > 0.0 ? (double) t.timeInSamples / sampleRate : 0.0);
    info.setIsPlaying (t.playing);
    info.setIsRecording (t.recording);
    info.setIsLooping (t.looping);
}

//==============================================================================
ParamWatcher::ParamWatcher (juce::AudioPluginInstance& i)
    : instance (i),
      values ((std::size_t) i.getParameters().size()),
      dirty ((std::size_t) i.getParameters().size())
{
    for (auto* p : instance.getParameters())
        p->addListener (this);
}

ParamWatcher::~ParamWatcher()
{
    for (auto* p : instance.getParameters())
        p->removeListener (this);
}

void ParamWatcher::parameterValueChanged (int index, float newValue)
{
    if (juce::isPositiveAndBelow (index, (int) values.size()))
    {
        values[(std::size_t) index].store (newValue);
        dirty[(std::size_t) index].store (true);
    }
}

void ParamWatcher::collect (std::uint32_t slotId, std::vector<control::ParamChange>& out)
{
    for (size_t i = 0; i < dirty.size(); ++i)
        if (dirty[i].exchange (false))
            out.push_back ({ slotId, (std::uint32_t) i, values[i].load() });
}

//==============================================================================
Chain::Slot::~Slot()
{
    watcher.reset();   // stop listening before the instance goes
    if (net != nullptr)
        net->release();
    if (instance != nullptr)
        instance->releaseResources();
}

Chain::Chain() = default;

Chain::~Chain()
{
    // The audio thread is stopped by now (the session's worker goes first).
    delete current.exchange (nullptr);
    slots.clear();
}

//==============================================================================
void Chain::prepare (double newSampleRate, int newBlockSize)
{
    // Called with the session's audio thread stopped.
    sampleRate = newSampleRate;
    blockSize = newBlockSize;
    for (auto& s : slots)
    {
        s->instance->releaseResources();
        s->instance->prepareToPlay (sampleRate, blockSize);
    }
}

juce::String Chain::addSlot (std::uint32_t slotId, int position,
                             std::unique_ptr<juce::AudioPluginInstance> instance,
                             const std::vector<std::uint8_t>& state,
                             control::SlotReady& ready)
{
    if (instance == nullptr)
        return "no instance";
    if (hasSlot (slotId))
        return "slot " + juce::String ((int) slotId) + " already exists";

    auto slot = std::make_shared<Slot>();
    slot->slotId = slotId;

    catalog::useDefaultBuses (*instance);   // not enableAllBuses: see Buses.h
    if (auto* vst3 = instance->getVST3Client())
        slot->net = fxme::net::queryNetPlugin (vst3->getIComponentPtr());

    // The role before prepareToPlay, as NetBridge expects.
    if (slot->net != nullptr)
        slot->net->setRole ((std::int32_t) fxme::net::Role::source);

    instance->setPlayHead (&playHead);
    if (! state.empty())
        instance->setStateInformation (state.data(), (int) state.size());
    instance->prepareToPlay (sampleRate, blockSize);

    slot->numChannels = juce::jmax (1, instance->getTotalNumInputChannels(),
                                    instance->getTotalNumOutputChannels());
    if (slot->numChannels > wire::maxChannels)
        return instance->getName() + " has more than " + juce::String (wire::maxChannels) + " channels";

    ready.slotId = slotId;
    ready.latency = instance->getLatencySamples();
    ready.numInputs = instance->getTotalNumInputChannels();
    ready.numOutputs = instance->getTotalNumOutputChannels();
    ready.params = describeParameters (*instance);
    ready.fxmenetAbi = 0;
    if (slot->net != nullptr)
    {
        ready.fxmenetAbi = (int) slot->net->getAbiVersion();
        const auto needed = -slot->net->getTelemetrySchema (nullptr, 0);
        if (needed > 0)
        {
            std::vector<std::uint8_t> schema ((std::size_t) needed);
            const auto n = slot->net->getTelemetrySchema (schema.data(), needed);
            if (n > 0)
                ready.telemetrySchema.append (schema.data(), (std::size_t) n);
        }
    }

    slot->instance = std::move (instance);
    slot->watcher = std::make_unique<ParamWatcher> (*slot->instance);   // after the state is loaded: no echo of it
    position = juce::jlimit (0, (int) slots.size(), position);
    slots.insert (slots.begin() + position, std::move (slot));
    publish();
    return {};
}

bool Chain::removeSlot (std::uint32_t slotId)
{
    const int i = indexOf (slotId);
    if (i < 0)
        return false;
    slots.erase (slots.begin() + i);
    publish();
    return true;
}

bool Chain::moveSlot (std::uint32_t slotId, int position)
{
    const int i = indexOf (slotId);
    if (i < 0)
        return false;
    auto slot = slots[(std::size_t) i];
    slots.erase (slots.begin() + i);
    position = juce::jlimit (0, (int) slots.size(), position);
    slots.insert (slots.begin() + position, std::move (slot));
    publish();
    return true;
}

bool Chain::setBypass (std::uint32_t slotId, bool shouldBypass)
{
    if (auto* s = findSlot (slotId))
    {
        s->bypassed = shouldBypass;
        return true;
    }
    return false;
}

void Chain::clear()
{
    slots.clear();
    publish();
}

Chain::Slot* Chain::findSlot (std::uint32_t slotId) const
{
    const int i = indexOf (slotId);
    return i < 0 ? nullptr : slots[(std::size_t) i].get();
}

int Chain::indexOf (std::uint32_t slotId) const
{
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i]->slotId == slotId)
            return (int) i;
    return -1;
}

void Chain::publish()
{
    int channels = 2;
    for (auto& s : slots)
        channels = juce::jmax (channels, s->numChannels);
    requiredChannels = channels;

    auto* old = current.exchange (new Snapshot { slots });
    if (old == nullptr)
        return;

    // Wait until the audio thread has let go of the old snapshot: at most
    // one block. If it never does (a plugin stuck in processBlock), leak it
    // rather than free memory in use.
    const auto start = juce::Time::getMillisecondCounter();
    while (inUse.load() == old)
    {
        if (juce::Time::getMillisecondCounter() - start > 2000)
        {
            log ("warning: the audio thread holds an old chain for over 2 s; leaking it");
            return;
        }
        juce::Thread::sleep (1);
    }
    delete old;   // the last owner of a removed slot destroys its instance here
}

//==============================================================================
void Chain::process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool offline)
{
    // Hazard pointer: announce the snapshot, then check it is still current,
    // so publish() cannot free it between the two.
    Snapshot* s = nullptr;
    for (;;)
    {
        s = current.load();
        inUse.store (s);
        if (current.load() == s)
            break;
    }

    if (s != nullptr)
    {
        int total = 0;
        for (auto& slot : s->slots)
        {
            auto& instance = *slot->instance;
            if (instance.isNonRealtime() != offline)
                instance.setNonRealtime (offline);
            const int n = juce::jmin (slot->numChannels, buffer.getNumChannels());
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), n, buffer.getNumSamples());

            if (slot->bypassed.load())
                instance.processBlockBypassed (view, midi);
            else
                instance.processBlock (view, midi);

            total += instance.getLatencySamples();
        }
        latency.store (total);
    }

    inUse.store (nullptr);
}

//==============================================================================
std::vector<control::ParamInfo> describeParameters (juce::AudioPluginInstance& instance)
{
    std::vector<control::ParamInfo> out;
    auto* bypass = instance.getBypassParameter();

    for (auto* p : instance.getParameters())
    {
        control::ParamInfo i;
        i.index = p->getParameterIndex();
        if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p))
            i.paramId = hosted->getParameterID();
        i.name = p->getName (256);
        i.label = p->getLabel();
        i.defaultValue = p->getDefaultValue();
        i.value = p->getValue();
        i.automatable = p->isAutomatable();
        i.isBypass = p == bypass;

        const int steps = p->getNumSteps();
        const bool stepped = (p->isDiscrete() || p->isBoolean()) && steps > 1
                             && steps != juce::AudioProcessor::getDefaultNumParameterSteps();
        i.numSteps = stepped ? steps : 0;
        if (stepped && steps <= 32)
            for (int k = 0; k < steps; ++k)
                i.stepTexts.add (p->getText ((float) k / (float) (steps - 1), 64));

        out.push_back (i);
    }
    return out;
}

} // namespace fxmenet::server
