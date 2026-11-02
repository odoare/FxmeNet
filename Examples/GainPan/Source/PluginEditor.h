/*
  ------------------------------------------------------------------------------
    PluginEditor.h

    GainPan's window: the FX-Mechanics top bar (preset bar, "?" for the hover
    help, info), the input meter, the gain and pan knobs, the output meter,
    and the splash. Nothing here knows about FxmeNet: the meters it reads are
    filled by processBlock locally, or from the server under FxmeNet.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "AppSettings.h"
#include "Theme.h"

class GainPanAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    explicit GainPanAudioProcessorEditor (GainPanAudioProcessor&);
    ~GainPanAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    /** Pushes one meter pair's levels into its display; the held max is read
        and reset, so each frame shows the peak since the previous one. */
    static void updateMeter (fxme::StereoLevelMeter& display,
                             fxme::VuMeter& left, fxme::VuMeter& right);

    /** An embedded image by its binary-data name ("splash_png"). */
    static juce::Image assetImage (const char* resourceName);

    GainPanAudioProcessor& processor;
    fxme::FxmeLookAndFeel laf;

    fxme::TopBar topBar { gainpan::theme::displayName, "Gain, balance, meters: the FxmeNet example",
                          JucePlugin_VersionString, assetImage ("logo686_png") };
    fxme::PresetBarComponent presetBar { processor.getPresetManager() };
    fxme::AccentToggle tooltipsButton;
    fxme::InfoButton infoButton;

    fxme::FxmeSlider gainKnob { processor.getAPVTS(), "gain", "Gain", gainpan::theme::accent };
    fxme::FxmeSlider panKnob  { processor.getAPVTS(), "pan",  "Pan",  gainpan::theme::panAccent };
    fxme::StereoLevelMeter inputMeter, outputMeter;
    juce::Rectangle<int> inputLabelArea, outputLabelArea;

    /** Last among the children, so it covers everything. */
    fxme::SplashOverlay splash;

    struct ToggleableTooltipWindow : public juce::TooltipWindow
    {
        using juce::TooltipWindow::TooltipWindow;
        juce::String getTipFor (juce::Component& c) override
        {
            return gainpan::getUiTooltips() ? juce::TooltipWindow::getTipFor (c) : juce::String();
        }
    };
    ToggleableTooltipWindow tooltipWindow { this, 700 };

    // Right-click value entry puts a TextEditor on screen: one fixer, after
    // the children.
    fxme::TextEntryFocusFixer textEntryFixer { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainPanAudioProcessorEditor)
};
