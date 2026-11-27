/*
  ------------------------------------------------------------------------------
    LocalChain.h

    Local play (doc/fallback-plan.md): the chain processed on this computer,
    with the slots' local copies (the shadows that show their editors, told
    `local`), through the same core as the server's (chain::ChainProcessor).
    ClientEngine hands it each block where it would send it to the server
    (client::LocalProcessor), so the latency reported to the DAW is the
    same.

    The processor publishes the slots (message thread) whenever the chain,
    its local copies or the routing change; a slot with no local copy is
    bridged (its audio passes through). The shadows stay owned by the
    processor, which publishes a chain without a shadow before deleting it.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

#include <FxmeNet/chain/ChainProcessor.h>
#include <FxmeNet/client/LocalProcessor.h>
#include <FxmeNet/protocol/Wire.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace fxmenet::plugin
{

class LocalChain : public client::LocalProcessor
{
public:
    struct Slot : chain::ProcessorSlot
    {
        std::uint32_t slotId = 0;
    };
    using SlotPtr = std::shared_ptr<Slot>;

    LocalChain()
    {
        midi.ensureSize (8 * wire::maxMidiBytes + (std::size_t) chain::MidiGuard::releaseSize());
    }

    /** Sizes the work buffer for blocks of `blockSize`. Message thread, with
        the audio stopped. */
    void prepare (int blockSize)
    {
        work.setSize (wire::maxChannels, juce::jmax (1, blockSize), false, true, false);
    }

    /** Publishes the slots in order (through `plan` when routed, null: in
        series). Returns once the audio thread no longer uses the previous
        ones. Message thread. */
    bool publish (const std::vector<SlotPtr>& slots, std::unique_ptr<chain::RoutingPlan> plan)
    {
        current = slots;
        return core.publish (std::vector<chain::ChainProcessor::SlotPtr> (slots.begin(), slots.end()), std::move (plan));
    }

    /** The slots last published (message thread). */
    const std::vector<SlotPtr>& getSlots() const noexcept   { return current; }

    /** The plan last published, for the next one's delay lines (message thread). */
    const chain::RoutingPlan* getPlan() const noexcept      { return core.getPlan(); }

    /** A slot's latency changed (routed): a new plan is due. Message thread. */
    bool takeLatencyStale() noexcept                        { return core.takeLatencyStale(); }

    //==============================================================================
    void processLocalBlock (const float* const* in, int numInputs, float* const* out, int numOutputs,
                            int numSamples, const std::uint8_t* midiBytes, std::size_t numMidiBytes,
                            bool offline, bool reset) override
    {
        if (numSamples > work.getNumSamples())
        {
            for (int c = 0; c < numOutputs; ++c)
                std::fill (out[c], out[c] + numSamples, 0.0f);   // not prepared for it
            return;
        }

        work.clear();
        for (int c = 0; c < numInputs && c < work.getNumChannels(); ++c)
            std::copy (in[c], in[c] + numSamples, work.getWritePointer (c));

        midi.clear();
        wire::MidiReader reader (midiBytes, numMidiBytes);
        wire::MidiEvent e;
        while (reader.next (e))
            midi.addEvent (e.data, e.size, juce::jlimit (0, numSamples - 1, (int) e.sampleOffset));

        juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), work.getNumChannels(), numSamples);
        core.process (view, midi, offline, reset, reset);

        for (int c = 0; c < numOutputs; ++c)
        {
            if (c < work.getNumChannels())
                std::copy (work.getReadPointer (c), work.getReadPointer (c) + numSamples, out[c]);
            else
                std::fill (out[c], out[c] + numSamples, 0.0f);
        }
    }

    int getLocalLatency() const override   { return core.getLatency(); }

private:
    chain::ChainProcessor core;
    std::vector<SlotPtr> current;   // message thread

    juce::AudioBuffer<float> work;  // audio thread, sized by prepare()
    juce::MidiBuffer midi;

    JUCE_DECLARE_NON_COPYABLE (LocalChain)
};

} // namespace fxmenet::plugin
