/*
  ------------------------------------------------------------------------------
    Theme.h

    GainPan's colours, names and geometry, in one place.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

namespace gainpan::theme
{
    inline const juce::String displayName = "GainPan";

    inline const juce::Colour background { 0xff17161f };
    inline const juce::Colour topBarBg   { 0xff1f1e29 };
    inline const juce::Colour accent     { 0xff4cc9f0 };   // gain, the shell
    inline const juce::Colour panAccent  { 0xffd9b13a };
    inline const juce::Colour text       { 0xffd8d8e0 };
    inline const juce::Colour dimText    { 0xff8a8a9a };
    inline const juce::Colour panel      { 0xff20202c };
    inline const juce::Colour panelLine  { 0xff3a3a4c };

    constexpr int topBarHeight = 34;
    constexpr int width        = 440;
    constexpr int height       = 230 + topBarHeight;

    /** Dark disc, accent on the arc, outline and pointer; value and label drawn
        by the look-and-feel. */
    inline void accentKnob (juce::Slider& s, juce::Colour a)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour (0xff2b2b2b));
        s.setColour (juce::Slider::rotarySliderOutlineColourId, a.darker (1.6f));
        s.setColour (juce::Slider::trackColourId,               a);
        s.setColour (juce::Slider::thumbColourId,               a.brighter (0.4f));
        s.getProperties().set ("showLabel", true);
    }
}
