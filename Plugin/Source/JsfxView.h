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

    Interactive (phase 4): the mouse (position, buttons, modifiers, wheel)
    and the keys reach @gfx. What @gfx then changes goes to the server:
      - a slider: as the parameter (PARAM_SET, in a gesture while a button
        is held), so that @slider runs there and the DAW sees it;
      - other variables and memory: JSFX_WRITE, only while the user acts on
        the display and for a moment after (interactionHoldMs), since
        @gfx also changes its own scratch at every frame.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include "PluginProcessor.h"

#include <FxmeNet/jsfx/JsfxBundle.h>
#include <FxmeNet/jsfx/JsfxSync.h>

#include <map>
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
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

    /** A key as gfx_getchar() returns it, 0 for none. */
    static int scriptKeyCode (const juce::KeyPress&);

private:
    void timerCallback() override;

    /** A fresh display instance of the script (the server's reset). */
    bool createInstance();
    void destroyInstance();
    void apply (const jsfx::SyncFrame&);
    void subscribeNewBlocks();
    void drawFrame();

    /** Before @gfx runs: what it may change (while the user acts on it). */
    void snapshot();
    /** After: sliders it moved as parameters, the rest as JSFX_WRITE. */
    void sendChanges();
    void touched();   ///< the user acted on the display
    int mouseCap (const juce::ModifierKeys&) const;

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
    int buttons = 0;              ///< mouse_cap's buttons: 1 left, 2 right, 64 middle
    int modifiers = 0;            ///< mouse_cap's modifiers
    std::map<int, int> heldKeys;  ///< script code -> JUCE key code, to release them

    // Write-back (phase 4).
    static constexpr juce::uint32 interactionHoldMs = 500;
    static constexpr std::size_t maxSnapshotBlocks = 8;
    juce::uint32 lastInteraction = 0;
    std::vector<bool> writable;          ///< by variable index: may go to the server
    std::vector<double> before;          ///< the variables before @gfx ran
    std::map<std::uint32_t, std::vector<double>> beforeBlocks;
    std::vector<double*> sliderVars;     ///< by parameter index
    std::vector<double> sliderBefore;
    std::set<int> gestures;              ///< parameters in a gesture (a button held)
    jsfx::SyncFrame writeFrame;
    std::vector<std::uint8_t> writePayload;
    juce::uint32 lastFrameAt = 0;
    std::uint64_t framesReceived = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JsfxView)
};

} // namespace fxmenet::plugin
