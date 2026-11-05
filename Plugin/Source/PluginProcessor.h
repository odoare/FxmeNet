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

#include "LocalCatalog.h"
#include "ParameterPool.h"
#include "Shadow.h"

#include <array>
#include <map>
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
    void reset() override                    { engine.reset(); }   // passed on to the server's chain
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return true; }

    const juce::String getName() const override             { return JucePlugin_Name; }
    bool acceptsMidi() const override                       { return true; }   // passed on to the server's chain
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
    fxmenet::plugin::LocalCatalog& getLocalCatalog() noexcept { return *localCatalog; }

    /** How a server plugin compares with the local plugins (its tier). */
    fxmenet::plugin::LocalMatch matchLocal (const fxmenet::control::PluginEntry& e) const   { return localCatalog->match (e); }

    //==============================================================================
    // Shadows (Shadow.h): local copies showing a slot's own editor.

    /** The slot's shadow, created on first request when the slot is loaded
        and its plugin is compatible here (tier 1 or 2). Null otherwise, with
        the reason in `why`. */
    fxmenet::plugin::Shadow* getShadow (std::uint32_t slotId, juce::String* why = nullptr);

    /** The last parameter moved (or grabbed) in a shadow's editor, slotId 0
        when none: what "Map last touched" acts on, since a plugin's own
        editor has no FxmeNet controls of its own. */
    fxmenet::plugin::Mapping getLastTouched() const noexcept   { return lastTouched; }

    /** A slot's own editor was shown or closed: the server sends its
        telemetry only while shown (TELEMETRY_ACTIVE). */
    void setEditorShown (std::uint32_t slotId, bool shown)   { engine.setTelemetryActive (slotId, shown); }

    /** The shadow of a slot if it exists (never creates one). */
    fxmenet::plugin::Shadow* findShadow (std::uint32_t slotId) const;

    /** Called before a shadow is deleted (its slot is gone or reloaded), so
        an editor showing its view can drop it first. Set by the editor. */
    std::function<void (std::uint32_t slotId)> beforeShadowRemoved;

    /** The server's catalog entry of a slot; false when the catalog does not
        have it (not received yet, or the plugin is gone from the server). */
    bool findCatalogEntry (const fxmenet::client::SlotModel&, fxmenet::control::PluginEntry& out) const;

    /** "host" or "host:port"; empty disconnects. Remembered for new instances. */
    void connectTo (const juce::String& hostAndPort);
    juce::String getServer() const;

    /** Adds a server plugin at the end of the chain. */
    void addPlugin (const fxmenet::control::PluginEntry&);

    /** How many pool parameters are mapped. */
    int getNumMapped() const;

    //==============================================================================
    // Stream settings: N, D and the channel masks (bit c: channel c sent, or
    // received). Changing them reconfigures the session (a moment of dry audio).

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

    StreamSettings getStreamSettings() const;
    void setStreamSettings (const StreamSettings&);

    /** The host block size and channel counts last prepared (0 before). */
    int getPreparedBlockSize() const;
    int getPreparedInputs() const;
    int getPreparedOutputs() const;

    //==============================================================================
    // The pool mapping (ParameterPool.h).

    using Mapping = fxmenet::plugin::Mapping;

    Mapping getMapping (int poolIndex) const;
    fxmenet::plugin::PoolParameter& getPoolParameter (int poolIndex)   { return *pool[(std::size_t) poolIndex]; }

    /** The pool entry mapped to a slot parameter, or -1. */
    int findPoolFor (Mapping target) const;

    /** Maps pool entry `poolIndex` to `target`: whatever the entry pointed to
        is dropped, and so is any other entry pointing to `target` (one pool
        entry per slot parameter). The entry takes the parameter's name and
        current value. */
    void mapPool (int poolIndex, Mapping target);
    void unmapPool (int poolIndex);

    /** Maps `target` to a free pool entry (pickFreePool); its index, or -1
        when the pool is full. */
    int mapToFreePool (Mapping target);

    /** Learn: the next pool parameter the DAW moves (automation, or its
        generic controls) is mapped to `target`. */
    void startLearn (Mapping target);
    void cancelLearn();
    Mapping getLearnTarget() const     { return learnTarget; }

    /** A slot parameter set from the editor: through its pool entry when it
        has one (so the DAW records it), else straight to the server. The
        gesture calls frame a drag, for the DAW's automation. */
    void setSlotParameter (Mapping target, float value);
    void beginSlotParameterGesture (Mapping target);
    void endSlotParameterGesture (Mapping target);

    static constexpr int stateVersion = 1;

private:

    void process (juce::AudioBuffer<float>&, bool bypassed, const juce::MidiBuffer* midi = nullptr);
    void timerCallback() override;

    /** Message thread: the engine's audio events, into the events log when
        it is on (AppSettings); the machine-wide switches followed. */
    void handleAudioEvents();
    int settingsPollTicks = 0;
    bool logEvents = false;

    /** Re-prepares the engine with new stream settings, the audio suspended. */
    void applyStreamSettings (const StreamSettings&);

    // Timer jobs, message thread.
    void sendDirtyParameters();
    void applyServerParameterChanges();
    void followChain();
    void syncShadows();
    void deliverTelemetry();
    void removeShadow (std::uint32_t slotId);

    /** With mappingLock held: names a mapped entry after its parameter;
        returns the parameter's current value (or -1 if not found). */
    float describeMapped (int poolIndex, const std::vector<fxmenet::client::SlotModel>& slots);

    Mapping learnTarget;   ///< message thread; slotId 0: not learning
    Mapping lastTouched;   ///< message thread

    std::array<fxmenet::plugin::PoolParameter*, fxmenet::plugin::poolSize> pool {};

    mutable std::mutex mappingLock;   ///< the mapping, read by getStateInformation on any thread
    std::array<fxmenet::plugin::Mapping, fxmenet::plugin::poolSize> mapping {};
    std::array<float, fxmenet::plugin::poolSize> lastSent {};   ///< echo suppression, message thread
    std::set<std::uint32_t> autoMapped;    ///< slots whose parameters were given pool entries already

    /** Pool entries ever mapped in this project (saved with it). The DAW's
        automation belongs to pool entries, not to what they control: an
        entry freed by a removed plugin keeps its lanes, so automatic
        allocation never hands it to another plugin (a new plugin's input
        gain once got the old one's automation, +24 dB). Only an explicit
        Learn reuses it, or allocation once all 256 have been used. */
    std::array<bool, fxmenet::plugin::poolSize> everUsed {};

    /** With mappingLock held: a free entry, never used if possible; -1 when
        none is free. */
    int pickFreePool() const;
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
    static constexpr int maxMidiEvents = 2048;   ///< per host callback; more are not sent
    std::array<fxmenet::client::MidiInput, maxMidiEvents> midiEvents {};

    juce::AudioPluginFormatManager shadowFormats;
    std::map<std::uint32_t, std::unique_ptr<fxmenet::plugin::Shadow>> shadows;   ///< message thread

    // One scan for every instance in the process.
    juce::SharedResourcePointer<fxmenet::plugin::LocalCatalog> localCatalog;

    // Last, so it is destroyed first: its connection thread calls back into
    // the members above until it stops.
    fxmenet::client::ClientEngine engine { "FxmeNetPlugin" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxmeNetAudioProcessor)
};
