/*
  ------------------------------------------------------------------------------
    JsfxView.h

    A JSFX script's @gfx display, drawn here while the script's audio runs on
    the server (doc/jsfx-plan.md, phase 3). The view holds a second instance
    of the same script, a display one (no audio code runs in it), and keeps
    it in step with the server's (Common/FxmeNet/jsfx/JsfxSync.h):

      - it turns the slot's telemetry on (TELEMETRY_ACTIVE) while it exists;
        the server answers with a reset, then a JSFX_SYNC at each telemetry
        tick: changed variables, changed memory;
      - at its frame rate (the script's gfx_hz, else 30 Hz) it applies what
        arrived, runs @gfx, and shows the frame;
      - when @gfx touches a memory block for the first time, it subscribes
        to it (JSFX_SUBSCRIBE): only the memory the display reads travels.

    Read-only for now (phase 3): the mouse position reaches @gfx (hover),
    clicks and keys do not (phase 4).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include "PluginProcessor.h"

#include <FxmeNet/jsfx/JsfxBundle.h>
#include <FxmeNet/jsfx/JsfxSync.h>

#include <set>

class SX_Instance;

namespace fxmenet::plugin
{

class JsfxView : public juce::Component,
                 private juce::Timer
{
public:
    /** The view of a loaded JSFX slot. Check getError() after. */
    JsfxView (FxmeNetAudioProcessor&, std::uint32_t slotId);
    ~JsfxView() override;

    /** Empty when the script runs here; else why not. */
    const juce::String& getError() const noexcept   { return error; }

    std::uint32_t getSlotId() const noexcept   { return slotId; }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override   {}
    void mouseExit (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    /** A fresh display instance of the script (the server's reset). */
    bool createInstance();
    void destroyInstance();
    void apply (const jsfx::SyncFrame&);
    void subscribeNewBlocks();
    void drawFrame();

    FxmeNetAudioProcessor& processor;
    const std::uint32_t slotId;
    juce::String error;

    jsfx::Bundle bundle;
    double sampleRate = 48000.0;
    juce::File folder;            ///< the script's files, for the engine
    SX_Instance* sx = nullptr;
    std::vector<double*> vars;    ///< the server's variables, by index, here
    std::set<std::uint32_t> subscribed;
    bool haveReset = false;       ///< the server's reset arrived: its frames apply
    bool shown = false;           ///< the slot's telemetry was turned on here

    juce::Image frame;
    bool retina = false;          ///< the script draws at the screen's pixel density
    juce::Point<int> mouse { -1, -1 };
    juce::uint32 lastFrameAt = 0;
    std::uint64_t framesReceived = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JsfxView)
};

} // namespace fxmenet::plugin
