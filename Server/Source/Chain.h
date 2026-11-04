/*
  ------------------------------------------------------------------------------
    Chain.h

    A session's chain of plugin slots, processed in series.

    Edits (add, remove, move, bypass, clear, prepare) happen on the message
    thread and are published as an immutable Snapshot through an atomic
    pointer; the audio thread processes whatever snapshot is current and
    never waits for an edit. The audio thread announces the snapshot it uses
    in a second atomic (a hazard pointer), and the message thread frees a
    replaced snapshot only once the audio thread has let go of it. Slots are
    shared between snapshots, so a removed plugin instance is destroyed when
    the last snapshot holding it goes: on the message thread, as VST3 hosting
    requires.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <FxmeTools/net/NetAbi.h>
#include <FxmeNet/control/ControlMessages.h>
#include <FxmeNet/protocol/Wire.h>

#include <atomic>
#include <memory>
#include <vector>

namespace fxmenet::server
{

/** The DAW's play head as the client sent it with the current block. Written
    by the audio thread before processing, read by the plugins during it. */
class SessionPlayHead : public juce::AudioPlayHead
{
public:
    void set (const wire::Transport& t, double sampleRate);
    void clear()    { info = {}; }

    juce::Optional<PositionInfo> getPosition() const override   { return info; }

private:
    PositionInfo info;
};

/** Notes the parameters a plugin changes itself, for PARAM_CHANGED. The
    listener may be called on any thread, the audio thread included, so it
    only sets atomics; the message thread collects them. */
class ParamWatcher : private juce::AudioProcessorParameter::Listener
{
public:
    explicit ParamWatcher (juce::AudioPluginInstance&);
    ~ParamWatcher() override;

    /** Message thread: appends the parameters changed since the last call. */
    void collect (std::uint32_t slotId, std::vector<control::ParamChange>& out);

private:
    void parameterValueChanged (int index, float newValue) override;
    void parameterGestureChanged (int, bool) override {}

    juce::AudioPluginInstance& instance;
    std::vector<std::atomic<float>> values;
    std::vector<std::atomic<bool>> dirty;
};

class Chain
{
public:
    struct Slot
    {
        std::uint32_t slotId = 0;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        fxme::net::INetPlugin* net = nullptr;   ///< nullptr: not FxmeNet-aware
        std::atomic<bool> bypassed { false };
        int numChannels = 2;                     ///< max of the instance's inputs and outputs

        // Message thread only.
        std::unique_ptr<ParamWatcher> watcher;
        bool telemetryActive = false;            ///< the client's editor for it is open
        std::uint32_t telemetryFrame = 0;

        ~Slot();
    };

    Chain();
    ~Chain();

    //==============================================================================
    // Message thread.

    /** Sets the processing format and prepares every slot for it. */
    void prepare (double sampleRate, int blockSize);

    /** Takes an instance (already created), sets it up as a source (role,
        play head, state, prepareToPlay) and inserts it at `position`
        (clamped). Fills `ready`. Returns an error, or {}. */
    juce::String addSlot (std::uint32_t slotId, int position,
                          std::unique_ptr<juce::AudioPluginInstance> instance,
                          const std::vector<std::uint8_t>& state,
                          control::SlotReady& ready);

    bool removeSlot (std::uint32_t slotId);
    bool moveSlot (std::uint32_t slotId, int position);
    bool setBypass (std::uint32_t slotId, bool shouldBypass);
    void clear();

    /** A slot of the message thread's view, or nullptr. */
    Slot* findSlot (std::uint32_t slotId) const;

    bool hasSlot (std::uint32_t slotId) const   { return findSlot (slotId) != nullptr; }

    /** Calls fn (Slot&) for each slot, in chain order. */
    template <typename Function>
    void forEachSlot (Function&& fn) const
    {
        for (auto& s : slots)
            fn (*s);
    }

    SessionPlayHead& getPlayHead() noexcept     { return playHead; }

    //==============================================================================
    // Audio thread.

    /** Processes `buffer` (at least as many channels as any slot needs, see
        getRequiredChannels) through every slot in order. `offline` is the
        request's offline flag, passed on to the plugins (setNonRealtime: the
        VST3 host turns it into the process mode of this very call).
        `reset`: every slot is reset first (the client's plugin was, at
        this point of the stream: protocol.md section 5.1). */
    void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool offline, bool reset = false);

    /** The chain's current latency, summed over the slots (any thread). */
    int getLatency() const noexcept             { return latency.load(); }

    /** The widest slot's channel count, as of the last publish. */
    int getRequiredChannels() const noexcept    { return requiredChannels.load(); }

private:
    using SlotPtr = std::shared_ptr<Slot>;

    struct Snapshot
    {
        std::vector<SlotPtr> slots;
    };

    void publish();
    int indexOf (std::uint32_t slotId) const;

    std::vector<SlotPtr> slots;            // the message thread's view
    std::atomic<Snapshot*> current { nullptr };
    std::atomic<Snapshot*> inUse { nullptr };

    SessionPlayHead playHead;
    double sampleRate = 48000.0;
    int blockSize = 128;
    std::atomic<int> latency { 0 };
    std::atomic<int> requiredChannels { 2 };

    JUCE_DECLARE_NON_COPYABLE (Chain)
};

/** The parameter list of an instance, for SLOT_READY (protocol.md 4.4). */
std::vector<control::ParamInfo> describeParameters (juce::AudioPluginInstance&);

} // namespace fxmenet::server
