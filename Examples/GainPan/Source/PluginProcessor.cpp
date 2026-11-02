/*
  ------------------------------------------------------------------------------
    PluginProcessor.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::Identifier stateVersionAttribute { "stateVersion" };

    /** Left and right gains of the balance: the far side is lowered linearly,
        the near one stays at unity, so the centre is transparent and nothing
        is ever boosted. */
    std::pair<float, float> balanceGains (float pan) noexcept
    {
        return { pan > 0.0f ? 1.0f - pan : 1.0f,
                 pan < 0.0f ? 1.0f + pan : 1.0f };
    }
}

//==============================================================================
GainPanAudioProcessor::GainPanAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout()),
      presetManager (apvts,
                     fxme::PresetManager::getVendorPresetDirectory ("FxmeNetGainPan"),
                     BinaryData::namedResourceList,
                     BinaryData::namedResourceListSize,
                     BinaryData::getNamedResource)
{
    gainParam = apvts.getRawParameterValue ("gain");
    panParam  = apvts.getRawParameterValue ("pan");

    // FxmeNet: everything the editor reads that is not a parameter.
    auto& telemetry = net.telemetry();
    telemetry.addMeter ("inL",  inputMeters[0]);
    telemetry.addMeter ("inR",  inputMeters[1]);
    telemetry.addMeter ("outL", outputMeters[0]);
    telemetry.addMeter ("outR", outputMeters[1]);
}

juce::AudioProcessorValueTreeState::ParameterLayout GainPanAudioProcessor::createParameterLayout()
{
    using namespace juce;

    auto gain = std::make_unique<AudioParameterFloat> (
        ParameterID { "gain", 1 }, "Gain",
        NormalisableRange<float> (-60.0f, 12.0f, 0.1f, 2.0f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")
            .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; }));

    auto pan = std::make_unique<AudioParameterFloat> (
        ParameterID { "pan", 1 }, "Pan",
        NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int)
            {
                if (std::abs (v) < 0.005f)
                    return String ("C");
                return String (roundToInt (std::abs (v) * 100.0f)) + (v < 0.0f ? " L" : " R");
            }));

    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::move (gain), std::move (pan));
    return layout;
}

//==============================================================================
void GainPanAudioProcessor::prepareToPlay (double sampleRate, int)
{
    for (auto& m : inputMeters)
        m.prepare (sampleRate);
    for (auto& m : outputMeters)
        m.prepare (sampleRate);

    const auto [l, r] = balanceGains (panParam->load());
    const float g = juce::Decibels::decibelsToGain (gainParam->load(), -60.0f);
    leftGain.reset (sampleRate, 0.02);
    rightGain.reset (sampleRate, 0.02);
    leftGain.setCurrentAndTargetValue (g * l);
    rightGain.setCurrentAndTargetValue (g * r);
}

bool GainPanAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void GainPanAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    for (int ch = 0; ch < 2; ++ch)
        inputMeters[(size_t) ch].process (buffer.getReadPointer (ch), n);

    const auto [l, r] = balanceGains (panParam->load());
    const float g = juce::Decibels::decibelsToGain (gainParam->load(), -60.0f);
    leftGain.setTargetValue (g * l);
    rightGain.setTargetValue (g * r);
    leftGain.applyGain (buffer.getWritePointer (0), n);
    rightGain.applyGain (buffer.getWritePointer (1), n);

    for (int ch = 0; ch < 2; ++ch)
        outputMeters[(size_t) ch].process (buffer.getReadPointer (ch), n);
}

//==============================================================================
juce::AudioProcessorEditor* GainPanAudioProcessor::createEditor()
{
    return new GainPanAudioProcessorEditor (*this);
}

void GainPanAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (stateVersionAttribute, stateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void GainPanAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // Version 1 is the first: nothing to convert yet. A later version reads
    // stateVersionAttribute here and migrates older states before replacing.
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GainPanAudioProcessor();
}
