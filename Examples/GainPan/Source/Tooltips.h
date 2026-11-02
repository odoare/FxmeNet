/*
  ------------------------------------------------------------------------------
    Tooltips.h

    Every hover-help string of GainPan. No tooltip literal anywhere else.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

namespace gainpan::tips
{
    inline constexpr auto gain        = "Output gain, -60 to +12 dB. Right-click to type a value.";
    inline constexpr auto pan         = "Balance between left and right: the far side is lowered, never boosted.";
    inline constexpr auto inputMeter  = "Input level (RMS bars, held peak marks), dBFS.";
    inline constexpr auto outputMeter = "Output level (RMS bars, held peak marks), dBFS.";
    inline constexpr auto tooltips    = "Hover help on the controls: on or off (all instances).";
}
