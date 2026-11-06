/*
  ------------------------------------------------------------------------------
    Theme.h

    FxmeNet's colours, names and geometry, in one place.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

namespace fxmenet::plugin::theme
{
    inline const juce::String displayName = "FxmeNet";

    inline const juce::Colour background { 0xff17161f };
    inline const juce::Colour topBarBg   { 0xff1f1e29 };
    inline const juce::Colour accent     { 0xff7bd88f };
    inline const juce::Colour warning    { 0xffe0a03a };
    inline const juce::Colour error      { 0xffe0586f };
    inline const juce::Colour text       { 0xffd8d8e0 };
    inline const juce::Colour dimText    { 0xff8a8a9a };
    inline const juce::Colour panel      { 0xff20202c };
    inline const juce::Colour panelLine  { 0xff3a3a4c };
    inline const juce::Colour selection  { 0xff2e3a33 };

    constexpr int topBarHeight = 34;
    constexpr int width        = 960;
    constexpr int height       = 612 + topBarHeight;   // 580, + 32 for the routing defaults row
    constexpr int rowHeight    = 22;
}
