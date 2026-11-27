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

#include <algorithm>
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
// The main buses' layout, chosen per slot (doc/bus-width-plan.md, revised:
// layouts, not widths). What the plugin accepts decides what is offered.
// JUCE's VST3 host can only ask a plugin about a layout while it is not
// prepared, so these are called before prepareToPlay (or after
// releaseResources).

/** A layout's key, exact and stable: "amb:<order>", "dis:<channels>", or a
    named layout's speaker string ("L R C Lfe Ls Rs"). */
inline juce::String layoutKey (const juce::AudioChannelSet& set)
{
    if (set.isDisabled())
        return {};
    if (const int order = set.getAmbisonicOrder(); order >= 0)
        return "amb:" + juce::String (order);
    if (set.isDiscreteLayout())
        return "dis:" + juce::String (set.size());
    return set.getSpeakerArrangementAsString();
}

/** The layout of a key (disabled when the key means nothing). */
inline juce::AudioChannelSet layoutFromKey (const juce::String& key)
{
    if (key.startsWith ("amb:"))
    {
        const int order = key.fromFirstOccurrenceOf (":", false, false).getIntValue();
        return order >= 0 && order <= 7 ? juce::AudioChannelSet::ambisonic (order) : juce::AudioChannelSet::disabled();
    }
    if (key.startsWith ("dis:"))
    {
        const int n = key.fromFirstOccurrenceOf (":", false, false).getIntValue();
        return n > 0 ? juce::AudioChannelSet::discreteChannels (n) : juce::AudioChannelSet::disabled();
    }
    return key.isEmpty() ? juce::AudioChannelSet::disabled() : juce::AudioChannelSet::fromAbbreviatedString (key);
}

/** A layout as shown: JUCE's name for named layouts ("5.1 Surround"),
    "Ambisonics, order 3 (16 ch)", "16 channels". */
inline juce::String layoutName (const juce::AudioChannelSet& set)
{
    if (const int order = set.getAmbisonicOrder(); order >= 0)
        return "Ambisonics, order " + juce::String (order) + " (" + juce::String (set.size()) + " ch)";
    if (set.isDiscreteLayout())
        return juce::String (set.size()) + (set.size() == 1 ? " channel" : " channels");
    return set.getDescription();
}

/** Every layout tried, 1 to maxChannels channels: JUCE's list for each
    count (discrete, named, ambisonics) and the named layouts it leaves out. */
inline std::vector<juce::AudioChannelSet> candidateLayouts (int maxChannels)
{
    std::vector<juce::AudioChannelSet> sets;
    const auto add = [&sets] (const juce::AudioChannelSet& set)
    {
        if (! set.isDisabled() && std::find (sets.begin(), sets.end(), set) == sets.end())
            sets.push_back (set);
    };
    for (int n = 1; n <= maxChannels; ++n)
        for (auto& set : juce::AudioChannelSet::channelSetsWithNumberOfChannels (n))
            add (set);
    add (juce::AudioChannelSet::create5point0point2());
    add (juce::AudioChannelSet::create5point0point4());
    std::stable_sort (sets.begin(), sets.end(), [] (const juce::AudioChannelSet& a, const juce::AudioChannelSet& b)
    {
        return a.size() < b.size();
    });
    return sets;
}

/** The layouts the plugin accepts on its main input (or output), the other
    buses as they are. Not prepared. */
inline std::vector<juce::AudioChannelSet> acceptedMainLayouts (juce::AudioProcessor& plugin, bool input,
                                                               int maxChannels = 64)
{
    std::vector<juce::AudioChannelSet> accepted;
    if (plugin.getBus (input, 0) == nullptr)
        return accepted;
    const auto current = plugin.getBusesLayout();
    for (auto& set : candidateLayouts (maxChannels))
    {
        auto layout = current;
        (input ? layout.inputBuses : layout.outputBuses).getReference (0) = set;
        if (plugin.checkBusesLayoutSupported (layout))
            accepted.push_back (set);
    }
    return accepted;
}

/** The main input's (or output's) layout now, as a key. */
inline juce::String mainLayoutKey (const juce::AudioProcessor& plugin, bool input)
{
    return plugin.getBus (input, 0) != nullptr ? layoutKey (plugin.getBusesLayout().getChannelSet (input, 0)) : juce::String();
}

/** Sets the main input and output layouts by key (empty: as they are), the
    other buses kept. False when the plugin refuses the pair (or a key means
    nothing): it keeps its layout. Not prepared. */
inline bool setMainLayouts (juce::AudioProcessor& plugin, const juce::String& inputKey, const juce::String& outputKey)
{
    auto layout = plugin.getBusesLayout();
    bool change = false;
    for (const bool input : { true, false })
    {
        const auto& key = input ? inputKey : outputKey;
        if (key.isEmpty() || plugin.getBus (input, 0) == nullptr || key == mainLayoutKey (plugin, input))
            continue;
        const auto set = layoutFromKey (key);
        if (set.isDisabled())
            return false;
        (input ? layout.inputBuses : layout.outputBuses).getReference (0) = set;
        change = true;
    }
    if (! change)
        return true;
    return plugin.checkBusesLayoutSupported (layout) && plugin.setBusesLayout (layout);
}

} // namespace fxmenet::catalog
