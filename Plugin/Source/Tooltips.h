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

    inline constexpr auto paramSlider   = "The parameter's value on the server (normalised). Double-click for the default.";
    inline constexpr auto paramPool     = "The automation parameter of this plugin it is mapped to, as the DAW lists it.";
    inline constexpr auto learn         = "Learn: then move any of FxmeNet's automation parameters in the DAW, and it controls this parameter.";
    inline constexpr auto map           = "Map to the first free automation parameter, or unmap.";
    inline constexpr auto mappingList   = "Every automation parameter in use and what it controls. Select some, then Unmap.";
    inline constexpr auto unmap         = "Free the selected automation parameters.";
    inline constexpr auto unmapAll      = "Free every automation parameter.";
    inline constexpr auto blockSize     = "Samples per block sent to the server: smaller gives less latency, more packets.";
    inline constexpr auto budget        = "Blocks of time a reply has to come back: more survives a busier network, at more latency.";
    inline constexpr auto allChannels   = "Enable every channel.";
    inline constexpr auto inputChannel  = "Send this input to the server. Off: the chain gets silence on it.";
    inline constexpr auto outputChannel = "Receive this output from the server. Off: silent.";
}
