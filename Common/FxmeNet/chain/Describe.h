/*
  ------------------------------------------------------------------------------
    Describe.h

    A hosted plugin's buses and parameters as SLOT_READY carries them
    (protocol.md 4.4 and 4.5): from the server's instance, and, in local
    play (doc/fallback-plan.md), from the plugin's local copy when no
    server has described the slot. Header-only (Common is linked into
    several JUCE targets).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <FxmeNet/control/ControlMessages.h>

#include <functional>
#include <vector>

namespace fxmenet::chain
{

/** The buses of an instance, each at its full width. */
inline std::vector<control::BusInfo> describeBuses (juce::AudioProcessor& instance)
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

/** The parameter list of an instance. `more` may add what only its owner
    knows (the server: a JSFX slider's range). */
inline std::vector<control::ParamInfo> describeParameters (
    juce::AudioPluginInstance& instance,
    const std::function<void (juce::AudioProcessorParameter&, control::ParamInfo&)>& more = {})
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
        if (more)
            more (*p, i);

        const int steps = p->getNumSteps();
        const bool stepped = (p->isDiscrete() || p->isBoolean()) && steps > 1
                             && steps != juce::AudioProcessor::getDefaultNumParameterSteps();
        i.numSteps = stepped ? steps : 0;
        if (stepped && (steps <= 32 || (i.choices && steps <= 1024)))   // a list: every name
            for (int k = 0; k < steps; ++k)
                i.stepTexts.add (p->getText ((float) k / (float) (steps - 1), 64));

        out.push_back (i);
    }
    return out;
}

/** What SLOT_READY says of an instance, apart from the server's own
    fields (FxmeNet-awareness, telemetry, JSFX). */
inline control::SlotReady describeSlot (std::uint32_t slotId, juce::AudioPluginInstance& instance)
{
    control::SlotReady ready;
    ready.slotId = slotId;
    ready.latency = instance.getLatencySamples();
    ready.numInputs = instance.getTotalNumInputChannels();
    ready.numOutputs = instance.getTotalNumOutputChannels();
    ready.buses = describeBuses (instance);
    ready.params = describeParameters (instance);
    return ready;
}

} // namespace fxmenet::chain
