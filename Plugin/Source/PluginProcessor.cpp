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

    juce::addDefaultFormatsToManager (shadowFormats);

    engine.onParamChanged = [this] (std::uint32_t slotId, std::uint32_t index, float value)
    {
        const std::lock_guard<std::mutex> l (serverChangesLock);
        serverChanges.push_back ({ slotId, index, value });
    };

    engine.setFlushOnReset (fxmenet::plugin::getFlushOnReset());
    logEvents = fxmenet::plugin::getLogEvents();
    startTimerHz (50);
}

FxmeNetAudioProcessor::~FxmeNetAudioProcessor()
{
    stopTimer();
    shadows.clear();   // the editor, and the shadows' views, are gone already
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
void FxmeNetAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    process (buffer, false, &midi);
}

void FxmeNetAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    process (buffer, true);   // dry, delayed by the latency the host compensates
}

void FxmeNetAudioProcessor::process (juce::AudioBuffer<float>& buffer, bool bypassed, const juce::MidiBuffer* midi)
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

    // The host's MIDI, travelling with the blocks that hold it (bypassed:
    // nothing is sent, so neither is MIDI).
    int numMidi = 0;
    if (midi != nullptr)
        for (const auto m : *midi)
        {
            if (numMidi == maxMidiEvents)
                break;
            midiEvents[(std::size_t) numMidi++] = { m.samplePosition, m.numBytes, m.data };
        }

    engine.process (inPointers.data(), outPointers.data(), buffer.getNumSamples(),
                    isNonRealtime(), bypassed, transportPointer, midiEvents.data(), numMidi);
}

//==============================================================================
void FxmeNetAudioProcessor::handleAudioEvents()
{
    if (++settingsPollTicks >= 25)   // twice a second: another instance may have changed them
    {
        settingsPollTicks = 0;
        engine.setFlushOnReset (fxmenet::plugin::getFlushOnReset());
        logEvents = fxmenet::plugin::getLogEvents();
    }

    juce::String lines;
    const auto tag = juce::String::toHexString ((juce::pointer_sized_int) this).getLastCharacters (5);
    engine.drainEvents ([&] (const ClientEngine::AudioEvent& e)
    {
        if (! logEvents)
            return;
        using E = ClientEngine::AudioEvent;
        auto line = juce::Time::getCurrentTime().formatted ("%H:%M:%S.")
                  + juce::String (juce::Time::getCurrentTime().getMilliseconds()).paddedLeft ('0', 3)
                  + " [" + tag + "] " + juce::String (e.wallMs, 1) + " ms  callback "
                  + juce::String ((juce::int64) e.callback);
        if ((e.bits & E::resetSeen) != 0)
            line << "  RESET (" << juce::String (e.resetDelayMs, 1) << " ms after reset()"
                 << (e.flushed ? ", flushed)" : ", not flushed)");
        if ((e.bits & E::playChange) != 0)
            line << (e.playing ? "  STOP->PLAY" : "  PLAY->STOP");
        if ((e.bits & E::jump) != 0)
            line << "  JUMP " << (juce::int64) (e.time - e.expected) << " samples";
        if ((e.bits & E::concealed) != 0)
            line << "  CONCEALED " << (int) e.concealedBlocks;
        line << "  (play head " << (juce::int64) e.time << ", " << e.numSamples << " samples, host ahead "
             << juce::String (e.aheadMicros / 1000.0, 1) << " ms)";
        lines << line << "\n";
    });

    if (lines.isNotEmpty())
    {
        const auto file = fxmenet::plugin::getEventsLogFile();
        file.getParentDirectory().createDirectory();
        juce::FileOutputStream out (file);   // appends
        if (out.openedOk())
            out.writeText (lines, false, false, nullptr);
    }
}

void FxmeNetAudioProcessor::timerCallback()
{
    applyServerParameterChanges();
    sendDirtyParameters();
    followChain();
    syncShadows();
    deliverTelemetry();
    handleAudioEvents();

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
    if (lastTouched.isMapped() && present.count (lastTouched.slotId) == 0)
        lastTouched = {};

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

            for (auto& p : s.info.params)
            {
                if (! p.automatable || p.isBypass)
                    continue;
                const int free = pickFreePool();
                if (free < 0)
                    break;   // the pool is full: the rest stays unmapped
                mapping[(std::size_t) free] = { s.slotId, (std::uint32_t) p.index };
                everUsed[(std::size_t) free] = true;
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

//==============================================================================
fxmenet::plugin::Shadow* FxmeNetAudioProcessor::findShadow (std::uint32_t slotId) const
{
    const auto it = shadows.find (slotId);
    return it != shadows.end() ? it->second.get() : nullptr;
}

fxmenet::plugin::Shadow* FxmeNetAudioProcessor::getShadow (std::uint32_t slotId, juce::String* why)
{
    if (auto* s = findShadow (slotId))
        return s;

    auto fail = [why] (const juce::String& reason) -> fxmenet::plugin::Shadow*
    {
        if (why != nullptr)
            *why = reason;
        return nullptr;
    };

    SlotModel slot;
    bool found = false;
    for (auto& s : engine.getSlotInfos())
        if (s.slotId == slotId)
        {
            slot = s;
            found = true;
        }
    if (! found || ! slot.loaded)
        return fail ("not loaded on the server");

    fxmenet::control::PluginEntry entry;
    if (! findCatalogEntry (slot, entry))
        return fail ("not in the server's catalog");
    const auto m = matchLocal (entry);
    if (m.kind != fxmenet::plugin::LocalMatch::Kind::compatible)
        return fail (m.text);

    // The server's rate, and its block size: editors may read both.
    double rate;
    {
        const std::lock_guard<std::mutex> l (settingsLock);
        rate = preparedRate > 0.0 ? preparedRate : 48000.0;
    }
    juce::String error;
    auto shadow = fxmenet::plugin::Shadow::create (shadowFormats, m.local, rate, getStreamSettings().blockSize, error);
    if (shadow == nullptr)
        return fail ("cannot load the local copy: " + error);

    if (shadow->getNumParameters() != (int) slot.info.params.size())
        return fail ("the local copy has " + juce::String (shadow->getNumParameters()) + " parameters, the server's "
                     + juce::String ((int) slot.info.params.size()));

    // Its editor moves parameters as the generic view does.
    shadow->onParameterChanged = [this, slotId] (int index, float value)
    {
        lastTouched = { slotId, (std::uint32_t) index };
        setSlotParameter ({ slotId, (std::uint32_t) index }, value);
    };
    shadow->onGesture = [this, slotId] (int index, bool starting)
    {
        lastTouched = { slotId, (std::uint32_t) index };
        if (starting)
            beginSlotParameterGesture ({ slotId, (std::uint32_t) index });
        else
            endSlotParameterGesture ({ slotId, (std::uint32_t) index });
    };

    // The server's state, then the values set since (syncShadows).
    auto* raw = shadow.get();
    shadows[slotId] = std::move (shadow);
    juce::WeakReference<fxmenet::plugin::Shadow> weak (raw);
    engine.fetchState (slotId, [weak] (std::vector<std::uint8_t> state)
    {
        juce::MessageManager::callAsync ([weak, state]
        {
            if (auto* s = weak.get())
            {
                if (! state.empty())
                    s->getInstance().setStateInformation (state.data(), (int) state.size());
                s->lastStateChangeCount = s->getStateChangeCount();   // our own load is no change
                s->stateReceived = true;
            }
        });
    });
    return raw;
}

/** The server's telemetry frames into the shadows' meters and spectra
    (FxmeNet-aware plugins: INetPlugin::writeTelemetry, in the order they
    came). Frames for a slot without a shadow are dropped. */
void FxmeNetAudioProcessor::deliverTelemetry()
{
    for (auto& [slotId, frames] : engine.takeTelemetry())
        if (auto* shadow = findShadow (slotId))
            if (auto* net = shadow->getNet())
                for (auto& f : frames)
                    net->writeTelemetry (f.data(), (std::int32_t) f.size());
}

void FxmeNetAudioProcessor::removeShadow (std::uint32_t slotId)
{
    if (findShadow (slotId) == nullptr)
        return;
    if (beforeShadowRemoved)
        beforeShadowRemoved (slotId);
    shadows.erase (slotId);
}

/** Keeps the shadows in step: values from the server, the pool or the
    generic view shown in them; their state changes sent to the server;
    shadows of removed or unloaded slots deleted. */
void FxmeNetAudioProcessor::syncShadows()
{
    if (shadows.empty())
        return;

    const auto slots = engine.getSlotInfos();
    std::vector<std::uint32_t> gone;
    for (auto& [id, shadow] : shadows)
    {
        const SlotModel* slot = nullptr;
        for (auto& s : slots)
            if (s.slotId == id)
                slot = &s;
        if (slot == nullptr || ! slot->loaded)
        {
            gone.push_back (id);
            continue;
        }
        if (! shadow->stateReceived)
            continue;   // its state is on its way

        for (auto& p : slot->info.params)
            shadow->setQuietly (p.index, ClientEngine::currentValue (*slot, p));

        // A non-parameter change in its editor (FxmeNet-aware plugins say so).
        const auto count = shadow->getStateChangeCount();
        if (count != shadow->lastStateChangeCount)
        {
            shadow->lastStateChangeCount = count;
            juce::MemoryBlock block;
            shadow->getInstance().getStateInformation (block);
            engine.setSlotState (id, std::vector<std::uint8_t> ((const std::uint8_t*) block.getData(),
                                                                (const std::uint8_t*) block.getData() + block.getSize()));
        }
    }
    for (auto id : gone)
        removeShadow (id);
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
        everUsed[(std::size_t) poolIndex] = true;
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

    int free;
    {
        const std::lock_guard<std::mutex> l (mappingLock);
        free = pickFreePool();
    }
    if (free >= 0)
        mapPool (free, target);
    return free;
}

int FxmeNetAudioProcessor::pickFreePool() const
{
    int freed = -1;
    for (int i = 0; i < poolSize; ++i)
    {
        if (mapping[(std::size_t) i].isMapped())
            continue;
        if (! everUsed[(std::size_t) i])
            return i;
        if (freed < 0)
            freed = i;
    }
    return freed;   // all used once: a freed one, its automation lanes with it
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
        juce::String used;   // one hex digit per four entries
        for (int i = 0; i < poolSize; i += 4)
        {
            int nibble = 0;
            for (int b = 0; b < 4; ++b)
                nibble |= everUsed[(std::size_t) (i + b)] ? (1 << b) : 0;
            used << juce::String::toHexString (nibble);
        }
        root.setAttribute ("usedPool", used);

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
        everUsed.fill (false);
        for (auto* e : xml->getChildWithTagNameIterator ("MAP"))
        {
            const int i = e->getIntAttribute ("pool", -1);
            if (juce::isPositiveAndBelow (i, poolSize))
            {
                mapping[(std::size_t) i] = { (std::uint32_t) e->getIntAttribute ("slot"),
                                             (std::uint32_t) e->getIntAttribute ("param") };
                everUsed[(std::size_t) i] = true;   // also for states saved before "usedPool"
            }
        }
        const auto used = xml->getStringAttribute ("usedPool");
        for (int d = 0; d < used.length() && d * 4 < poolSize; ++d)
        {
            const int nibble = juce::String::charToString (used[d]).getHexValue32();
            for (int b = 0; b < 4; ++b)
                if ((nibble >> b) & 1)
                    everUsed[(std::size_t) (d * 4 + b)] = true;
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
