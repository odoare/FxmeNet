/*
  ------------------------------------------------------------------------------
    PluginProcessor.h

    FxmeNetPlugin, tier 0: a chain of plugins processed on an fxmenet-server.
    The work is ClientEngine's (Common/FxmeNet/client); this class is the DAW
    glue:

      - buses of 1 to 64 channels in and out; each channel can be left out
        of the stream (the masks: a disabled input reaches the chain as
        silence, a disabled output is silent);
      - processBlock hands the buffers, the play head and isNonRealtime() to
        the engine; the DAW's bypass plays the input dry, delayed by the
        reported latency, as does a missing server;
      - the latency (re-blocking, network budget, the chain's own) is
        reported again whenever it changes;
      - the 256 pool parameters (ParameterPool.h): a slot's parameters are
        mapped to free pool parameters when it first loads, values travel
        both ways (PARAM_SET, PARAM_CHANGED) from a 50 Hz timer;
      - the state is the chain (each slot's state, fetched from the server
        when the DAW saves), the mapping, the server and the stream settings.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

#include <FxmeNet/client/ClientEngine.h>

#include "ParameterPool.h"

#include <array>
#include <mutex>
#include <set>
#include <utility>

class FxmeNetAudioProcessor : public juce::AudioProcessor,
                              private juce::Timer
{
public:
    FxmeNetAudioProcessor();
    ~FxmeNetAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

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

    //==============================================================================
    // Message thread (the editor).

    fxmenet::client::ClientEngine& getEngine() noexcept     { return engine; }

    /** "host" or "host:port"; empty disconnects. Remembered for new instances. */
    void connectTo (const juce::String& hostAndPort);
    juce::String getServer() const;

    /** Adds a server plugin at the end of the chain. */
    void addPlugin (const fxmenet::control::PluginEntry&);

    /** How many pool parameters are mapped. */
    int getNumMapped() const;

    static constexpr int stateVersion = 1;

private:
    struct StreamSettings
    {
        int blockSize = 128, budgetBlocks = 2;
        std::uint64_t inputMask = ~std::uint64_t (0), outputMask = ~std::uint64_t (0);

        bool operator== (const StreamSettings& o) const noexcept
        {
            return blockSize == o.blockSize && budgetBlocks == o.budgetBlocks
                && inputMask == o.inputMask && outputMask == o.outputMask;
        }
    };

    void process (juce::AudioBuffer<float>&, bool bypassed);
    void timerCallback() override;

    /** Re-prepares the engine with new stream settings, the audio suspended. */
    void applyStreamSettings (const StreamSettings&);

    // Timer jobs, message thread.
    void sendDirtyParameters();
    void applyServerParameterChanges();
    void followChain();

    std::array<fxmenet::plugin::PoolParameter*, fxmenet::plugin::poolSize> pool {};

    mutable std::mutex mappingLock;   ///< the mapping, read by getStateInformation on any thread
    std::array<fxmenet::plugin::Mapping, fxmenet::plugin::poolSize> mapping {};
    std::array<float, fxmenet::plugin::poolSize> lastSent {};   ///< echo suppression, message thread
    std::set<std::uint32_t> autoMapped;    ///< slots whose parameters were given pool entries already
    std::set<std::uint32_t> syncedSlots;   ///< loaded slots whose values the pool shows
    juce::String lastSlotsSignature;

    std::mutex serverChangesLock;
    std::vector<fxmenet::control::ParamChange> serverChanges;   ///< PARAM_CHANGED, from the connection thread

    mutable std::mutex settingsLock;
    juce::String server;
    StreamSettings stream;
    double preparedRate = 0.0;
    int preparedBlock = 0, preparedInputs = 0, preparedOutputs = 0;
    std::atomic<bool> catalogRequested { false };

    // Audio thread.
    std::array<const float*, 64> inPointers {};
    std::array<float*, 64> outPointers {};

    // Last, so it is destroyed first: its connection thread calls back into
    // the members above until it stops.
    fxmenet::client::ClientEngine engine { "FxmeNetPlugin" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxmeNetAudioProcessor)
};
