/*
  ------------------------------------------------------------------------------
    ChainProcessor.h

    The audio-thread core of a chain of plugins, shared by the server's
    Chain and, later, the plugin's local play (doc/fallback-plan.md): the
    slots processed in series, or through a RoutingPlan; a slot's bypass;
    the release of held notes at a break (MidiGuard); the chain's latency.

    The owner edits its slots on the message thread and publishes them as
    an immutable Snapshot through an atomic pointer; the audio thread
    processes whatever snapshot is current and never waits for an edit. The
    audio thread announces the snapshot it uses in a second atomic (a hazard
    pointer), and publish() frees a replaced snapshot only once the audio
    thread has let go of it. Slots are shared (shared_ptr) between
    snapshots, so a removed plugin instance is destroyed when the last
    snapshot holding it goes: on the message thread, as VST3 hosting
    requires. A slot is any ProcessorSlot subclass: the owner keeps its
    own fields there.

    Header-only (Common is linked into several JUCE targets).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <FxmeNet/chain/MidiGuard.h>
#include <FxmeNet/chain/RoutingEngine.h>
#include <FxmeNet/protocol/Wire.h>

#include <atomic>
#include <memory>
#include <vector>

namespace fxmenet::chain
{

/** What the audio thread needs of a slot. The owner derives its own slot
    type from it and sets `processor` (it keeps the instance's ownership).
    A null `processor` is a bridged slot: its audio passes through (a plugin
    not installed for the plugin's local play). */
struct ProcessorSlot
{
    virtual ~ProcessorSlot() = default;

    juce::AudioPluginInstance* processor = nullptr;
    std::atomic<bool> bypassed { false };
    bool wasBypassed = false;   ///< audio thread: as of the last block
    int numChannels = 2;        ///< max of the instance's inputs and outputs
};

class ChainProcessor
{
public:
    using SlotPtr = std::shared_ptr<ProcessorSlot>;

    ChainProcessor()
    {
        merged.ensureSize ((std::size_t) (8 * (int) wire::maxMidiBytes + MidiGuard::releaseSize()));
        releases.ensureSize ((std::size_t) MidiGuard::releaseSize());
    }

    ~ChainProcessor()
    {
        // The audio thread is stopped by now.
        delete current.exchange (nullptr);
    }

    //==============================================================================
    // Message thread.

    /** Publishes `slots` in order, through `plan` when routed (null: in
        series). Returns once the audio thread has let go of the previous
        snapshot (at most a block), which is then freed; false when it still
        held it after 2 s (a plugin stuck in processBlock): it is leaked
        rather than freed in use. */
    bool publish (std::vector<SlotPtr> slots, std::unique_ptr<RoutingPlan> plan)
    {
        int channels = 2;
        for (auto& s : slots)
            channels = juce::jmax (channels, s->numChannels);
        requiredChannels.store (channels);
        if (plan != nullptr)
        {
            latency.store (plan->getLatency());
            outputsFed.store (plan->getOutputsFed());
        }

        auto* old = current.exchange (new Snapshot { std::move (slots), std::move (plan) });
        if (old == nullptr)
            return true;

        const auto start = juce::Time::getMillisecondCounter();
        while (inUse.load() == old)
        {
            if (juce::Time::getMillisecondCounter() - start > 2000)
                return false;
            juce::Thread::sleep (1);
        }
        delete old;   // the last owner of a removed slot destroys its instance here
        return true;
    }

    /** The current plan (null: in series). Only the message thread replaces
        or frees it, so reading it there is safe: a new plan carries its
        delay lines on from it. */
    const RoutingPlan* getPlan() const noexcept
    {
        const auto* s = current.load();
        return s != nullptr ? s->plan.get() : nullptr;
    }

    /** True once after the audio thread saw a slot's latency differ from
        the plan's (routed): time to publish a new plan. */
    bool takeLatencyStale() noexcept            { return latencyStale.exchange (false); }

    //==============================================================================
    // Any thread.

    /** The chain's current latency: summed over the slots, or the plan's. */
    int getLatency() const noexcept             { return latency.load(); }

    /** The outputs the chain feeds (bit c: output c): all in series, those
        the routing connects when routed. */
    std::uint64_t getOutputsFed() const noexcept   { return outputsFed.load(); }

    /** The widest slot's channel count, as of the last publish. */
    int getRequiredChannels() const noexcept    { return requiredChannels.load(); }

    //==============================================================================
    // Audio thread.

    /** Processes `buffer` (at least getRequiredChannels() channels) through
        every slot. `offline` goes to the plugins (setNonRealtime). `reset`:
        every slot is reset first. `releaseNotes`: a break, the notes held
        are released before this block's events (MidiGuard); a slot bypassed
        while notes are held gets their release in one more processed
        block. `midi` must have room for MidiGuard::releaseSize() more
        bytes. */
    void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool offline, bool reset = false,
                  bool releaseNotes = false)
    {
        if (releaseNotes && guard.isHolding())
        {
            merged.clear();
            guard.addReleases (merged, 0);
            merged.addEvents (midi, 0, -1, 0);
            midi.clear();
            midi.addEvents (merged, 0, -1, 0);
            guard.clear();
        }
        guard.track (midi);

        // Hazard pointer: announce the snapshot, then check it is still
        // current, so publish() cannot free it between the two.
        Snapshot* s = nullptr;
        for (;;)
        {
            s = current.load();
            inUse.store (s);
            if (current.load() == s)
                break;
        }

        if (s != nullptr && s->plan != nullptr)
            processRouted (*s, buffer, midi, offline, reset);
        else if (s != nullptr)
        {
            int total = 0;
            for (auto& slot : s->slots)
            {
                if (slot->processor == nullptr)
                    continue;   // bridged: the buffer goes on as it is
                auto& instance = *slot->processor;
                if (instance.isNonRealtime() != offline)
                    instance.setNonRealtime (offline);
                if (reset)
                    instance.reset();   // a VST3: deactivated and activated again, so not free, but rare
                const int n = juce::jmin (slot->numChannels, buffer.getNumChannels());
                juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), n, buffer.getNumSamples());
                processSlot (*slot, view, midi);
                total += instance.getLatencySamples();
            }
            latency.store (total);
        }

        inUse.store (nullptr);
    }

private:
    struct Snapshot
    {
        std::vector<SlotPtr> slots;
        std::unique_ptr<RoutingPlan> plan;   ///< null: in series
    };

    void processRouted (Snapshot& s, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi,
                        bool offline, bool reset)
    {
        auto& plan = *s.plan;
        const int n = buffer.getNumSamples();
        if (reset)
            plan.clearDelays();

        // The plan copies the inputs first, then runs the slots on its own
        // buffers; the outputs are copied back into `buffer` at the end.
        plan.process (buffer.getArrayOfReadPointers(), buffer.getNumChannels(), n,
                      [&] (int index, float* const* channels, int width, int count)
        {
            auto& slot = *s.slots[(std::size_t) index];
            if (slot.processor == nullptr)
            {
                // Bridged: each output takes the input of the same index.
                juce::ignoreUnused (channels, width, count);
                return;
            }
            auto& instance = *slot.processor;
            if (instance.isNonRealtime() != offline)
                instance.setNonRealtime (offline);
            if (reset)
                instance.reset();

            juce::AudioBuffer<float> view (channels, width, count);
            processSlot (slot, view, midi);

            if (instance.getLatencySamples() != plan.getSlotLatency (index))
                latencyStale.store (true);   // the owner publishes a new plan
        });

        for (int c = 0; c < buffer.getNumChannels(); ++c)
        {
            if (const auto* out = plan.getOutput (c))
                buffer.copyFrom (c, 0, out, n);
            else
                buffer.clear (c, 0, n);
        }
    }

    /** One slot's block: bypassed or not, and the notes' release when it
        has just been bypassed. */
    void processSlot (ProcessorSlot& slot, juce::AudioBuffer<float>& view, juce::MidiBuffer& midi)
    {
        auto& instance = *slot.processor;
        const bool bypassed = slot.bypassed.load();
        if (bypassed && ! slot.wasBypassed && guard.isHolding())
        {
            // Just bypassed while notes are held: one more block processed,
            // with only their release, so that none sounds again when it
            // comes back.
            releases.clear();
            guard.addReleases (releases, 0);
            instance.processBlock (view, releases);
        }
        else if (bypassed)
        {
            instance.processBlockBypassed (view, midi);
        }
        else
        {
            instance.processBlock (view, midi);
        }
        slot.wasBypassed = bypassed;
    }

    std::atomic<Snapshot*> current { nullptr };
    std::atomic<Snapshot*> inUse { nullptr };

    MidiGuard guard;                    // audio thread
    juce::MidiBuffer merged, releases;  // audio thread: room for a release, no allocation

    std::atomic<bool> latencyStale { false };
    std::atomic<int> latency { 0 };
    std::atomic<std::uint64_t> outputsFed { ~std::uint64_t (0) };
    std::atomic<int> requiredChannels { 2 };

    JUCE_DECLARE_NON_COPYABLE (ChainProcessor)
};

} // namespace fxmenet::chain
