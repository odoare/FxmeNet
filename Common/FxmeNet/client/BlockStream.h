/*
  ------------------------------------------------------------------------------
    BlockStream.h

    The client's audio-thread core (protocol.md section 5.1): cuts the
    host's buffers, whatever their size, into the session's N-sample
    blocks, hands each full block to the network, and plays each block's
    processed result back exactly `getLatency()` samples later. JUCE-free,
    no allocation after prepare(), no locks.

    Output works as a jitter buffer: a FIFO prefilled with `latency` samples
    of silence, topped up one block at a time, only when the host needs the
    samples (so each reply gets as long as possible to arrive). A block whose
    reply is not there when needed is played as silence: the samples before
    it, if not yet played, fade out, and the next good block fades in
    (`fadeLength` samples).

    The latency, B being the host's largest block and D the budget in blocks:

        host blocks multiples of N:  latency = D * N + B - N
        otherwise:                   latency = D * N + B + N - 1

    Either way a reply has at least D * N samples of time (counted from the
    start of the callback that sent its block to the start of the one that
    plays it). With blocks that are multiples of N, block boundaries fall on
    callback boundaries, and B - N covers the blocks a large callback
    completes before its last one. Otherwise a block may complete at the end
    of one callback and be needed by the next, hence the extra. A host block
    that is not a multiple of N (fixed, or occasional, as some hosts do)
    switches to the second formula, once, the first time it happens: 2N - 1
    samples of silence then, and a latency change to report.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace fxmenet::client
{

class BlockStream
{
public:
    static constexpr int fadeLength = 64;

    /** The latency for a configuration (formula above). */
    static int latencyFor (int blockSize, int maxHostBlock, int budgetBlocks, bool aligned) noexcept
    {
        return budgetBlocks * blockSize + maxHostBlock + (aligned ? -blockSize : blockSize - 1);
    }

    //==============================================================================
    /** Allocates and resets. Not on the audio thread. `maxHostBlock` is the
        largest buffer the host announced; larger ones are cut into pieces. */
    void prepare (int numInputs, int numOutputs, int blockSize, int maxHostBlock, int budgetBlocks)
    {
        nIn = std::max (0, numInputs);
        nOut = std::max (0, numOutputs);
        n = std::max (1, blockSize);
        maxHost = std::max (1, maxHostBlock);
        budget = std::max (1, budgetBlocks);
        capacity = latencyFor (n, maxHost, budget, false) + maxHost + n;

        inBlock.assign ((std::size_t) (nIn * n), 0.0f);
        outBlock.assign ((std::size_t) (nOut * n), 0.0f);
        fifo.assign ((std::size_t) (nOut * capacity), 0.0f);
        lastSample.assign ((std::size_t) nOut, 0.0f);

        inPointers.clear();
        for (int c = 0; c < nIn; ++c)
            inPointers.push_back (inBlock.data() + c * n);
        outPointers.clear();
        for (int c = 0; c < nOut; ++c)
            outPointers.push_back (outBlock.data() + c * n);

        reset();
    }

    /** Back to block 0, the FIFO full of silence. Audio thread, or with the
        audio stopped. */
    void reset() noexcept
    {
        aligned = maxHost % n == 0;
        latency.store (latencyFor (n, maxHost, budget, aligned));
        fill = 0;
        nextBlock = 0;
        nextFetch = 0;
        std::fill (fifo.begin(), fifo.end(), 0.0f);
        readPos = 0;
        available = latency.load();
        std::fill (lastSample.begin(), lastSample.end(), 0.0f);
        fadeInPending = false;
    }

    //==============================================================================
    /** One host callback. `in` has numInputs channels, `out` numOutputs, both
        `numSamples` long; they may be the same buffers. The network:

          void blockReady (std::uint64_t k, const float* const* block, int endOffset)
              block k is full (numInputs pointers, N samples each); its last
              sample was at endOffset - 1 in this call's buffers (so it began
              at endOffset - N, earlier than this call when negative).

          bool fetchBlock (std::uint64_t k, float* const* block)
              fill numOutputs x N samples with block k's result; false when it
              is not there (it is then concealed).

        fetchBlock (k) is only ever called after blockReady (k), in order. */
    template <typename Network>
    void process (const float* const* in, float* const* out, int numSamples, Network& network)
    {
        for (int done = 0; done < numSamples;)
        {
            const int chunk = std::min (numSamples - done, maxHost);
            processChunk (in, out, done, chunk, network);
            done += chunk;
        }
    }

    //==============================================================================
    int getLatency() const noexcept                 { return latency.load(); }   ///< any thread
    int getBlockSize() const noexcept               { return n; }
    int getBudgetBlocks() const noexcept            { return budget; }
    bool isAligned() const noexcept                 { return aligned; }
    std::uint64_t getBlocksSent() const noexcept    { return nextBlock; }

    /** Blocks played as silence because their reply was missing (any thread). */
    std::uint64_t getConcealedCount() const noexcept   { return concealed.load (std::memory_order_relaxed); }

private:
    template <typename Network>
    void processChunk (const float* const* in, float* const* out, int offset, int count, Network& network)
    {
        if (aligned && count % n != 0)
        {
            aligned = false;
            const int newLatency = latencyFor (n, maxHost, budget, false);
            pushSilence (newLatency - latency.load());
            latency.store (newLatency);
        }

        // Input: into blocks, each sent as soon as it is full.
        for (int pos = 0; pos < count;)
        {
            const int take = std::min (count - pos, n - fill);
            for (int c = 0; c < nIn; ++c)
                std::copy (in[c] + offset + pos, in[c] + offset + pos + take, inPointers[(std::size_t) c] + fill);
            fill += take;
            pos += take;
            if (fill == n)
            {
                network.blockReady (nextBlock++, inPointers.data(), offset + pos);
                fill = 0;
            }
        }

        // Output: as many blocks as needed, then the samples.
        while (available < count)
            fetchNext (network);

        for (int c = 0; c < nOut; ++c)
        {
            const float* ring = fifo.data() + c * capacity;
            float* dest = out[c] + offset;
            const int first = std::min (count, capacity - readPos);
            std::copy (ring + readPos, ring + readPos + first, dest);
            std::copy (ring, ring + (count - first), dest + first);
        }
        readPos = (readPos + count) % capacity;
        available -= count;
    }

    template <typename Network>
    void fetchNext (Network& network)
    {
        // Never true with the latency above; kept so that a wrong latency
        // costs silence, never a block played out of place.
        const bool sent = nextFetch < nextBlock;
        const bool ok = sent && network.fetchBlock (nextFetch, outPointers.data());
        ++nextFetch;

        if (! ok)
        {
            concealed.fetch_add (1, std::memory_order_relaxed);
            const int len = std::min (fadeLength, n);
            for (int c = 0; c < nOut; ++c)
            {
                float* dest = outPointers[(std::size_t) c];
                const float held = lastSample[(std::size_t) c];
                for (int i = 0; i < len; ++i)
                    dest[i] = held * (float) (len - 1 - i) / (float) len;
                std::fill (dest + len, dest + n, 0.0f);
            }
            fadeInPending = true;
        }
        else if (fadeInPending)
        {
            const int len = std::min (fadeLength, n);
            for (int c = 0; c < nOut; ++c)
                for (int i = 0; i < len; ++i)
                    outPointers[(std::size_t) c][i] *= (float) i / (float) len;
            fadeInPending = false;
        }

        pushBlock();
    }

    void pushBlock() noexcept
    {
        const int writePos = (readPos + available) % capacity;
        const int first = std::min (n, capacity - writePos);
        for (int c = 0; c < nOut; ++c)
        {
            float* ring = fifo.data() + c * capacity;
            const float* src = outPointers[(std::size_t) c];
            std::copy (src, src + first, ring + writePos);
            std::copy (src + first, src + n, ring);
            lastSample[(std::size_t) c] = src[n - 1];
        }
        available += n;
    }

    void pushSilence (int count) noexcept
    {
        const int writePos = (readPos + available) % capacity;
        for (int c = 0; c < nOut; ++c)
            for (int i = 0; i < count; ++i)
                fifo[(std::size_t) (c * capacity + (writePos + i) % capacity)] = 0.0f;
        std::fill (lastSample.begin(), lastSample.end(), 0.0f);
        available += count;
    }

    int nIn = 0, nOut = 0, n = 128, maxHost = 512, budget = 2, capacity = 0;
    bool aligned = true;
    std::atomic<int> latency { 0 };

    std::vector<float> inBlock, outBlock, fifo, lastSample;
    std::vector<float*> inPointers, outPointers;
    int fill = 0;
    std::uint64_t nextBlock = 0, nextFetch = 0;
    int readPos = 0, available = 0;
    bool fadeInPending = false;
    std::atomic<std::uint64_t> concealed { 0 };
};

} // namespace fxmenet::client
