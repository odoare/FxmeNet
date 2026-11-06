/*
  ------------------------------------------------------------------------------
    RoutingView.h

    The routing between the chain's plugins (doc/routing.md, step E), in the
    stream and channels tab: stacked top to bottom, FxmeNet's INPUTS (the
    channels sent to the server), one box per plugin in chain order with its
    input pins on top and its output pins below, grouped by bus, and the
    OUTPUTS (received from the server). Connections are curves from an
    output pin to an input pin below: bright when the user set the input
    (manual), dim when it follows the default rule. An aux bus nothing is
    connected to is drawn hollow: it is disabled on the server.

    Mouse:
      - drag from a pin to a pin: one connection (either direction, the
        output end must be above);
      - drag from a bus name: its channels in order, onto a bus name (from
        its first channel) or a pin (from that channel);
      - shift-click a connection: removed (as in Reaper);
      - right-click an input pin: back to the default rule, or disconnect.

    Everything goes through the engine's routing calls; the view redraws
    from the engine's model (refresh(), from the panel's 10 Hz timer) when
    it changed.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"

namespace fxmenet::plugin
{

class RoutingView : public juce::Component
{
public:
    explicit RoutingView (FxmeNetAudioProcessor&);
    ~RoutingView() override;

    /** Re-reads the engine's routing; redraws when something changed. */
    void refresh();

    void resized() override;

private:
    class Canvas;

    juce::Viewport viewport;
    std::unique_ptr<Canvas> canvas;
};

} // namespace fxmenet::plugin
