/*
  ------------------------------------------------------------------------------
    ReplyRing.h

    Audio replies, from the client's receive thread to its audio thread:
    `numSlots` blocks, block k in slot k % numSlots. JUCE-free, lock-free,
    no allocation after prepare().

    The audio thread says which block it will read next (setNextRead); the
    receive thread refuses replies older than that (too late: the block was
    concealed) or too far ahead to fit, and duplicates (a reply to a retry).
    So the slot the audio thread is reading is never written; each slot's
    tag is still checked before and after a read (a seqlock), so a broken
    assumption shows as a missing block, never as a torn one.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeNet/protocol/Wire.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace fxmenet::client
{

class ReplyRing
{
public:
    /** Not while either thread uses the ring. */
    void prepare (int numChannels, int blockSize, int numSlotsToUse = 64)
    {
        channels = std::max (0, numChannels);
        n = std::max (1, blockSize);
        numSlots = std::max (2, numSlotsToUse);
        samples.assign ((std::size_t) (numSlots * channels * n), 0.0f);
        tags = std::make_unique<std::atomic<std::uint64_t>[]> ((std::size_t) numSlots);
        for (int i = 0; i < numSlots; ++i)
            tags[(std::size_t) i].store (0);
        nextRead.store (0);
    }

    int getNumSlots() const noexcept  { return numSlots; }

    //==============================================================================
    /** Receive thread: stores a reply. Channel c of the block is the c-th
        channel set in the reply's mask counting from bit 0, written to
        channel (that bit's index); channels the reply does not carry are
        silent. False when refused (late, too far ahead, duplicate, wrong
        size). */
    bool write (const wire::BlockView& v)
    {
        const auto k = v.info.block;
        const auto first = nextRead.load (std::memory_order_acquire);
        if (k < first || k >= first + (std::uint64_t) numSlots || v.info.samples != n)
            return false;

        auto& tag = tags[(std::size_t) (k % (std::uint64_t) numSlots)];
        if (tag.load (std::memory_order_relaxed) == k + 1)
            return false;

        tag.store (0, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);

        float* slot = slotData (k);
        std::fill (slot, slot + channels * n, 0.0f);
        int ordinal = 0;
        for (int c = 0; c < wire::maxChannels; ++c)
        {
            if ((v.info.mask & (std::uint64_t (1) << c)) == 0)
                continue;
            if (c < channels)
                v.copyChannel (ordinal, slot + c * n);
            ++ordinal;
        }

        tag.store (k + 1, std::memory_order_release);
        return true;
    }

    //==============================================================================
    /** Audio thread: copies block k into `dest` (numChannels x N) and moves
        on to k + 1. False when block k is not there. */
    bool read (std::uint64_t k, float* const* dest)
    {
        auto& tag = tags[(std::size_t) (k % (std::uint64_t) numSlots)];
        bool ok = tag.load (std::memory_order_acquire) == k + 1;
        if (ok)
        {
            const float* slot = slotData (k);
            for (int c = 0; c < channels; ++c)
                std::copy (slot + c * n, slot + (c + 1) * n, dest[c]);
            std::atomic_thread_fence (std::memory_order_acquire);
            ok = tag.load (std::memory_order_relaxed) == k + 1;
        }
        setNextRead (k + 1);
        return ok;
    }

    /** Audio thread: the next block it will read (replies older are refused). */
    void setNextRead (std::uint64_t k) noexcept     { nextRead.store (k, std::memory_order_release); }

    /** Whether block k has arrived (any thread). */
    bool has (std::uint64_t k) const noexcept
    {
        return tags[(std::size_t) (k % (std::uint64_t) numSlots)].load (std::memory_order_acquire) == k + 1;
    }

private:
    float* slotData (std::uint64_t k) noexcept
    {
        return samples.data() + (std::size_t) (k % (std::uint64_t) numSlots) * (std::size_t) (channels * n);
    }

    int channels = 0, n = 128, numSlots = 64;
    std::vector<float> samples;
    std::unique_ptr<std::atomic<std::uint64_t>[]> tags;
    std::atomic<std::uint64_t> nextRead { 0 };
};

} // namespace fxmenet::client
