/*
  ------------------------------------------------------------------------------
    PluginProcessor.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "PluginProcessor.h"

#include <FxmeNet/chain/Describe.h>
#if FXMENET_JSFX
 #include <FxmeNet/jsfx/JsfxInstance.h>
#endif
#include "PluginEditor.h"
#include "AppSettings.h"

#include <FxmeNet/chain/RoutingEngine.h>   // wantedBuses, as on the server

using fxmenet::client::ClientEngine;
using fxmenet::client::SlotModel;
using fxmenet::control::ParamChange;
using fxmenet::plugin::Mapping;
using fxmenet::plugin::PoolParameter;
using fxmenet::plugin::poolSize;

namespace
{
    constexpr int defaultControlPort = 54322;

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
    : AudioProcessor (FxmeNetAudioProcessor::defaultBuses())
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

    // Local play: a JSFX display's mirror fed by the local script.
    fxmenet::client::ClientEngine::LocalJsfx hooks;
    hooks.reset = [this] (std::uint32_t slotId)
    {
       #if FXMENET_JSFX
        if (auto* js = findLocalScript (slotId))
            js->syncReset();
       #endif
        localDisplays.erase (slotId);   // the pump starts it again
    };
    hooks.subscribe = [this] (std::uint32_t slotId, const std::vector<std::uint32_t>& blocks)
    {
       #if FXMENET_JSFX
        if (auto* js = findLocalScript (slotId))
            js->syncSubscribe (blocks);
       #else
        juce::ignoreUnused (slotId, blocks);
       #endif
    };
    hooks.write = [this] (const std::vector<std::uint8_t>& payload)
    {
       #if FXMENET_JSFX
        fxmenet::jsfx::SyncFrame w;
        if (fxmenet::jsfx::decode (payload.data(), payload.size(), w) && ! w.reset)
            if (auto* js = findLocalScript (w.slotId))
                js->syncWrite (w);
       #else
        juce::ignoreUnused (payload);
       #endif
    };
    engine.setLocalJsfx (std::move (hooks));

    engine.setFlushOnReset (fxmenet::plugin::getFlushOnReset());
    logEvents = fxmenet::plugin::getLogEvents();
    startTimerHz (50);
}

FxmeNetAudioProcessor::~FxmeNetAudioProcessor()
{
    stopTimer();
    engine.setLocalProcessor (nullptr);
    localChain.publish ({}, nullptr);   // nothing points to the shadows any more
    shadows.clear();   // the editor, and the shadows' views, are gone already
}

//==============================================================================
juce::AudioProcessor::BusesProperties FxmeNetAudioProcessor::defaultBuses()
{
    // The instrument has no audio input (doc/instrument-plan.md): the
    // session then sends none, and the chain is fed silence.
   #if FXMENET_INSTRUMENT
    return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
   #else
    return BusesProperties().withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                            .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
   #endif
}

bool FxmeNetAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const int in = layouts.getMainInputChannels(), out = layouts.getMainOutputChannels();
   #if FXMENET_INSTRUMENT
    const int minIn = 0;
   #else
    const int minIn = 1;
   #endif
    return in >= minIn && in <= fxmenet::wire::maxChannels && out >= 1 && out <= fxmenet::wire::maxChannels;
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
    engine.prepare (sampleRate, samplesPerBlock, preparedInputs, preparedOutputs, e);
    prepareLocal (sampleRate, s.blockSize);
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

    suspendProcessing (true);   // waits for a processBlock in progress
    engine.prepare (rate, block, ins, outs, e);
    prepareLocal (rate, s.blockSize);
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

    // An export starting: its warning, if any, is the next one's.
    const bool rendering = isNonRealtime();
    if (rendering && ! wasRendering)
        renderWarning = {};
    wasRendering = rendering;

    // The local chain's load, over the last second.
    localPeakMicros = juce::jmax (localPeakMicros, localChain.takePeakMicros());
    if (const auto now = juce::Time::getMillisecondCounter(); now - localLoadMs >= 1000)
    {
        double rate;
        {
            const std::lock_guard<std::mutex> l (settingsLock);
            rate = preparedRate > 0.0 ? preparedRate : 48000.0;
        }
        const double blockMicros = 1.0e6 * getStreamSettings().blockSize / rate;
        localLoad = 100.0 * localPeakMicros / blockMicros;
        localPeakMicros = 0;
        localLoadMs = now;
    }

    followAuto();
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
        signature << (int) s.slotId << (isDescribed (s) ? "L" : "-") << ";";
    if (signature == lastSlotsSignature)
        return;
    lastSlotsSignature = signature;

    std::set<std::uint32_t> present, loaded;
    for (auto& s : slots)
    {
        present.insert (s.slotId);
        if (isDescribed (s))
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
            if (! isDescribed (s) || autoMapped.count (s.slotId) != 0)
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
            if (! isDescribed (s) || syncedSlots.count (s.slotId) != 0)
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
    if (host.isEmpty() || processing == Processing::local)
    {
        engine.disconnect();   // local play: the address is kept for later
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

bool FxmeNetAudioProcessor::addScript (const fxmenet::jsfx::Script& script, juce::String& error, juce::StringArray* notes)
{
    fxmenet::jsfx::Bundle bundle;
    fxmenet::jsfx::BundleBuilder builder (script.effectsFolder);
    builder.convertAudio = fxmenet::jsfx::convertAudioForJsfx;
    if (! builder.build (script.path, bundle, error, notes))
        return false;
    engine.addSlot (fxmenet::jsfx::jsfxUid, script.path, fxmenet::jsfx::jsfxVendor,
                    (int) engine.getSlots().size(), fxmenet::jsfx::makeSlotState (std::move (bundle)));
    return true;
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

    if (playingLocal)   // local copies are made by syncLocal
    {
        if (why != nullptr)
        {
            const auto problem = getLocalProblem (slotId);
            *why = problem.isNotEmpty() ? "played locally without it: " + problem : juce::String ("its local copy is loading");
        }
        return nullptr;
    }

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
    if (isJsfx (slot))
        return fail ("a JSFX script");

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

    // The model's state at once (the editor does not open on the defaults),
    // then the server's when it comes, and the values set since (syncShadows).
    // (`slot` comes from getSlotInfos: no state blob in it.)
    if (const auto state = engine.getSlotState (slotId); ! state.empty())
        shadow->getInstance().setStateInformation (state.data(), (int) state.size());
    for (auto& [index, v] : slot.params)
        shadow->setQuietly ((int) index, v.value);

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
                if (s->getNet() == nullptr)
                    s->takeStateReference();
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
    if (playingLocal)
    {
        publishLocal (slotId);   // the audio thread lets go of it first
        localSignature = {};
        localDisplays.erase (slotId);
    }
    if (beforeShadowRemoved)
        beforeShadowRemoved (slotId);
    shadows.erase (slotId);
}

/** Keeps the shadows in step: values from the server, the pool or the
    generic view shown in them; their state changes sent to the server;
    shadows of removed or unloaded slots deleted. */
void FxmeNetAudioProcessor::syncShadows()
{
    if (playingLocal)
    {
        syncLocal();
        return;
    }

    // Auto: every slot's copy is ready, so that losing the server only
    // changes their role (goLocal). Once per slot (a failure is kept).
    if (processing == Processing::automatic && preloadCopies)
        for (auto& s : engine.getSlotInfos())
            if (s.loaded && ! isJsfx (s) && findShadow (s.slotId) == nullptr && localProblems.count (s.slotId) == 0)
            {
                juce::String why;
                if (getShadow (s.slotId, &why) == nullptr && why.isNotEmpty())
                    localProblems[s.slotId] = why;
            }

    if (shadows.empty())
        return;

    const bool pollTick = ++statePollTick >= statePollTicks;
    if (pollTick)
        statePollTick = 0;

    const auto slots = engine.getSlotInfos();
    const bool routed = engine.isRoutingAvailable();
    const auto routing = routed ? engine.getServerRouting() : std::vector<fxmenet::chain::Connection> {};
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

        // The same buses as on the server (a sidechain connected or not).
        if (routed && slot->hasBox)
        {
            std::vector<bool> in, out;
            fxmenet::chain::wantedBuses (slot->box, routing, in, out);
            shadow->setWantedBuses (in, out);
        }

        // A non-parameter change in its editor: FxmeNet-aware plugins say
        // so; the others are watched while their editor is open, and once
        // when it closes (Shadow.h).
        juce::MemoryBlock block;
        bool changed = false;
        if (shadow->getNet() != nullptr)
        {
            const auto count = shadow->getStateChangeCount();
            if (count != shadow->lastStateChangeCount)
            {
                shadow->lastStateChangeCount = count;
                shadow->getInstance().getStateInformation (block);
                changed = true;
            }
        }
        else
        {
            const bool open = shadow->getInstance().getActiveEditor() != nullptr;
            if ((open && pollTick) || (! open && shadow->editorWasOpen))
                changed = shadow->takeStateChange (block);
            shadow->editorWasOpen = open;
        }
        if (changed)
            engine.setSlotState (id, std::vector<std::uint8_t> ((const std::uint8_t*) block.getData(),
                                                                (const std::uint8_t*) block.getData() + block.getSize()));
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
        if (s.slotId == m.slotId && isDescribed (s))
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
    // they were when the server does not answer in time. Local play: the
    // local copies' states are in the model already (syncLocal).
    if (! playingLocal)
        engine.refreshStates (2000);

    juce::XmlElement root ("FxmeNet");
    root.setAttribute ("stateVersion", stateVersion);
    {
        const std::lock_guard<std::mutex> l (settingsLock);
        root.setAttribute ("server", server);
        root.setAttribute ("blockSize", stream.blockSize);
        root.setAttribute ("budgetBlocks", stream.budgetBlocks);
    }
    root.setAttribute ("processing", processing == Processing::local ? "local"
                                   : processing == Processing::automatic ? "auto" : "server");
    root.setAttribute ("autoReturn", autoReturn);
    root.setAttribute ("autoPreload", preloadCopies);

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
        if (s.hasBox)   // its buses: the routing holds before the server answers
            for (const bool input : { true, false })
                for (auto& b : input ? s.box.inputs : s.box.outputs)
                {
                    auto* bus = e->createNewChildElement ("BUS");
                    bus->setAttribute ("input", input);
                    bus->setAttribute ("name", juce::String (b.name));
                    bus->setAttribute ("channels", b.channels);
                    bus->setAttribute ("main", b.main);
                }
    }

    // The routing: the default rule's options, and only the pins the user
    // set (the rest follows the rule).
    {
        const auto options = engine.getRoutingOptions();
        root.setAttribute ("routeAround", options.around);
        root.setAttribute ("routeExtraOutputs", options.extraOutputs);
    }
    for (auto& m : engine.getRoutingPins())
    {
        auto* e = root.createNewChildElement ("ROUTE");
        e->setAttribute ("box", fxmenet::control::json::hex32 (m.input.box));
        e->setAttribute ("channel", m.input.channel);
        for (auto& src : m.sources)
        {
            auto* f = e->createNewChildElement ("FROM");
            f->setAttribute ("box", fxmenet::control::json::hex32 (src.box));
            f->setAttribute ("channel", src.channel);
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
        for (auto* b : e->getChildWithTagNameIterator ("BUS"))
        {
            fxmenet::chain::Bus bus { b->getStringAttribute ("name").toStdString(),
                                      juce::jlimit (0, 1024, b->getIntAttribute ("channels")), b->getBoolAttribute ("main") };
            (b->getBoolAttribute ("input") ? s.box.inputs : s.box.outputs).push_back (bus);
            s.hasBox = true;
        }
        s.box.id = s.slotId;
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

    std::vector<fxmenet::chain::ManualInput> pins;
    for (auto* e : xml->getChildWithTagNameIterator ("ROUTE"))
    {
        fxmenet::chain::ManualInput m;
        m.input = { (std::uint32_t) e->getStringAttribute ("box").getHexValue32(), e->getIntAttribute ("channel") };
        for (auto* f : e->getChildWithTagNameIterator ("FROM"))
            m.sources.push_back ({ (std::uint32_t) f->getStringAttribute ("box").getHexValue32(), f->getIntAttribute ("channel") });
        pins.push_back (std::move (m));
    }
    fxmenet::chain::Options options;
    options.around = xml->getBoolAttribute ("routeAround", options.around);
    options.extraOutputs = xml->getBoolAttribute ("routeExtraOutputs", options.extraOutputs);
    engine.setRoutingOptions (options);
    engine.setRoutingPins (pins);

    StreamSettings s;
    s.blockSize = xml->getIntAttribute ("blockSize", s.blockSize);
    s.budgetBlocks = xml->getIntAttribute ("budgetBlocks", s.budgetBlocks);
    applyStreamSettings (s);

    {
        const std::lock_guard<std::mutex> l (settingsLock);
        server = xml->getStringAttribute ("server").trim();
    }
    autoReturn = xml->getBoolAttribute ("autoReturn", true);
    preloadCopies = xml->getBoolAttribute ("autoPreload", true);
    const auto mode = xml->getStringAttribute ("processing");
    setProcessing (mode == "local" ? Processing::local : mode == "auto" ? Processing::automatic : Processing::server);
    connectTo (xml->getStringAttribute ("server"));
}

//==============================================================================
// Local play (doc/fallback-plan.md).

void FxmeNetAudioProcessor::setProcessing (Processing p)
{
    if (p == processing)
        return;

    // The side leaving holds the plugins' current state (presets, files,
    // every value): into the model, which the side arriving starts from, so
    // that nothing jumps back to an older state, or to the defaults.
    if (playingLocal)
    {
        for (auto& [id, shadow] : shadows)
        {
            juce::MemoryBlock block;
            shadow->getInstance().getStateInformation (block);
            engine.setSlotState (id, std::vector<std::uint8_t> ((const std::uint8_t*) block.getData(),
                                                                (const std::uint8_t*) block.getData() + block.getSize()));
        }
    }
    else if (! (engine.getStatus() == ClientEngine::Status::ready && engine.refreshStates (2000)))
    {
        for (auto& [id, shadow] : shadows)   // no server: its mirrors, as last shown
            if (shadow->stateReceived)
            {
                juce::MemoryBlock block;
                shadow->getInstance().getStateInformation (block);
                engine.setSlotState (id, std::vector<std::uint8_t> ((const std::uint8_t*) block.getData(),
                                                                    (const std::uint8_t*) block.getData() + block.getSize()));
            }
    }

    // The shadows change role: the current ones go (out of the local chain
    // first), the next ones are made as needed.
    engine.setLocalProcessor (nullptr);
    localChain.publish ({}, nullptr);
    playingLocal = false;   // removeShadow: no local chain to update meanwhile
    std::vector<std::uint32_t> ids;
    for (auto& [id, shadow] : shadows)
        ids.push_back (id);
    for (auto id : ids)
        removeShadow (id);
    localProblems.clear();
    localSignature = {};
    localDisplays.clear();   // a script's display starts again from its new source
    serverBack = returnAsked = false;
    lostSince = backSince = 0;

    processing = p;
    if (p == Processing::local)
    {
        engine.disconnect();
        playingLocal = true;
        engine.setLocalProcessor (&localChain);
        syncLocal();
    }
    else
    {
        connectTo (getServer());
    }
    setLatencySamples (engine.getLatency());
}

void FxmeNetAudioProcessor::followAuto()
{
    if (processing != Processing::automatic)
        return;
    const auto now = juce::Time::getMillisecondCounter();
    const bool ready = engine.isSessionReady();

    // Blocks sent and replies received: a session up but silent is lost too
    // (a cable pulled: the control connection notices later).
    const auto& st = engine.getStatistics();
    const auto sent = st.sent.load(), received = st.received.load();
    if (sent != requestsSeen)
        requestsMs = now;
    if (received != repliesSeen)
        repliesMs = now;
    requestsSeen = sent;
    repliesSeen = received;

    if (! playingLocal)
    {
        const bool silent = now - requestsMs < 200 && now - repliesMs > lostAfterMs;
        const bool down = ! ready && engine.getStatus() != ClientEngine::Status::building;
        if (silent || down)
        {
            if (lostSince == 0)
                lostSince = now;
            if (silent || now - lostSince >= lostAfterMs)
                goLocal();
        }
        else
        {
            lostSince = 0;
        }
        return;
    }

    if (engine.isReturning())
    {
        if (engine.isReturnDone())
            finishReturn();
        return;
    }

    if (ready)
    {
        if (backSince == 0)
            backSince = now;
    }
    else
    {
        backSince = 0;
    }
    serverBack = backSince != 0 && now - backSince >= backAfterMs;
    if (serverBack && (autoReturn || returnAsked))
        startReturn();
}

void FxmeNetAudioProcessor::noteSwitch (const juce::String& what)
{
    if (isNonRealtime())
        renderWarning = "This export switched sides (" + what + "): check it, or export again";
}

void FxmeNetAudioProcessor::goLocal()
{
    noteSwitch ("server lost");
    // The copies loaded already (syncShadows, or the editors') take the local
    // role; the others are loaded now. The chain plays once all are there:
    // silence meanwhile rather than a chain missing some of its plugins.
    for (auto& [id, shadow] : shadows)
        shadow->setLocal (true);
    playingLocal = true;
    lostSince = backSince = 0;
    serverBack = returnAsked = false;
    localProblems.clear();
    localSignature = {};
    localDisplays.clear();
    syncLocal();   // the missing copies, and the chain published
    engine.setLocalProcessor (&localChain);
    setLatencySamples (engine.getLatency());
}

void FxmeNetAudioProcessor::startReturn()
{
    noteSwitch ("back to the server");
    // The server gets the copies' state first (its chain was rebuilt from
    // the model when it came back), then the crossfade, about 50 ms.
    for (auto& [id, shadow] : shadows)
    {
        juce::MemoryBlock block;
        shadow->getInstance().getStateInformation (block);
        engine.setSlotState (id, std::vector<std::uint8_t> ((const std::uint8_t*) block.getData(),
                                                            (const std::uint8_t*) block.getData() + block.getSize()));
    }
    double rate;
    {
        const std::lock_guard<std::mutex> l (settingsLock);
        rate = preparedRate > 0.0 ? preparedRate : 48000.0;
    }
    const int n = juce::jmax (1, getStreamSettings().blockSize);
    engine.startReturnToServer (juce::jmax (2, (int) std::ceil (0.05 * rate / n)));
    returnAsked = false;
}

void FxmeNetAudioProcessor::finishReturn()
{
    engine.setLocalProcessor (nullptr);
    localChain.publish ({}, nullptr);   // the audio thread no longer touches the copies
    playingLocal = false;
    std::vector<std::uint32_t> scripts;
    for (auto& [id, shadow] : shadows)
    {
       #if FXMENET_JSFX
        if (dynamic_cast<fxmenet::jsfx::JsfxInstance*> (&shadow->getInstance()) != nullptr)
        {
            scripts.push_back (id);
            continue;
        }
       #endif
        shadow->setLocal (false);   // a mirror again
    }
    for (auto id : scripts)
        removeShadow (id);              // a script's display is the server's again
    localProblems.clear();
    localSignature = {};
    localDisplays.clear();
    serverBack = false;
    backSince = 0;
    for (auto& s : engine.getSlotInfos())
        if (isJsfx (s))
            engine.restartJsfxSync (s.slotId);
    if (! preloadCopies)
        dropUnshownCopies();
    setLatencySamples (engine.getLatency());
}

void FxmeNetAudioProcessor::setPreloadCopies (bool preload)
{
    preloadCopies = preload;
    if (! preload && processing == Processing::automatic && ! playingLocal)
        dropUnshownCopies();
    localProblems.clear();   // preloading again: try every slot
}

void FxmeNetAudioProcessor::dropUnshownCopies()
{
    if (playingLocal)
        return;
    std::vector<std::uint32_t> unshown;
    for (auto& [id, shadow] : shadows)
        if (shadow->getInstance().getActiveEditor() == nullptr)
            unshown.push_back (id);
    for (auto id : unshown)
        removeShadow (id);
}

juce::String FxmeNetAudioProcessor::getLocalProblem (std::uint32_t slotId) const
{
    const auto it = localProblems.find (slotId);
    return it != localProblems.end() ? it->second : juce::String();
}

void FxmeNetAudioProcessor::prepareLocal (double sampleRate, int streamBlock)
{
    for (auto& [id, shadow] : shadows)
        shadow->prepare (sampleRate, streamBlock);
    localChain.prepare (streamBlock);
}

fxmenet::plugin::Shadow* FxmeNetAudioProcessor::createLocalShadow (const SlotModel& slot, juce::String& why)
{
    why = {};
    double rate;
    {
        const std::lock_guard<std::mutex> l (settingsLock);
        rate = preparedRate > 0.0 ? preparedRate : 48000.0;
    }
    const int block = getStreamSettings().blockSize;
    std::unique_ptr<fxmenet::plugin::Shadow> shadow;
    bool script = false;

    if (isJsfx (slot))
    {
       #if FXMENET_JSFX
        // The script travels in the slot's state: loaded here as the server would.
        juce::String error;
        auto instance = fxmenet::jsfx::JsfxInstance::create (engine.getSlotState (slot.slotId), rate, error);
        if (instance == nullptr)
        {
            why = "the script does not load here: " + error;
            return nullptr;
        }
        shadow = fxmenet::plugin::Shadow::fromInstance (std::move (instance), rate, block, true);
        script = true;
       #else
        why = "a JSFX script (this build has no JSFX engine)";
        return nullptr;
       #endif
    }
    else
    {
    // The server's catalog says which version; without it, any version here.
    fxmenet::control::PluginEntry entry;
    if (! findCatalogEntry (slot, entry))
    {
        entry.uid = slot.uid;
        entry.name = slot.name;
        entry.vendor = slot.vendor;
    }
    const auto m = matchLocal (entry);
    if (m.kind == fxmenet::plugin::LocalMatch::Kind::unknown)
        return nullptr;   // the local scan is not finished: asked again later (why stays empty)
    if (m.kind == fxmenet::plugin::LocalMatch::Kind::missing)
    {
        why = "not installed here";
        return nullptr;
    }

    juce::String error;
    shadow = fxmenet::plugin::Shadow::create (shadowFormats, m.local, rate, block, error, true);
    if (shadow == nullptr)
    {
        why = "cannot load it: " + error;
        return nullptr;
    }
    }

    if (! slot.info.params.empty() && shadow->getNumParameters() != (int) slot.info.params.size())
    {
        why = "it has " + juce::String (shadow->getNumParameters()) + " parameters here, "
            + juce::String ((int) slot.info.params.size()) + " on the server";
        return nullptr;
    }

    const auto slotId = slot.slotId;
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

    // The model's state (the last the server gave, or this computer's), then
    // the values set since. (`slot` comes from getSlotInfos: no state blob.)
    // A script was made from it already.
    if (const auto state = engine.getSlotState (slot.slotId); ! state.empty() && ! script)
        shadow->getInstance().setStateInformation (state.data(), (int) state.size());
    for (auto& [index, v] : slot.params)
        shadow->setQuietly ((int) index, v.value);
    shadow->lastStateChangeCount = shadow->getStateChangeCount();
    if (shadow->getNet() == nullptr)
        shadow->takeStateReference();
    shadow->stateReceived = true;

    // Its parameters and buses, when no server has described the slot: the
    // generic controls, the mapping and the routing work without one.
    auto info = fxmenet::chain::describeSlot (slotId, shadow->getInstance());
   #if FXMENET_JSFX
    if (auto* js = dynamic_cast<fxmenet::jsfx::JsfxInstance*> (&shadow->getInstance()))
    {
        // As the server describes a script: its sliders' units and lists.
        info.params = fxmenet::chain::describeParameters (*js, [] (juce::AudioProcessorParameter& p, fxmenet::control::ParamInfo& i)
        {
            if (auto* ranged = dynamic_cast<fxmenet::jsfx::RangedParameter*> (&p))
            {
                i.hasRange = ranged->getRange (i.rangeMin, i.rangeMax, i.rangeStep);
                i.choices = ranged->isChoice();
                i.hidden = ranged->isHidden();
            }
        });
        info.jsfx = true;
        info.jsfxGfx = js->hasGfx();
    }
   #endif
    engine.setLocalInfo (slotId, std::move (info));

    auto* raw = shadow.get();
    shadows[slotId] = std::move (shadow);
    return raw;
}

fxmenet::jsfx::JsfxInstance* FxmeNetAudioProcessor::findLocalScript (std::uint32_t slotId) const
{
   #if FXMENET_JSFX
    if (playingLocal)
        if (auto* shadow = findShadow (slotId))
            return dynamic_cast<fxmenet::jsfx::JsfxInstance*> (&shadow->getInstance());
   #else
    juce::ignoreUnused (slotId);
   #endif
    return nullptr;
}

void FxmeNetAudioProcessor::pumpLocalJsfx()
{
   #if FXMENET_JSFX
    // As the server's Session::pumpJsfxSync, into the display's queue.
    for (auto& [id, shadow] : shadows)
    {
        auto* js = dynamic_cast<fxmenet::jsfx::JsfxInstance*> (&shadow->getInstance());
        if (js == nullptr || ! js->hasGfx())
            continue;
        if (! engine.isTelemetryWanted (id))
        {
            localDisplays.erase (id);
            continue;
        }
        if (localDisplays.insert (id).second)
            js->syncReset();   // a display just shown: everything anew
        if (js->syncCollect (id, fxmenet::jsfx::defaultFrameBudget, localJsfxFrame))
        {
            fxmenet::jsfx::encode (localJsfxFrame, localJsfxPayload);
            engine.pushJsfxSync (id, localJsfxPayload);
        }
    }
   #endif
}

void FxmeNetAudioProcessor::publishLocal (std::uint32_t without)
{
    const auto slots = engine.getSlotInfos();
    bool complete = false;
    const auto routing = engine.getLocalRouting (complete);
    int ins, outs, streamBlock;
    {
        const std::lock_guard<std::mutex> l (settingsLock);
        ins = juce::jmax (1, preparedInputs);
        outs = juce::jmax (1, preparedOutputs);
        streamBlock = stream.blockSize;
    }

    // A slot keeps its object while its copy is the same (its bypass state
    // goes on: no release of notes for nothing).
    std::vector<fxmenet::plugin::LocalChain::SlotPtr> list;
    std::vector<fxmenet::chain::SlotLayout> layouts;
    for (auto& slot : slots)
    {
        auto* shadow = slot.slotId == without ? nullptr : findShadow (slot.slotId);
        auto* instance = shadow != nullptr ? &shadow->getInstance() : nullptr;

        fxmenet::plugin::LocalChain::SlotPtr ls;
        for (auto& old : localChain.getSlots())
            if (old->slotId == slot.slotId && old->processor == instance)
                ls = old;
        if (ls == nullptr)
        {
            ls = std::make_shared<fxmenet::plugin::LocalChain::Slot>();
            ls->slotId = slot.slotId;
            ls->processor = instance;
        }
        ls->bypassed = slot.bypassed;
        ls->numChannels = instance != nullptr
                            ? juce::jmax (1, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels())
                            : 2;
        list.push_back (ls);

        if (complete)
        {
            fxmenet::chain::SlotLayout l;
            l.box = slot.box;
            for (const bool input : { true, false })
            {
                const auto& buses = input ? slot.box.inputs : slot.box.outputs;
                for (int i = 0; i < (int) buses.size(); ++i)
                {
                    int width = buses[(std::size_t) i].channels;   // bridged: every bus
                    if (instance != nullptr)
                    {
                        auto* bus = instance->getBus (input, i);
                        width = bus != nullptr && bus->isEnabled() ? bus->getNumberOfChannels() : 0;
                    }
                    (input ? l.inputWidths : l.outputWidths).push_back (width);
                }
            }
            l.latency = instance != nullptr ? instance->getLatencySamples() : 0;
            layouts.push_back (std::move (l));
        }
    }

    std::unique_ptr<fxmenet::chain::RoutingPlan> plan;
    if (complete)
        plan = std::make_unique<fxmenet::chain::RoutingPlan> (ins, outs, layouts, routing, streamBlock, localChain.getPlan());
    localChain.publish (list, std::move (plan));
}

void FxmeNetAudioProcessor::syncLocal()
{
    const auto slots = engine.getSlotInfos();
    const bool pollTick = ++statePollTick >= statePollTicks;
    if (pollTick)
        statePollTick = 0;

    // Copies of slots gone.
    std::vector<std::uint32_t> gone;
    for (auto& [id, shadow] : shadows)
        if (std::none_of (slots.begin(), slots.end(), [id = id] (const SlotModel& s) { return s.slotId == id; }))
            gone.push_back (id);
    for (auto id : gone)
    {
        removeShadow (id);
        localProblems.erase (id);
    }

    // A copy for each slot (once: a slot that cannot have one is bridged).
    for (auto& slot : slots)
        if (findShadow (slot.slotId) == nullptr && localProblems.count (slot.slotId) == 0)
        {
            juce::String why;
            if (createLocalShadow (slot, why) == nullptr && why.isNotEmpty())
                localProblems[slot.slotId] = why;
        }

    // Parameters, buses and states in step with the model.
    bool complete = false;
    const auto routing = engine.getLocalRouting (complete);
    std::vector<std::uint32_t> busChanges;
    for (auto& slot : slots)
    {
        auto* shadow = findShadow (slot.slotId);
        if (shadow == nullptr)
            continue;

        for (auto& [index, v] : slot.params)
            shadow->setQuietly ((int) index, v.value);

        if (complete && slot.hasBox)
        {
            std::vector<bool> in, out;
            fxmenet::chain::wantedBuses (slot.box, routing, in, out);
            if (shadow->wantsBuses (in, out))
                busChanges.push_back (slot.slotId);
        }

        if (! pollTick)
            continue;
        juce::MemoryBlock block;
        bool changed = false;
        if (shadow->getNet() != nullptr)
        {
            const auto count = shadow->getStateChangeCount();
            if (count != shadow->lastStateChangeCount)
            {
                shadow->lastStateChangeCount = count;
                shadow->getInstance().getStateInformation (block);
                changed = true;
            }
        }
        else
        {
            changed = shadow->takeStateChange (block);
        }
        if (changed)
            engine.setSlotState (slot.slotId, std::vector<std::uint8_t> ((const std::uint8_t*) block.getData(),
                                                                         (const std::uint8_t*) block.getData() + block.getSize()));
    }

    // A copy whose buses change is prepared again: out of the chain first.
    for (auto id : busChanges)
    {
        publishLocal (id);
        for (auto& slot : slots)
            if (slot.slotId == id)
            {
                std::vector<bool> in, out;
                fxmenet::chain::wantedBuses (slot.box, routing, in, out);
                findShadow (id)->setWantedBuses (in, out);
            }
        localSignature = {};
    }

    // Republished when the chain, its copies, their channels or latencies,
    // or the routing changed.
    juce::String signature;
    signature << (complete ? "R" : "S");
    for (auto& slot : slots)
    {
        signature << ";" << (int) slot.slotId;
        if (auto* shadow = findShadow (slot.slotId))
        {
            auto& p = shadow->getInstance();
            signature << ":" << juce::String::toHexString ((juce::pointer_sized_int) &p)
                      << ":" << p.getTotalNumInputChannels() << "/" << p.getTotalNumOutputChannels()
                      << ":" << p.getLatencySamples();
        }
    }
    for (auto& c : routing)
        signature << "|" << (int) c.from.box << "." << c.from.channel << ">" << (int) c.to.box << "." << c.to.channel;

    pumpLocalJsfx();

    if (signature != localSignature || localChain.takeLatencyStale())
    {
        localSignature = signature;
        publishLocal();
    }
    else
    {
        for (auto& ls : localChain.getSlots())   // bypass: no republish
            for (auto& slot : slots)
                if (slot.slotId == ls->slotId)
                    ls->bypassed = slot.bypassed;
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FxmeNetAudioProcessor();
}
