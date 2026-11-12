/*
  ------------------------------------------------------------------------------
    Chain.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Chain.h"
#if FXMENET_JSFX
 #include "JsfxInstance.h"
#endif
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
void Chain::prepare (double newSampleRate, int newBlockSize, int newInputs, int newOutputs)
{
    // Called with the session's audio thread stopped.
    sampleRate = newSampleRate;
    blockSize = newBlockSize;
    numInputs = newInputs;
    numOutputs = newOutputs;
    for (auto& s : slots)
    {
        s->instance->releaseResources();
        s->instance->prepareToPlay (sampleRate, blockSize);
    }
    update();   // a plan for the new format; buses again (the channel counts may let more connections fit)
}

void Chain::setRouting (std::vector<chain::Connection> connections)
{
    routing = std::move (connections);
    routed = true;
    update();
}

void Chain::refreshLatencies()
{
    if (routed && latencyStale.exchange (false))
        publish();
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
    ready.buses = describeBuses (*instance);
    ready.params = describeParameters (*instance);
    ready.fxmenetAbi = 0;
   #if FXMENET_JSFX
    if (auto* script = dynamic_cast<JsfxInstance*> (instance.get()))
    {
        ready.jsfx = true;
        ready.jsfxGfx = script->hasGfx();
    }
   #endif
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

    // For the routing: its buses at full width, and the layout it has.
    slot->box = control::toBox (ready);
    for (const bool input : { true, false })
        for (int i = 0; i < instance->getBusCount (input); ++i)
            (input ? slot->wantedIn : slot->wantedOut).push_back (instance->getBus (input, i)->isEnabled());

    slot->instance = std::move (instance);
    slot->watcher = std::make_unique<ParamWatcher> (*slot->instance);   // after the state is loaded: no echo of it
    position = juce::jlimit (0, (int) slots.size(), position);
    slots.insert (slots.begin() + position, std::move (slot));
    update();
    return {};
}

bool Chain::removeSlot (std::uint32_t slotId)
{
    const int i = indexOf (slotId);
    if (i < 0)
        return false;
    slots.erase (slots.begin() + i);
    update();
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
    update();
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

//==============================================================================
std::vector<chain::Box> Chain::boxes() const
{
    std::vector<chain::Box> out { chain::Box::makeInputs (numInputs) };
    for (auto& s : slots)
        out.push_back (s->box);
    out.push_back (chain::Box::makeOutputs (numOutputs));
    return out;
}

void Chain::update()
{
    if (! routed)
    {
        publish();
        return;
    }

    // The buses each slot needs with the connections that fit now.
    const auto fit = chain::fittingConnections (boxes(), routing);
    struct Change { Slot* slot; std::vector<bool> in, out; };
    std::vector<Change> changes;
    for (auto& s : slots)
    {
        Change c { s.get(), {}, {} };
        chain::wantedBuses (s->box, fit, c.in, c.out);
        if (c.in != s->wantedIn || c.out != s->wantedOut)
            changes.push_back (std::move (c));
    }

    if (! changes.empty())
    {
        // Out of the audio thread's way first: a plan without them, which
        // publish() only returns from once the old one is let go.
        for (auto& c : changes)
            c.slot->muted = true;
        const bool released = publish();
        for (auto& c : changes)
        {
            if (released)
                applyBuses (*c.slot, c.in, c.out);
            c.slot->muted = false;
        }
    }
    publish();
}

void Chain::applyBuses (Slot& s, const std::vector<bool>& in, const std::vector<bool>& out)
{
    auto& p = *s.instance;
    auto layout = p.getBusesLayout();
    for (const bool input : { true, false })
    {
        const auto& wanted = input ? in : out;
        auto& sets = input ? layout.inputBuses : layout.outputBuses;
        for (int i = 0; i < sets.size() && i < (int) wanted.size(); ++i)
        {
            auto* bus = p.getBus (input, i);
            if (bus == nullptr)
                continue;
            if (! wanted[(std::size_t) i])
                sets.getReference (i) = juce::AudioChannelSet::disabled();
            else if (sets[i].isDisabled())
                sets.getReference (i) = ! bus->getLastEnabledLayout().isDisabled() ? bus->getLastEnabledLayout()
                                                                                   : bus->getDefaultLayout();
        }
    }

    p.releaseResources();
    if (! p.setBusesLayout (layout))
        log ("slot " + juce::String ((int) s.slotId) + ": " + p.getName() + " refused a bus layout; it keeps its own");
    p.prepareToPlay (sampleRate, blockSize);

    // What was asked, refused or not: asked again only when it changes.
    s.wantedIn = in;
    s.wantedOut = out;
    s.numChannels = juce::jmax (1, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
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

bool Chain::publish()
{
    int channels = 2;
    for (auto& s : slots)
        channels = juce::jmax (channels, s->numChannels);
    requiredChannels = channels;

    auto* next = new Snapshot { slots, nullptr };
    if (routed)
    {
        std::vector<chain::SlotLayout> layouts;
        for (auto& s : slots)
        {
            chain::SlotLayout l;
            l.box = s->box;
            for (const bool input : { true, false })
                for (int i = 0; i < (int) (input ? s->box.inputs : s->box.outputs).size(); ++i)
                {
                    auto* bus = s->instance->getBus (input, i);
                    (input ? l.inputWidths : l.outputWidths).push_back (bus != nullptr && bus->isEnabled()
                                                                        ? bus->getNumberOfChannels() : 0);
                }
            l.latency = s->instance->getLatencySamples();
            l.muted = s->muted;
            layouts.push_back (std::move (l));
        }
        // Only this thread replaces or frees the current snapshot: reading
        // its plan here is safe. Its delay lines go on in the new one.
        const auto* previous = current.load();
        next->plan = std::make_unique<chain::RoutingPlan> (numInputs, numOutputs, layouts, routing, blockSize,
                                                           previous != nullptr ? previous->plan.get() : nullptr);
        latency.store (next->plan->getLatency());
        outputsFed.store (next->plan->getOutputsFed());
    }

    auto* old = current.exchange (next);
    if (old == nullptr)
        return true;

    // Wait until the audio thread has let go of the old snapshot: at most
    // one block. If it never does (a plugin stuck in processBlock), leak it
    // rather than free memory in use.
    const auto start = juce::Time::getMillisecondCounter();
    while (inUse.load() == old)
    {
        if (juce::Time::getMillisecondCounter() - start > 2000)
        {
            log ("warning: the audio thread holds an old chain for over 2 s; leaking it");
            return false;
        }
        juce::Thread::sleep (1);
    }
    delete old;   // the last owner of a removed slot destroys its instance here
    return true;
}

//==============================================================================
void Chain::process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool offline, bool reset)
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

    if (s != nullptr && s->plan != nullptr)
        processRouted (*s, buffer, midi, offline, reset);
    else if (s != nullptr)
    {
        int total = 0;
        for (auto& slot : s->slots)
        {
            auto& instance = *slot->instance;
            if (instance.isNonRealtime() != offline)
                instance.setNonRealtime (offline);
            if (reset)
                instance.reset();   // a VST3: deactivated and activated again, so not free, but rare
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

void Chain::processRouted (Snapshot& s, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi,
                           bool offline, bool reset)
{
    auto& plan = *s.plan;
    const int n = buffer.getNumSamples();
    if (reset)
        plan.clearDelays();

    // The plan copies the inputs first, then runs the slots on its own
    // buffers; the outputs are copied back into `buffer` at the end.
    plan.process (buffer.getArrayOfReadPointers(), buffer.getNumChannels(), n,
                  [&] (int index, float* const* channels, int width, int count)
    {
        auto& slot = *s.slots[(std::size_t) index];
        auto& instance = *slot.instance;
        if (instance.isNonRealtime() != offline)
            instance.setNonRealtime (offline);
        if (reset)
            instance.reset();

        juce::AudioBuffer<float> view (channels, width, count);
        if (slot.bypassed.load())
            instance.processBlockBypassed (view, midi);
        else
            instance.processBlock (view, midi);

        if (instance.getLatencySamples() != plan.getSlotLatency (index))
            latencyStale.store (true);   // the session's timer rebuilds the plan
    });

    for (int c = 0; c < buffer.getNumChannels(); ++c)
    {
        if (const auto* out = plan.getOutput (c))
            buffer.copyFrom (c, 0, out, n);
        else
            buffer.clear (c, 0, n);
    }
}

//==============================================================================
std::vector<control::BusInfo> describeBuses (juce::AudioProcessor& instance)
{
    std::vector<control::BusInfo> out;
    for (const bool input : { true, false })
        for (int i = 0; i < instance.getBusCount (input); ++i)
            if (auto* bus = instance.getBus (input, i))
            {
                // Its full width even when disabled, so that the routing's
                // channel numbers do not move when it is enabled.
                int channels = bus->getNumberOfChannels();
                if (channels == 0)
                    channels = bus->getDefaultLayout().size();
                if (channels == 0)
                    channels = bus->getLastEnabledLayout().size();

                control::BusInfo b;
                b.name = bus->getName();
                b.input = input;
                b.channels = channels;
                b.main = i == 0;
                out.push_back (b);
            }
    return out;
}

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
       #if FXMENET_JSFX
        if (auto* ranged = dynamic_cast<RangedParameter*> (p))
            i.hasRange = ranged->getRange (i.rangeMin, i.rangeMax, i.rangeStep);
       #endif

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
