/*
  ------------------------------------------------------------------------------
    PluginEditor.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "PluginEditor.h"
#include "Tooltips.h"

namespace theme = gainpan::theme;
namespace tips  = gainpan::tips;

GainPanAudioProcessorEditor::GainPanAudioProcessorEditor (GainPanAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    laf.setAccentColour (theme::accent);
    setLookAndFeel (&laf);

    // Top bar: presets with their browser, the "?" switch, info.
    topBar.setAccentColour (theme::accent);
    topBar.setBackgroundColour (theme::topBarBg);
    topBar.setDecoration (assetImage ("icon_png"));
    topBar.onLogoClicked = [this] { splash.show (-1); };
    addAndMakeVisible (topBar);

    presetBar.setAccentColour (theme::accent);
    presetBar.setBrowserButtonVisible (true);
    presetBar.setBrowserSize (300, 300);

    tooltipsButton.setButtonText ("?");
    tooltipsButton.setAccent (theme::accent, theme::dimText);
    tooltipsButton.setTooltip (tips::tooltips);
    tooltipsButton.setToggleState (gainpan::getUiTooltips(), juce::dontSendNotification);
    tooltipsButton.onClick = [this] { gainpan::setUiTooltips (tooltipsButton.getToggleState()); };

    infoButton.setColours ({ theme::accent, theme::text, theme::panel, theme::panelLine });
    infoButton.setInfo (theme::displayName,
                        "Gain and left/right balance, with input and output meters.\n\n"
                        "Drag a knob vertically or horizontally to turn it; "
                        "right-click it to type a value.\n\n"
                        "GainPan is the FxmeNet example: the same plugin runs its audio "
                        "on an FxmeNet server while this window stays in the DAW.");

    topBar.setRightControls ({ { &presetBar, 210 }, { &tooltipsButton, 24 }, { &infoButton, 22 } });

    // Knobs.
    theme::accentKnob (gainKnob, theme::accent);
    theme::accentKnob (panKnob, theme::panAccent);
    panKnob.setCentralValue (0.0);
    gainKnob.setTooltip (tips::gain);
    panKnob.setTooltip (tips::pan);
    addAndMakeVisible (gainKnob);
    addAndMakeVisible (panKnob);

    // Meters.
    for (auto* m : { &inputMeter, &outputMeter })
    {
        fxme::StereoLevelMeter::Colours c;
        c.bar = theme::accent;
        m->setColours (c);
        addAndMakeVisible (*m);
    }
    inputMeter.setTooltip (tips::inputMeter);
    outputMeter.setTooltip (tips::outputMeter);

    setSize (theme::width, theme::height);

    // Splash, over everything, once per instance.
    splash.setImage (assetImage ("splash_png"));
    splash.setLink ("fx-mechanics.com", juce::URL ("https://fx-mechanics.com"), assetImage ("logo686_png"));
    splash.setLinkColour (theme::accent);
    addChildComponent (splash);
    splash.setBounds (getLocalBounds());
    if (processor.takeFirstSplash())
        splash.show (3000);

    startTimerHz (30);
}

GainPanAudioProcessorEditor::~GainPanAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
void GainPanAudioProcessorEditor::timerCallback()
{
    updateMeter (inputMeter,  processor.getInputMeter (0),  processor.getInputMeter (1));
    updateMeter (outputMeter, processor.getOutputMeter (0), processor.getOutputMeter (1));
}

void GainPanAudioProcessorEditor::updateMeter (fxme::StereoLevelMeter& display,
                                               fxme::VuMeter& left, fxme::VuMeter& right)
{
    int ch = 0;
    for (auto* m : { &left, &right })
    {
        display.update (ch++, m->getRMS(), m->getMax());
        m->resetMax();
    }
}

//==============================================================================
void GainPanAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);

    g.setColour (theme::dimText);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText ("IN",  inputLabelArea,  juce::Justification::centred);
    g.drawText ("OUT", outputLabelArea, juce::Justification::centred);
}

void GainPanAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    topBar.setBounds (area.removeFromTop (theme::topBarHeight));
    splash.setBounds (getLocalBounds());

    area.reduce (16, 14);
    constexpr int meterWidth = 26, labelHeight = 16;

    auto inColumn  = area.removeFromLeft (meterWidth);
    auto outColumn = area.removeFromRight (meterWidth);
    inputLabelArea  = inColumn.removeFromBottom (labelHeight);
    outputLabelArea = outColumn.removeFromBottom (labelHeight);
    inputMeter.setBounds (inColumn);
    outputMeter.setBounds (outColumn);

    area.reduce (16, 0);
    const int knobSize = juce::jmin (area.getHeight(), area.getWidth() / 2 - 8);
    auto left  = area.removeFromLeft (area.getWidth() / 2);
    gainKnob.setBounds (left.withSizeKeepingCentre (knobSize, knobSize));
    panKnob.setBounds (area.withSizeKeepingCentre (knobSize, knobSize));
}

//==============================================================================
juce::Image GainPanAudioProcessorEditor::assetImage (const char* resourceName)
{
    int size = 0;
    if (const char* data = BinaryData::getNamedResource (resourceName, size))
        return juce::ImageCache::getFromMemory (data, size);
    return {};
}
