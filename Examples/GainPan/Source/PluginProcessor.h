/*
  ------------------------------------------------------------------------------
    PluginProcessor.h

    GainPan: gain and left/right balance on a stereo signal, with input and
    output level meters. The FxmeNet howto: everything that makes it
    FxmeNet-aware is marked "FxmeNet" below (an include, one member, one
    override, four registrations in the constructor). The editor has no
    FxmeNet code at all.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <FxmeTools/net/NetBridge.h>   // FxmeNet

#include <array>
#include <utility>

class GainPanAudioProcessor : public juce::AudioProcessor
{
public:
    GainPanAudioProcessor();
    ~GainPanAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return true; }

    const juce::String getName() const override             { return JucePlugin_Name; }
    bool acceptsMidi() const override                       { return false; }
    bool producesMidi() const override                      { return false; }
    bool isMidiEffect() const override                      { return false; }
    double getTailLengthSeconds() const override            { return 0.0; }

    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // FxmeNet: the server finds the bridge through the VST3 component.
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &net; }

    //==============================================================================
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept     { return apvts; }
    fxme::PresetManager& getPresetManager() noexcept           { return presetManager; }

    /** Level meters, channel 0 (left) or 1 (right). The editor reads them;
        under FxmeNet they are filled from the server instead of processBlock. */
    fxme::VuMeter& getInputMeter (int channel) noexcept        { return inputMeters[(size_t) channel]; }
    fxme::VuMeter& getOutputMeter (int channel) noexcept       { return outputMeters[(size_t) channel]; }

    /** True the first time it is called in this instance (the splash). */
    bool takeFirstSplash() noexcept                            { return ! std::exchange (splashShown, true); }

    static constexpr int stateVersion = 1;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    fxme::PresetManager presetManager;

    std::atomic<float>* gainParam = nullptr;   // dB
    std::atomic<float>* panParam  = nullptr;   // -1 (left) .. +1 (right)
    juce::LinearSmoothedValue<float> leftGain, rightGain;

    std::array<fxme::VuMeter, 2> inputMeters, outputMeters;

    // FxmeNet: declared after the meters it registers.
    fxme::net::NetBridge net;

    bool splashShown = false;   // not saved

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainPanAudioProcessor)
};
