/*
  ------------------------------------------------------------------------------
    Layouts.h

    The main bus channel counts offered for a slot (doc/bus-width-plan.md),
    in the Parameters tab and on the Routing tab's boxes. Generic and plain
    (decided with the user): every channel count the plugin accepts, in
    any layout (the server, or the local copy, tested JUCE's layouts on
    it), shown as a number; the user knows what the plugin expects. A count
    is applied as the plugin's current layout when it has that count
    already, else as the first layout of that count it accepts.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeNet/client/ClientEngine.h>

#include <algorithm>
#include <vector>

namespace fxmenet::plugin
{

/** The key of a slot's main input (or output) layout now. */
inline juce::String currentLayoutKey (const client::SlotModel& s, bool input)
{
    return input ? s.info.inputLayout : s.info.outputLayout;
}

/** One layout per channel count the plugin accepts on its main input (or
    output), ascending: the current one for its count, else the first
    accepted of that count. */
inline std::vector<control::LayoutInfo> offeredLayouts (const client::SlotModel& s, bool input)
{
    const auto& accepted = input ? s.info.inputLayouts : s.info.outputLayouts;
    const auto now = currentLayoutKey (s, input);
    const auto current = std::find_if (accepted.begin(), accepted.end(), [&now] (const control::LayoutInfo& l) { return l.key == now; });

    std::vector<control::LayoutInfo> out;
    for (auto& l : accepted)
    {
        if (l.channels <= 0)
            continue;
        auto same = std::find_if (out.begin(), out.end(), [&l] (const control::LayoutInfo& o) { return o.channels == l.channels; });
        if (same == out.end())
            out.push_back (l);
        else if (l.key == now)
            *same = l;   // its current layout stands for its count
    }
    if (current == accepted.end() && now.isNotEmpty())   // not among those tested (should not happen): kept as it is
    {
        const int channels = input ? s.info.numInputs : s.info.numOutputs;
        if (std::none_of (out.begin(), out.end(), [channels] (const control::LayoutInfo& o) { return o.channels == channels; }))
            out.push_back ({ now, now, channels, false });
    }
    std::stable_sort (out.begin(), out.end(), [] (const control::LayoutInfo& a, const control::LayoutInfo& b)
    {
        return a.channels < b.channels;
    });
    return out;
}

/** A count as listed: the number of channels. */
inline juce::String layoutText (const control::LayoutInfo& l)
{
    return juce::String (l.channels);
}

} // namespace fxmenet::plugin
