/*
  ------------------------------------------------------------------------------
    Buses.h

    Which buses of a hosted plugin are active, the same everywhere FxmeNet
    loads one (server chain, client shadows, test tools): the plugin's own
    default layout, as a DAW gives it, with the main input and output
    enabled if they were not.

    Not enableAllBuses(): many plugins change what they do when an aux or
    sidechain input is active (a compressor then listens to it, a reverb
    mixes it in), and on the server that input is silent. Seen with Dynette
    (very loud: its detector on a silent sidechain) and TAL Reverb 4 (a
    different sound), 2026-11-03.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <vector>

namespace fxmenet::catalog
{

/** The plugin's default buses, main input and output enabled. Before
    prepareToPlay. */
inline void useDefaultBuses (juce::AudioProcessor& plugin)
{
    for (const bool input : { true, false })
        if (auto* main = plugin.getBus (input, 0))
            if (! main->isEnabled())
                main->enable (true);
}

//==============================================================================
// The main buses' width, chosen per slot (doc/bus-width-plan.md): plugins
// such as the IEM ambisonic ones set their order from it. JUCE's VST3 host
// can only ask a plugin about a layout while it is not prepared, so these
// are called before prepareToPlay (or after releaseResources).

/** The channel sets tried for a width, in order: mono and stereo, the
    named layout of that size, ambisonics for a square count (4, 9, 16...),
    then a discrete set. */
inline std::vector<juce::AudioChannelSet> channelSetsFor (int channels)
{
    std::vector<juce::AudioChannelSet> sets;
    if (channels == 1)
        sets.push_back (juce::AudioChannelSet::mono());
    if (channels == 2)
        sets.push_back (juce::AudioChannelSet::stereo());
    const auto canonical = juce::AudioChannelSet::canonicalChannelSet (channels);
    if (! canonical.isDisabled() && canonical.size() == channels && channels > 2)
        sets.push_back (canonical);
    for (int order = 1; order <= 7; ++order)
        if ((order + 1) * (order + 1) == channels)
            sets.push_back (juce::AudioChannelSet::ambisonic (order));
    if (channels > 2)
        sets.push_back (juce::AudioChannelSet::discreteChannels (channels));
    return sets;
}

/** The widths (1 to maxChannels) the plugin accepts on its main input (or
    output), the other buses as they are. Not prepared. */
inline std::vector<int> acceptedMainWidths (juce::AudioProcessor& plugin, bool input, int maxChannels = 64)
{
    std::vector<int> widths;
    if (plugin.getBus (input, 0) == nullptr)
        return widths;
    const auto current = plugin.getBusesLayout();
    for (int n = 1; n <= maxChannels; ++n)
        for (auto& set : channelSetsFor (n))
        {
            auto layout = current;
            (input ? layout.inputBuses : layout.outputBuses).getReference (0) = set;
            if (plugin.checkBusesLayoutSupported (layout))
            {
                widths.push_back (n);
                break;
            }
        }
    return widths;
}

/** Sets the main input and output to `inputs` and `outputs` channels (0:
    as they are), the other buses kept. False when the plugin refuses every
    way of having that pair: it keeps its layout. Not prepared. */
inline bool setMainWidths (juce::AudioProcessor& plugin, int inputs, int outputs)
{
    const auto current = plugin.getBusesLayout();
    const auto setsOf = [&] (bool input, int n)
    {
        if (n <= 0 || plugin.getBus (input, 0) == nullptr)
            return std::vector<juce::AudioChannelSet> { current.getChannelSet (input, 0) };
        return channelSetsFor (n);
    };
    if ((inputs <= 0 || current.getMainInputChannels() == inputs)
        && (outputs <= 0 || current.getMainOutputChannels() == outputs))
        return true;   // nothing to change

    for (auto& in : setsOf (true, inputs))
        for (auto& out : setsOf (false, outputs))
        {
            auto layout = current;
            if (! layout.inputBuses.isEmpty())
                layout.inputBuses.getReference (0) = in;
            if (! layout.outputBuses.isEmpty())
                layout.outputBuses.getReference (0) = out;
            if (plugin.checkBusesLayoutSupported (layout) && plugin.setBusesLayout (layout))
                return true;
        }
    return false;
}

} // namespace fxmenet::catalog
