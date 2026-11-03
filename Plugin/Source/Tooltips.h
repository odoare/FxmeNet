/*
  ------------------------------------------------------------------------------
    Tooltips.h

    Every hover-help string of FxmeNet. No tooltip literal anywhere else.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

namespace fxmenet::plugin::tips
{
    inline constexpr auto server     = "The server: pick one found on the network, or type host or host:port.";
    inline constexpr auto find       = "Look for FxmeNet servers on the local network (broadcast).";
    inline constexpr auto connect    = "Connect to the server, or disconnect (the audio then passes dry).";
    inline constexpr auto catalog    = "The plugins installed on the server. Double-click one, or select it and press Add.";
    inline constexpr auto add        = "Add the selected server plugin at the end of the chain.";
    inline constexpr auto chain      = "The chain, processed on the server from top to bottom.";
    inline constexpr auto moveUp     = "Move the selected plugin up the chain.";
    inline constexpr auto moveDown   = "Move the selected plugin down the chain.";
    inline constexpr auto bypass     = "Bypass the selected plugin (on the server).";
    inline constexpr auto remove     = "Remove the selected plugin from the chain.";
    inline constexpr auto statistics = "Latency reported to the host, network round trip, blocks played as silence because their reply came too late, and blocks saved by waiting a little for them.";
    inline constexpr auto tooltips   = "Hover help on the controls: on or off (all instances).";
}
