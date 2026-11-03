/*
  ------------------------------------------------------------------------------
    PluginProcessor.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "AppSettings.h"

using fxmenet::client::ClientEngine;
using fxmenet::client::SlotModel;
using fxmenet::control::ParamChange;
using fxmenet::plugin::Mapping;
using fxmenet::plugin::PoolParameter;
using fxmenet::plugin::poolSize;

namespace
{
    constexpr int defaultControlPort = 54322;

    juce::String hex64 (std::uint64_t v)    { return fxmenet::control::json::hex64 (v); }

    std::uint64_t parseHex64 (const juce::String& s, std::uint64_t fallback)
    {
        return s.isEmpty() ? fallback : (std::uint64_t) s.getHexValue64();
    }

    /** "host" or "host:port". */
    std::pair<juce::String, int> splitServer (const juce::String& s)
    {
        const auto t = s.trim();
        if (t.containsChar (':'))
            return { t.upToLastOccurrenceOf (":", false, false),
                     t.fromLastOccurrenceOf (":", false, false).getIntValue() };
        return { t, defaultControlPort };
    }
}

//==============================================================================
FxmeNetAudioProcessor::FxmeNetAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    auto group = std::make_unique<juce::AudioProcessorParameterGroup> ("pool", "Parameters", "|");
    for (int i = 0; i < poolSize; ++i)
    {
        auto p = std::make_unique<PoolParameter> (i);
        pool[(std::size_t) i] = p.get();
        group->addChild (std::move (p));
    }
    addParameterGroup (std::move (group));

    engine.onParamChanged = [this] (std::uint32_t slotId, std::uint32_t index, float value)
    {
        const std::lock_guard<std::mutex> l (serverChangesLock);
        serverChanges.push_back ({ slotId, index, value });
    };

    startTimerHz (50);
}

FxmeNetAudioProcessor::~FxmeNetAudioProcessor()
{
    stopTimer();
}

//==============================================================================
bool FxmeNetAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const int in = layouts.getMainInputChannels(), out = layouts.getMainOutputChannels();
    return in >= 1 && in <= fxmenet::wire::maxChannels && out >= 1 && out <= fxmenet::wire::maxChannels;
}

void FxmeNetAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    StreamSettings s;
    {
        const std::lock_guard<std::mutex> l (settingsLock);
        s = stream;
        preparedRate = sampleRate;
        preparedBlock = samplesPerBlock;
        preparedInputs = getTotalNumInputChannels();
        preparedOutputs = getTotalNumOutputChannels();
    }

    ClientEngine::Settings e;
    e.blockSize = s.blockSize;
    e.budgetBlocks = s.budgetBlocks;
    e.inputMask = s.inputMask;
    e.outputMask = s.outputMask;
    engine.prepare (sampleRate, samplesPerBlock, preparedInputs, preparedOutputs, e);
    setLatencySamples (engine.getLatency());
}

void FxmeNetAudioProcessor::applyStreamSettings (const StreamSettings& s)
{
    double rate;
    int block, ins, outs;
    {
        const std::lock_guard<std::mutex> l (settingsLock);
        if (s == stream)
            return;   // nothing to do: no reconfiguration, no gap
        stream = s;
        rate = preparedRate;
        block = preparedBlock;
        ins = preparedInputs;
        outs = preparedOutputs;
    }
    if (rate <= 0.0)
        return;   // prepareToPlay will

    ClientEngine::Settings e;
    e.blockSize = s.blockSize;
    e.budgetBlocks = s.budgetBlocks;
    e.inputMask = s.inputMask;
    e.outputMask = s.outputMask;

    suspendProcessing (true);   // waits for a processBlock in progress
    engine.prepare (rate, block, ins, outs, e);
    suspendProcessing (false);
    setLatencySamples (engine.getLatency());
}

//==============================================================================
void FxmeNetAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    process (buffer, false);
}

void FxmeNetAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    process (buffer, true);   // dry, delayed by the latency the host compensates
}

void FxmeNetAudioProcessor::process (juce::AudioBuffer<float>& buffer, bool bypassed)
{
    juce::ScopedNoDenormals noDenormals;
    const int ins = getTotalNumInputChannels(), outs = getTotalNumOutputChannels();
    if (ins != preparedInputs || outs != preparedOutputs || buffer.getNumChannels() < juce::jmax (ins, outs))
    {
        buffer.clear();   // a layout change not yet prepared
        return;
    }

    fxmenet::wire::Transport transport;
    const fxmenet::wire::Transport* transportPointer = nullptr;
    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
        {
            transport.bpm = position->getBpm().orFallback (120.0);
            transport.ppqPosition = position->getPpqPosition().orFallback (0.0);
            transport.ppqLastBarStart = position->getPpqPositionOfLastBarStart().orFallback (0.0);
            transport.timeInSamples = position->getTimeInSamples().orFallback (0);
            if (const auto loop = position->getLoopPoints())
                transport.loopStartPpq = loop->ppqStart;
            if (const auto sig = position->getTimeSignature())
            {
                transport.timeSigNumerator = (std::uint16_t) sig->numerator;
                transport.timeSigDenominator = (std::uint16_t) sig->denominator;
            }
            transport.playing = position->getIsPlaying();
            transport.recording = position->getIsRecording();
            transport.looping = position->getIsLooping();
            transportPointer = &transport;
        }

    for (int c = 0; c < ins; ++c)
        inPointers[(std::size_t) c] = buffer.getReadPointer (c);
    for (int c = 0; c < outs; ++c)
        outPointers[(std::size_t) c] = buffer.getWritePointer (c);

    engine.process (inPointers.data(), outPointers.data(), buffer.getNumSamples(),
                    isNonRealtime(), bypassed, transportPointer);
}

//==============================================================================
void FxmeNetAudioProcessor::timerCallback()
{
    applyServerParameterChanges();
    sendDirtyParameters();
    followChain();

    if (engine.getLatency() != getLatencySamples())
        setLatencySamples (engine.getLatency());

    if (engine.getStatus() != ClientEngine::Status::ready)
        catalogRequested = false;
    else if (! catalogRequested.exchange (true))
        engine.requestCatalog();
}

void FxmeNetAudioProcessor::sendDirtyParameters()
{
    std::vector<ParamChange> changes;
    int learned = -1;
    {
        const std::lock_guard<std::mutex> l (mappingLock);
        for (int i = 0; i < poolSize; ++i)
        {
            auto* p = pool[(std::size_t) i];
            if (! p->takeDirty())
                continue;
            const auto m = mapping[(std::size_t) i];
            const float v = p->get();
            if (juce::exactlyEqual (v, lastSent[(std::size_t) i]))
                continue;   // our own echo (a value from the server, a sync)

            // Learning: the first pool parameter the DAW moves gets the target.
            if (learnTarget.isMapped() && learned < 0)
            {
                learned = i;
                continue;
            }
            if (! m.isMapped())
                continue;
            lastSent[(std::size_t) i] = v;
            changes.push_back ({ m.slotId, m.paramIndex, v });
        }
    }
    if (! changes.empty())
        engine.setParameters (changes);

    if (learned >= 0)
    {
        const auto target = std::exchange (learnTarget, Mapping {});
        mapPool (learned, target);
    }
}

void FxmeNetAudioProcessor::applyServerParameterChanges()
{
    std::vector<ParamChange> changes;
    {
        const std::lock_guard<std::mutex> l (serverChangesLock);
        changes.swap (serverChanges);
    }

    for (auto& c : changes)
        for (int i = 0; i < poolSize; ++i)
        {
            bool match;
            {
                const std::lock_guard<std::mutex> l (mappingLock);
                match = mapping[(std::size_t) i] == Mapping { c.slotId, c.index };
            }
            if (match)
            {
                lastSent[(std::size_t) i] = c.value;   // not sent back
                pool[(std::size_t) i]->setValueNotifyingHost (c.value);
            }
        }
}

/** Keeps the pool in step with the chain: mappings of removed slots cleared,
    a newly loaded slot's parameters mapped to free pool entries, names and
    values taken from each slot once it is loaded. */
void FxmeNetAudioProcessor::followChain()
{
    const auto slots = engine.getSlotInfos();

    juce::String signature;
    for (auto& s : slots)
        signature << (int) s.slotId << (s.loaded ? "L" : "-") << ";";
    if (signature == lastSlotsSignature)
        return;
    lastSlotsSignature = signature;

    std::set<std::uint32_t> present, loaded;
    for (auto& s : slots)
    {
        present.insert (s.slotId);
        if (s.loaded)
            loaded.insert (s.slotId);
    }

    if (learnTarget.isMapped() && present.count (learnTarget.slotId) == 0)
        learnTarget = {};   // its slot is gone

    bool infoChanged = false;
    std::vector<std::pair<int, float>> values;   // set outside the lock
    {
        const std::lock_guard<std::mutex> l (mappingLock);

        for (int i = 0; i < poolSize; ++i)
        {
            auto& m = mapping[(std::size_t) i];
            if (m.isMapped() && present.count (m.slotId) == 0)
            {
                m = {};
                pool[(std::size_t) i]->describe ({}, nullptr);
                infoChanged = true;
            }
        }

        for (auto& s : slots)
        {
            if (! s.loaded || autoMapped.count (s.slotId) != 0)
                continue;
            autoMapped.insert (s.slotId);

            int free = 0;
            for (auto& p : s.info.params)
            {
                if (! p.automatable || p.isBypass)
                    continue;
                while (free < poolSize && mapping[(std::size_t) free].isMapped())
                    ++free;
                if (free == poolSize)
                    break;   // the pool is full: the rest stays unmapped
                mapping[(std::size_t) free] = { s.slotId, (std::uint32_t) p.index };
                infoChanged = true;
            }
        }

        for (auto& s : slots)
        {
            if (! s.loaded || syncedSlots.count (s.slotId) != 0)
                continue;
            syncedSlots.insert (s.slotId);

            for (int i = 0; i < poolSize; ++i)
            {
                const auto m = mapping[(std::size_t) i];
                if (m.slotId != s.slotId)
                    continue;
                for (auto& p : s.info.params)
                    if ((std::uint32_t) p.index == m.paramIndex)
                    {
                        pool[(std::size_t) i]->describe (s.name, &p);
                        const auto it = s.params.find (m.paramIndex);
                        values.push_back ({ i, it != s.params.end() ? it->second.value : p.value });
                        infoChanged = true;
                        break;
                    }
            }
        }

        for (auto it = syncedSlots.begin(); it != syncedSlots.end();)
            it = loaded.count (*it) == 0 ? syncedSlots.erase (it) : std::next (it);
    }

    for (auto [i, v] : values)
    {
        lastSent[(std::size_t) i] = v;
        pool[(std::size_t) i]->setValueNotifyingHost (v);
    }
    if (infoChanged)
        updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));
}

//==============================================================================
void FxmeNetAudioProcessor::connectTo (const juce::String& hostAndPort)
{
    {
        const std::lock_guard<std::mutex> l (settingsLock);
        server = hostAndPort.trim();
    }
    const auto [host, port] = splitServer (hostAndPort);
    if (host.isEmpty())
    {
        engine.disconnect();
        return;
    }
    fxmenet::plugin::setLastServer (hostAndPort.trim());
    engine.connectTo (host, port > 0 ? port : defaultControlPort);
}

juce::String FxmeNetAudioProcessor::getServer() const
{
    const std::lock_guard<std::mutex> l (settingsLock);
    return server;
}

void FxmeNetAudioProcessor::addPlugin (const fxmenet::control::PluginEntry& entry)
{
    engine.addSlot (entry.uid, entry.name, entry.vendor, (int) engine.getSlots().size());
}

bool FxmeNetAudioProcessor::findCatalogEntry (const SlotModel& s, fxmenet::control::PluginEntry& out) const
{
    for (auto& e : engine.getCatalog())
        if (e.uid == s.uid && e.name == s.name && e.vendor == s.vendor)
        {
            out = e;
            return true;
        }
    return false;
}

int FxmeNetAudioProcessor::getNumMapped() const
{
    const std::lock_guard<std::mutex> l (mappingLock);
    int n = 0;
    for (auto& m : mapping)
        n += m.isMapped() ? 1 : 0;
    return n;
}

//==============================================================================
FxmeNetAudioProcessor::StreamSettings FxmeNetAudioProcessor::getStreamSettings() const
{
    const std::lock_guard<std::mutex> l (settingsLock);
    return stream;
}

void FxmeNetAudioProcessor::setStreamSettings (const StreamSettings& s)
{
    applyStreamSettings (s);
}

int FxmeNetAudioProcessor::getPreparedBlockSize() const   { const std::lock_guard<std::mutex> l (settingsLock); return preparedBlock; }
int FxmeNetAudioProcessor::getPreparedInputs() const      { const std::lock_guard<std::mutex> l (settingsLock); return preparedInputs; }
int FxmeNetAudioProcessor::getPreparedOutputs() const     { const std::lock_guard<std::mutex> l (settingsLock); return preparedOutputs; }

//==============================================================================
Mapping FxmeNetAudioProcessor::getMapping (int poolIndex) const
{
    const std::lock_guard<std::mutex> l (mappingLock);
    return juce::isPositiveAndBelow (poolIndex, poolSize) ? mapping[(std::size_t) poolIndex] : Mapping {};
}

int FxmeNetAudioProcessor::findPoolFor (Mapping target) const
{
    const std::lock_guard<std::mutex> l (mappingLock);
    for (int i = 0; i < poolSize; ++i)
        if (mapping[(std::size_t) i] == target)
            return i;
    return -1;
}

float FxmeNetAudioProcessor::describeMapped (int poolIndex, const std::vector<SlotModel>& slots)
{
    const auto m = mapping[(std::size_t) poolIndex];
    for (auto& s : slots)
        if (s.slotId == m.slotId && s.loaded)
            for (auto& p : s.info.params)
                if ((std::uint32_t) p.index == m.paramIndex)
                {
                    pool[(std::size_t) poolIndex]->describe (s.name, &p);
                    return ClientEngine::currentValue (s, p);
                }
    pool[(std::size_t) poolIndex]->describe ({}, nullptr);   // named once its slot loads
    return -1.0f;
}

void FxmeNetAudioProcessor::mapPool (int poolIndex, Mapping target)
{
    if (! juce::isPositiveAndBelow (poolIndex, poolSize) || ! target.isMapped())
        return;

    const auto slots = engine.getSlotInfos();
    float value;
    {
        const std::lock_guard<std::mutex> l (mappingLock);
        for (int j = 0; j < poolSize; ++j)
            if (j != poolIndex && mapping[(std::size_t) j] == target)
            {
                mapping[(std::size_t) j] = {};
                pool[(std::size_t) j]->describe ({}, nullptr);
            }
        mapping[(std::size_t) poolIndex] = target;
        value = describeMapped (poolIndex, slots);
    }

    if (value >= 0.0f)
    {
        lastSent[(std::size_t) poolIndex] = value;   // shows the parameter's value, sends nothing
        pool[(std::size_t) poolIndex]->setValueNotifyingHost (value);
    }
    updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));
}

void FxmeNetAudioProcessor::unmapPool (int poolIndex)
{
    if (! juce::isPositiveAndBelow (poolIndex, poolSize))
        return;
    {
        const std::lock_guard<std::mutex> l (mappingLock);
        mapping[(std::size_t) poolIndex] = {};
        pool[(std::size_t) poolIndex]->describe ({}, nullptr);
    }
    updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));
}

int FxmeNetAudioProcessor::mapToFreePool (Mapping target)
{
    if (const int existing = findPoolFor (target); existing >= 0)
        return existing;

    int free = -1;
    {
        const std::lock_guard<std::mutex> l (mappingLock);
        for (int i = 0; i < poolSize && free < 0; ++i)
            if (! mapping[(std::size_t) i].isMapped())
                free = i;
    }
    if (free >= 0)
        mapPool (free, target);
    return free;
}

void FxmeNetAudioProcessor::startLearn (Mapping target)   { learnTarget = target; }
void FxmeNetAudioProcessor::cancelLearn()                 { learnTarget = {}; }

void FxmeNetAudioProcessor::setSlotParameter (Mapping target, float value)
{
    if (const int i = findPoolFor (target); i >= 0)
        pool[(std::size_t) i]->setValueNotifyingHost (value);   // sent by the timer, recorded by the DAW
    else
        engine.setParameter (target.slotId, target.paramIndex, value);
}

void FxmeNetAudioProcessor::beginSlotParameterGesture (Mapping target)
{
    if (const int i = findPoolFor (target); i >= 0)
        pool[(std::size_t) i]->beginChangeGesture();
}

void FxmeNetAudioProcessor::endSlotParameterGesture (Mapping target)
{
    if (const int i = findPoolFor (target); i >= 0)
        pool[(std::size_t) i]->endChangeGesture();
}

//==============================================================================
juce::AudioProcessorEditor* FxmeNetAudioProcessor::createEditor()
{
    return new FxmeNetAudioProcessorEditor (*this);
}

void FxmeNetAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // The slots' states live on the server: bring them back first. Kept as
    // they were when the server does not answer in time.
    engine.refreshStates (2000);

    juce::XmlElement root ("FxmeNet");
    root.setAttribute ("stateVersion", stateVersion);
    {
        const std::lock_guard<std::mutex> l (settingsLock);
        root.setAttribute ("server", server);
        root.setAttribute ("blockSize", stream.blockSize);
        root.setAttribute ("budgetBlocks", stream.budgetBlocks);
        root.setAttribute ("inputMask", hex64 (stream.inputMask));
        root.setAttribute ("outputMask", hex64 (stream.outputMask));
    }

    for (auto& s : engine.getSlots())
    {
        auto* e = root.createNewChildElement ("SLOT");
        e->setAttribute ("id", (int) s.slotId);
        e->setAttribute ("uid", fxmenet::control::json::hex32 (s.uid));
        e->setAttribute ("name", s.name);
        e->setAttribute ("vendor", s.vendor);
        e->setAttribute ("bypassed", s.bypassed);
        e->setAttribute ("state", juce::Base64::toBase64 (s.state.data(), s.state.size()));
        for (auto& [index, v] : s.params)
        {
            auto* p = e->createNewChildElement ("PARAM");
            p->setAttribute ("index", (int) index);
            p->setAttribute ("value", (double) v.value);
        }
    }

    {
        const std::lock_guard<std::mutex> l (mappingLock);
        for (int i = 0; i < poolSize; ++i)
            if (const auto m = mapping[(std::size_t) i]; m.isMapped())
            {
                auto* e = root.createNewChildElement ("MAP");
                e->setAttribute ("pool", i);
                e->setAttribute ("slot", (int) m.slotId);
                e->setAttribute ("param", (int) m.paramIndex);
            }
    }

    copyXmlToBinary (root, destData);
}

void FxmeNetAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // Version 1 is the first: nothing to convert yet.
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName ("FxmeNet"))
        return;

    std::vector<SlotModel> slots;
    for (auto* e : xml->getChildWithTagNameIterator ("SLOT"))
    {
        SlotModel s;
        s.slotId = (std::uint32_t) e->getIntAttribute ("id");
        s.uid = (std::uint32_t) e->getStringAttribute ("uid").getHexValue32();
        s.name = e->getStringAttribute ("name");
        s.vendor = e->getStringAttribute ("vendor");
        s.bypassed = e->getBoolAttribute ("bypassed");
        juce::MemoryOutputStream state;
        if (juce::Base64::convertFromBase64 (state, e->getStringAttribute ("state")))
            s.state.assign ((const std::uint8_t*) state.getData(), (const std::uint8_t*) state.getData() + state.getDataSize());
        for (auto* p : e->getChildWithTagNameIterator ("PARAM"))
            s.params[(std::uint32_t) p->getIntAttribute ("index")] = { (float) p->getDoubleAttribute ("value"), 0 };
        if (s.slotId != 0)
            slots.push_back (std::move (s));
    }

    {
        const std::lock_guard<std::mutex> l (mappingLock);
        mapping.fill ({});
        for (auto* e : xml->getChildWithTagNameIterator ("MAP"))
        {
            const int i = e->getIntAttribute ("pool", -1);
            if (juce::isPositiveAndBelow (i, poolSize))
                mapping[(std::size_t) i] = { (std::uint32_t) e->getIntAttribute ("slot"),
                                             (std::uint32_t) e->getIntAttribute ("param") };
        }
        for (auto* p : pool)
            p->describe ({}, nullptr);   // named again as their slots load
        autoMapped.clear();
        for (auto& s : slots)
            autoMapped.insert (s.slotId);   // the saved mapping is the user's: not redone
        syncedSlots.clear();
        lastSlotsSignature = {};
    }
    updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));

    engine.setSlots (std::move (slots));

    StreamSettings s;
    s.blockSize = xml->getIntAttribute ("blockSize", s.blockSize);
    s.budgetBlocks = xml->getIntAttribute ("budgetBlocks", s.budgetBlocks);
    s.inputMask = parseHex64 (xml->getStringAttribute ("inputMask"), s.inputMask);
    s.outputMask = parseHex64 (xml->getStringAttribute ("outputMask"), s.outputMask);
    applyStreamSettings (s);

    connectTo (xml->getStringAttribute ("server"));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FxmeNetAudioProcessor();
}
