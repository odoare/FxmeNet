/*
  ------------------------------------------------------------------------------
    Widths.h

    The main bus widths offered for a slot (doc/bus-width-plan.md), in the
    Parameters tab and on the Routing tab's boxes: of those the plugin
    accepts, only the useful ones (decided with the user): stereo and the
    ambisonic sizes (1, 4, 9, 16, 25, 36, 49, 64), plus the width it has
    now, so that a plugin whose default is another width keeps it in the
    list.

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

/** The width of a slot's main input (or output) now: its first bus of that
    side, as its description says. */
inline int currentMainWidth (const client::SlotModel& s, bool input)
{
    for (auto& b : s.info.buses)
        if (b.input == input && b.main)
            return b.channels;
    return input ? s.info.numInputs : s.info.numOutputs;
}

/** The widths offered for a slot's main input (or output), ascending. */
inline std::vector<int> usefulWidths (const client::SlotModel& s, bool input)
{
    static constexpr int useful[] = { 1, 2, 4, 9, 16, 25, 36, 49, 64 };
    const auto& accepted = input ? s.info.inputWidths : s.info.outputWidths;
    std::vector<int> out;
    for (int w : useful)
        if (std::find (accepted.begin(), accepted.end(), w) != accepted.end())
            out.push_back (w);
    const int now = currentMainWidth (s, input);
    if (now > 0 && std::find (out.begin(), out.end(), now) == out.end())
        out.insert (std::upper_bound (out.begin(), out.end(), now), now);
    return out;
}

/** A width as offered in a list: "16 (3rd order)" for ambisonic sizes. */
inline juce::String widthText (int channels)
{
    for (int order = 1; order <= 7; ++order)
        if ((order + 1) * (order + 1) == channels)
            return juce::String (channels) + " (order " + juce::String (order) + ")";
    return channels == 1 ? juce::String ("1 (mono)") : channels == 2 ? juce::String ("2 (stereo)") : juce::String (channels);
}

} // namespace fxmenet::plugin
