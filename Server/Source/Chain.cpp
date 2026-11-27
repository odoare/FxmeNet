/*
  ------------------------------------------------------------------------------
    Chain.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Chain.h"
#if FXMENET_JSFX
 #include <FxmeNet/jsfx/JsfxInstance.h>
#endif
#include "Log.h"

#include <FxmeNet/catalog/Buses.h>
#include <FxmeNet/chain/Describe.h>

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
    // The audio thread is stopped by now (the session's worker goes first);
    // the core frees its last snapshot.
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
    if (core.takeLatencyStale() && routed)
        publish();
}

juce::String Chain::addSlot (std::uint32_t slotId, int position,
                             std::unique_ptr<juce::AudioPluginInstance> instance,
                             const std::vector<std::uint8_t>& state,
                             control::SlotReady& ready,
                             int mainInputs, int mainOutputs)
{
    if (instance == nullptr)
        return "no instance";
    if (hasSlot (slotId))
        return "slot " + juce::String ((int) slotId) + " already exists";

    auto slot = std::make_shared<Slot>();
    slot->slotId = slotId;

    catalog::useDefaultBuses (*instance);   // not enableAllBuses: see Buses.h

    // The widths asked for, then those it accepts: before preparing (JUCE's
    // VST3 host can only ask an inactive plugin).
    if (! catalog::setMainWidths (*instance, mainInputs, mainOutputs))
        log ("slot " + juce::String ((int) slotId) + ": " + instance->getName() + " refused "
             + juce::String (mainInputs) + " in / " + juce::String (mainOutputs) + " out; it keeps its own");
    slot->inputWidths = catalog::acceptedMainWidths (*instance, true, wire::maxChannels);
    slot->outputWidths = catalog::acceptedMainWidths (*instance, false, wire::maxChannels);

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

    slot->instance = std::move (instance);
    slot->processor = slot->instance.get();
    describe (*slot, ready);

    // For the routing: its buses at full width, and the layout it has.
    slot->box = control::toBox (ready);
    for (const bool input : { true, false })
        for (int i = 0; i < slot->instance->getBusCount (input); ++i)
            (input ? slot->wantedIn : slot->wantedOut).push_back (slot->instance->getBus (input, i)->isEnabled());

    slot->watcher = std::make_unique<ParamWatcher> (*slot->instance);   // after the state is loaded: no echo of it
    position = juce::jlimit (0, (int) slots.size(), position);
    slots.insert (slots.begin() + position, std::move (slot));
    update();
    return {};
}

void Chain::describe (Slot& slot, control::SlotReady& ready) const
{
    auto& instance = *slot.instance;
    ready = {};
    ready.slotId = slot.slotId;
    ready.latency = instance.getLatencySamples();
    ready.numInputs = instance.getTotalNumInputChannels();
    ready.numOutputs = instance.getTotalNumOutputChannels();
    ready.buses = describeBuses (instance);
    ready.params = describeParameters (instance);
    ready.inputWidths = slot.inputWidths;
    ready.outputWidths = slot.outputWidths;
    ready.fxmenetAbi = 0;
   #if FXMENET_JSFX
    if (auto* script = dynamic_cast<jsfx::JsfxInstance*> (&instance))
    {
        ready.jsfx = true;
        ready.jsfxGfx = script->hasGfx();
    }
   #endif
    if (slot.net != nullptr)
    {
        ready.fxmenetAbi = (int) slot.net->getAbiVersion();
        const auto needed = -slot.net->getTelemetrySchema (nullptr, 0);
        if (needed > 0)
        {
            std::vector<std::uint8_t> schema ((std::size_t) needed);
            const auto n = slot.net->getTelemetrySchema (schema.data(), needed);
            if (n > 0)
                ready.telemetrySchema.append (schema.data(), (std::size_t) n);
        }
    }
}

juce::String Chain::setMainWidths (std::uint32_t slotId, int inputs, int outputs, control::SlotReady& ready)
{
    auto* s = findSlot (slotId);
    if (s == nullptr)
        return "no slot " + juce::String ((int) slotId);

    // Out of the audio thread's way first (as update() does for aux buses).
    s->muted = true;
    if (! publish())
    {
        s->muted = false;
        return "the audio thread is busy";
    }

    auto& p = *s->instance;
    p.releaseResources();   // inactive: the plugin can be asked about layouts
    const bool ok = catalog::setMainWidths (p, inputs, outputs);
    s->inputWidths = catalog::acceptedMainWidths (p, true, wire::maxChannels);
    s->outputWidths = catalog::acceptedMainWidths (p, false, wire::maxChannels);
    p.prepareToPlay (sampleRate, blockSize);

    s->numChannels = juce::jmax (1, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    describe (*s, ready);
    s->box = control::toBox (ready);
    s->wantedIn.clear();
    s->wantedOut.clear();
    for (const bool input : { true, false })
        for (int i = 0; i < p.getBusCount (input); ++i)
            (input ? s->wantedIn : s->wantedOut).push_back (p.getBus (input, i)->isEnabled());

    s->muted = false;
    update();   // the routing refits on its new pins
    if (! ok)
        return "refused";
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
    std::unique_ptr<chain::RoutingPlan> plan;
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
        // Its delay lines go on from the current plan's.
        plan = std::make_unique<chain::RoutingPlan> (numInputs, numOutputs, layouts, routing, blockSize, core.getPlan());
    }

    // In series, a muted slot (its layout changing) is left out; routed, the
    // plan keeps it, silent.
    std::vector<chain::ChainProcessor::SlotPtr> list;
    for (auto& s : slots)
        if (routed || ! s->muted)
            list.push_back (s);
    if (core.publish (std::move (list), std::move (plan)))
        return true;
    log ("warning: the audio thread holds an old chain for over 2 s; leaking it");
    return false;
}

//==============================================================================
std::vector<control::BusInfo> describeBuses (juce::AudioProcessor& instance)
{
    return chain::describeBuses (instance);
}

std::vector<control::ParamInfo> describeParameters (juce::AudioPluginInstance& instance)
{
    return chain::describeParameters (instance, [] (juce::AudioProcessorParameter& p, control::ParamInfo& i)
    {
       #if FXMENET_JSFX
        if (auto* ranged = dynamic_cast<jsfx::RangedParameter*> (&p))
        {
            i.hasRange = ranged->getRange (i.rangeMin, i.rangeMax, i.rangeStep);
            i.choices = ranged->isChoice();
            i.hidden = ranged->isHidden();
        }
       #else
        juce::ignoreUnused (p, i);
       #endif
    });
}

} // namespace fxmenet::server
