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

} // namespace fxmenet::catalog
