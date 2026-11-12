/*
  ------------------------------------------------------------------------------
    JsfxInstance.h

    A JSFX script as a plugin of the chain (doc/jsfx-plan.md, phase 2). The
    client sent the script with what it imports (a jsfx::Bundle, inside the
    slot's state); it is written to a temporary folder and loaded from there
    by the JSFX engine (the fxjsfx library: Cockos' sources without REAPER
    and without a window). To the chain it is an AudioPluginInstance like
    another: one parameter per slider, the script's pins as its channels,
    its state (bundle, sliders, @serialize) as the slot's state.

    The audio sections run here. @gfx never does: the client draws (phase 3).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeNet/jsfx/JsfxBundle.h>
#include <FxmeNet/jsfx/JsfxSync.h>

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <juce_events/juce_events.h>

#include <memory>
#include <vector>

class SX_Instance;

namespace fxmenet::server
{

/** A parameter that knows its values in the plugin's own units (a JSFX
    slider's range), for SLOT_READY. */
class RangedParameter
{
public:
    virtual ~RangedParameter() = default;
    /** False when there is no linear range to show. */
    virtual bool getRange (double& minimum, double& maximum, double& step) const = 0;
};

class JsfxInstance : public juce::AudioPluginInstance,
                     private juce::Timer
{
public:
    /** Loads the script of a slot state (a jsfx::SlotState, encoded). Null,
        with the reason (the compile error and its line), when it cannot.
        Message thread. */
    static std::unique_ptr<JsfxInstance> create (const std::vector<std::uint8_t>& slotState,
                                                 double sampleRate, juce::String& error);

    ~JsfxInstance() override;

    bool hasGfx() const;

    //==============================================================================
    // The mirror for the client's @gfx display (JsfxSync.h). Message thread.

    /** The editor opened (again): everything is sent anew. */
    void syncReset();
    void syncSubscribe (const std::vector<std::uint32_t>& blocks)   { sync.subscribe (blocks); }
    /** The next JSFX_SYNC, at most `budget` bytes of memory; false: nothing new. */
    bool syncCollect (std::uint32_t slotId, std::size_t budget, jsfx::SyncFrame&);

    //==============================================================================
    const juce::String getName() const override   { return name; }
    void fillInPluginDescription (juce::PluginDescription&) const override;

    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;

    bool isBusesLayoutSupported (const BusesLayout&) const override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override    { return true; }
    bool producesMidi() const override   { return true; }

    juce::AudioProcessorEditor* createEditor() override   { return nullptr; }
    bool hasEditor() const override                       { return false; }

    int getNumPrograms() override                               { return 1; }
    int getCurrentProgram() override                            { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return {}; }
    void changeProgramName (int, const juce::String&) override  {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int size) override;

private:
    class SliderParameter;
    struct Midi;

    JsfxInstance (jsfx::Bundle, juce::File folder, SX_Instance*, int channels);

    void timerCallback() override;
    static double midiHook (void* context, int action, double* time, double* message1, double* message23, double* bus);

    jsfx::Bundle bundle;
    juce::File folder;         ///< the temporary folder the script was loaded from
    SX_Instance* sx = nullptr;
    juce::String name;
    const int numChannels;

    double rate = 48000.0;
    std::vector<double> work;  ///< interleaved, as the engine wants it
    int workFrames = 0;
    std::unique_ptr<Midi> midi;
    jsfx::SyncSource sync;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JsfxInstance)
};

} // namespace fxmenet::server
