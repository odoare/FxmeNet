/*
  ------------------------------------------------------------------------------
    ClientTests.cpp

    The client's audio-thread core (Common/FxmeNet/client), without JUCE and
    without a network: BlockStream against a fake network that echoes each
    block, for many host block sizes, block sizes and budgets, fixed and
    random; then concealment, and ReplyRing.

    What is checked for BlockStream:
      - the output is the input delayed by exactly getLatency() samples;
      - a block is never fetched in the callback that sent it, and its reply
        always had at least D x N samples of time (from the start of the
        callback that sent it to the start of the one that plays it);
      - blocks are cut where they should be (blockReady's endOffset).

    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeNet/client/BlockStream.h>
#include <FxmeNet/client/ReplyRing.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <vector>

namespace
{
    int failures = 0;
    int checks   = 0;

    bool check (bool condition, const char* what, int line)
    {
        ++checks;
        if (! condition)
        {
            ++failures;
            std::printf ("  FAIL (line %d): %s\n", line, what);
        }
        return condition;
    }

    #define CHECK(cond) check ((cond), #cond, __LINE__)

    using fxmenet::client::BlockStream;
    using fxmenet::client::ReplyRing;
    namespace wire = fxmenet::wire;

    /** Sample t of input channel c: an exact integer in a float. */
    float inputSample (int c, long t)   { return (float) (c * 1000000L + t + 1); }

    //==============================================================================
    /** Echoes each block: output channel c is input channel c % numInputs. */
    struct EchoNetwork
    {
        int nIn = 0, nOut = 0, n = 0;
        long callIndex = 0, callStart = 0;   ///< the callback in progress
        std::vector<std::vector<float>> blocks;
        std::vector<long> sentCall, sentStart;
        std::set<std::uint64_t> drop;   ///< blocks whose reply "never arrives"

        long causalityErrors = 0, cutErrors = 0, orderErrors = 0;
        long minBudget = 1L << 40;

        void blockReady (std::uint64_t k, const float* const* block, int endOffset)
        {
            if (k != blocks.size())
                ++orderErrors;
            if (callStart + endOffset - n != (long) k * n)
                ++cutErrors;

            std::vector<float> copy ((std::size_t) (nIn * n));
            for (int c = 0; c < nIn; ++c)
                std::copy (block[c], block[c] + n, copy.begin() + c * n);
            blocks.push_back (std::move (copy));
            sentCall.push_back (callIndex);
            sentStart.push_back (callStart);
        }

        bool fetchBlock (std::uint64_t k, float* const* block)
        {
            if (k >= blocks.size())
            {
                ++causalityErrors;
                return false;
            }
            if (sentCall[k] >= callIndex)
                ++causalityErrors;
            minBudget = std::min (minBudget, callStart - sentStart[k]);
            if (drop.count (k) != 0)
                return false;

            for (int c = 0; c < nOut; ++c)
                for (int i = 0; i < n; ++i)
                    block[c][i] = nIn > 0 ? blocks[k][(std::size_t) ((c % nIn) * n + i)] : 0.0f;
            return true;
        }
    };

    struct RunResult
    {
        std::vector<std::vector<float>> out;
        int latency = 0;
        long latencyChangedAt = -1;   ///< output position where the latency changed, or -1
    };

    /** Runs `total` samples through a BlockStream in callbacks of `sizes`
        (cycled). */
    RunResult run (BlockStream& stream, EchoNetwork& net, int nIn, int nOut,
                   const std::vector<int>& sizes, long total)
    {
        RunResult r;
        r.out.assign ((std::size_t) nOut, std::vector<float> ((std::size_t) total, 0.0f));
        std::vector<std::vector<float>> inBuf ((std::size_t) nIn), outBuf ((std::size_t) nOut);
        std::vector<const float*> inPtrs ((std::size_t) nIn);
        std::vector<float*> outPtrs ((std::size_t) nOut);
        const int startLatency = stream.getLatency();

        long pos = 0;
        for (std::size_t call = 0; pos < total; ++call)
        {
            const int size = (int) std::min<long> (sizes[call % sizes.size()], total - pos);
            for (int c = 0; c < nIn; ++c)
            {
                inBuf[(std::size_t) c].resize ((std::size_t) size);
                for (int i = 0; i < size; ++i)
                    inBuf[(std::size_t) c][(std::size_t) i] = inputSample (c, pos + i);
                inPtrs[(std::size_t) c] = inBuf[(std::size_t) c].data();
            }
            for (int c = 0; c < nOut; ++c)
            {
                outBuf[(std::size_t) c].assign ((std::size_t) size, -1.0f);
                outPtrs[(std::size_t) c] = outBuf[(std::size_t) c].data();
            }

            net.callIndex = (long) call;
            net.callStart = pos;
            stream.process (inPtrs.data(), outPtrs.data(), size, net);

            for (int c = 0; c < nOut; ++c)
                std::copy (outBuf[(std::size_t) c].begin(), outBuf[(std::size_t) c].end(),
                           r.out[(std::size_t) c].begin() + pos);
            if (r.latencyChangedAt < 0 && stream.getLatency() != startLatency)
                r.latencyChangedAt = pos;
            pos += size;
        }
        r.latency = stream.getLatency();
        return r;
    }

    /** Output position t, from `from` on, is input t - latency (silence before 0). */
    bool isDelayedInput (const RunResult& r, int nIn, int latency, long from, long total)
    {
        for (std::size_t c = 0; c < r.out.size(); ++c)
            for (long t = from; t < total; ++t)
            {
                const float expected = t >= latency ? inputSample ((int) c % nIn, t - latency) : 0.0f;
                if (r.out[c][(std::size_t) t] != expected)
                {
                    std::printf ("    channel %d, sample %ld: %g, expected %g\n",
                                 (int) c, t, (double) r.out[c][(std::size_t) t], (double) expected);
                    return false;
                }
            }
        return true;
    }

    //==============================================================================
    void testFixedSizes()
    {
        std::printf ("BlockStream, fixed host block sizes\n");
        int cases = 0;
        for (int n : { 64, 128, 256 })
            for (int host : { 32, 64, 100, 128, 200, 256, 300, 441, 512, 1024, 2048 })
                for (int d : { 1, 2, 3, 5 })
                {
                    const int nIn = 2, nOut = 3;
                    BlockStream stream;
                    stream.prepare (nIn, nOut, n, host, d);
                    EchoNetwork net;
                    net.nIn = nIn;
                    net.nOut = nOut;
                    net.n = n;

                    // Whole callbacks: a shorter last one would (rightly)
                    // switch an aligned stream to the larger latency.
                    const long total = 40L * host * ((n + host - 1) / host);
                    const auto r = run (stream, net, nIn, nOut, { host }, total);
                    const bool aligned = host % n == 0;
                    ++cases;

                    if (! CHECK (r.latency == BlockStream::latencyFor (n, host, d, aligned))
                        || ! CHECK (r.latencyChangedAt < 0)
                        || ! CHECK (isDelayedInput (r, nIn, r.latency, 0, total))
                        || ! CHECK (net.causalityErrors == 0 && net.cutErrors == 0 && net.orderErrors == 0)
                        || ! CHECK (net.minBudget >= (long) d * n)
                        || ! CHECK (stream.getConcealedCount() == 0))
                        std::printf ("    N %d, host %d, D %d: latency %d, min budget %ld\n",
                                     n, host, d, r.latency, net.minBudget);
                }
        std::printf ("  %d configurations\n", cases);
    }

    void testRandomSizes()
    {
        std::printf ("BlockStream, random host block sizes\n");
        std::mt19937 rng (12345);
        int cases = 0;
        for (int n : { 64, 128 })
            for (int maxHost : { 100, 128, 512, 1000 })
                for (int d : { 1, 2, 4 })
                {
                    // Sizes not all multiples of N: from the start, so no
                    // latency change in the middle.
                    std::vector<int> sizes;
                    std::uniform_int_distribution<int> pick (1, maxHost);
                    sizes.push_back (maxHost % n == 0 ? n / 2 + 1 : maxHost);
                    for (int i = 0; i < 997; ++i)
                        sizes.push_back (pick (rng));

                    BlockStream stream;
                    stream.prepare (2, 2, n, maxHost, d);
                    EchoNetwork net;
                    net.nIn = 2;
                    net.nOut = 2;
                    net.n = n;
                    const long total = 300L * maxHost;
                    const auto r = run (stream, net, 2, 2, sizes, total);
                    ++cases;

                    // Aligned at prepare() when maxHost is a multiple of N:
                    // the first block switches it, before any output.
                    if (! CHECK (r.latency == BlockStream::latencyFor (n, maxHost, d, false))
                        || ! CHECK (isDelayedInput (r, 2, r.latency, 0, total))
                        || ! CHECK (net.causalityErrors == 0 && net.cutErrors == 0)
                        || ! CHECK (net.minBudget >= (long) d * n)
                        || ! CHECK (stream.getConcealedCount() == 0))
                        std::printf ("    N %d, max host %d, D %d: latency %d, min budget %ld\n",
                                     n, maxHost, d, r.latency, net.minBudget);
                }

        // Random multiples of N stay aligned.
        for (int d : { 1, 2 })
        {
            const int n = 128, maxHost = 1024;
            std::vector<int> sizes;
            std::uniform_int_distribution<int> pick (1, maxHost / n);
            for (int i = 0; i < 500; ++i)
                sizes.push_back (pick (rng) * n);

            BlockStream stream;
            stream.prepare (1, 1, n, maxHost, d);
            EchoNetwork net;
            net.nIn = 1;
            net.nOut = 1;
            net.n = n;
            const long total = 200L * maxHost;
            const auto r = run (stream, net, 1, 1, sizes, total);
            ++cases;
            if (! CHECK (stream.isAligned())
                || ! CHECK (r.latency == BlockStream::latencyFor (n, maxHost, d, true))
                || ! CHECK (isDelayedInput (r, 1, r.latency, 0, total))
                || ! CHECK (net.minBudget >= (long) d * n))
                std::printf ("    multiples of N, D %d: latency %d, min budget %ld\n", d, r.latency, net.minBudget);
        }
        std::printf ("  %d configurations\n", cases);
    }

    void testAlignmentLost()
    {
        std::printf ("BlockStream, alignment lost in the middle\n");
        const int n = 128, host = 512, d = 2;
        BlockStream stream;
        stream.prepare (2, 2, n, host, d);
        EchoNetwork net;
        net.nIn = 2;
        net.nOut = 2;
        net.n = n;

        std::vector<int> sizes (50, host);
        sizes.push_back (100);   // one odd callback, then 512 again
        for (int i = 0; i < 200; ++i)
            sizes.push_back (host);
        long sum = 0;
        for (int s : sizes)
            sum += s;
        const auto r = run (stream, net, 2, 2, sizes, sum);

        const int before = BlockStream::latencyFor (n, host, d, true);
        const int after = BlockStream::latencyFor (n, host, d, false);
        CHECK (! stream.isAligned());
        CHECK (r.latency == after);
        CHECK (r.latencyChangedAt == 50L * host);
        CHECK (isDelayedInput ({ r.out, 0, -1 }, 2, before, 0, 50L * host));
        // Once the extra silence has played out, the larger latency holds.
        CHECK (isDelayedInput (r, 2, after, 50L * host + after, sum));
        CHECK (net.causalityErrors == 0 && net.cutErrors == 0);
        CHECK (net.minBudget >= (long) d * n);
    }

    void testConcealment()
    {
        std::printf ("BlockStream, concealment\n");
        const int n = 128, host = 128, d = 2;
        BlockStream stream;
        stream.prepare (1, 1, n, host, d);
        EchoNetwork net;
        net.nIn = 1;
        net.nOut = 1;
        net.n = n;
        net.drop = { 10 };

        const long total = 30L * host;
        const auto r = run (stream, net, 1, 1, { host }, total);
        const int latency = r.latency;
        const auto& out = r.out[0];
        const long gapStart = latency + 10L * n, gapEnd = gapStart + n;

        CHECK (stream.getConcealedCount() == 1);
        // The rest of the missing block is silent.
        bool silent = true;
        for (long t = gapStart + BlockStream::fadeLength; t < gapEnd; ++t)
            silent = silent && out[(std::size_t) t] == 0.0f;
        CHECK (silent);
        // The block before is untouched; its last sample is held and ramped
        // down over the first fadeLength samples of the gap...
        const int fade = BlockStream::fadeLength;
        const float held = inputSample (0, gapStart - 1 - latency);
        CHECK (out[(std::size_t) (gapStart - 1)] == held);
        CHECK (std::abs (out[(std::size_t) gapStart] - held * (float) (fade - 1) / (float) fade) < 1.0f);
        CHECK (std::abs (out[(std::size_t) (gapStart + fade / 2)] - held * (float) (fade / 2 - 1) / (float) fade) < 1.0f);
        CHECK (out[(std::size_t) (gapStart + fade - 1)] == 0.0f);
        // ...and the block after fades in.
        CHECK (out[(std::size_t) gapEnd] == 0.0f);
        CHECK (std::abs (out[(std::size_t) (gapEnd + fade / 2)]
                         - 0.5f * inputSample (0, gapEnd + fade / 2 - latency)) < 1.0f);
        CHECK (out[(std::size_t) (gapEnd + fade)] == inputSample (0, gapEnd + fade - latency));
        // Everything else is untouched.
        RunResult tail { r.out, latency, -1 };
        CHECK (isDelayedInput (tail, 1, latency, gapEnd + fade, total));
    }

    //==============================================================================
    std::vector<std::uint8_t> encodeReply (std::uint64_t block, int samples, std::uint64_t mask, float base)
    {
        const int count = wire::countChannels (mask);
        std::vector<std::vector<float>> audio ((std::size_t) count, std::vector<float> ((std::size_t) samples));
        std::vector<const float*> ptrs;
        for (int c = 0; c < count; ++c)
        {
            for (int i = 0; i < samples; ++i)
                audio[(std::size_t) c][(std::size_t) i] = base + (float) (c * 1000 + i);
            ptrs.push_back (audio[(std::size_t) c].data());
        }
        wire::BlockInfo info;
        info.block = block;
        info.samples = (std::uint16_t) samples;
        info.mask = mask;
        std::vector<std::uint8_t> body;
        wire::ByteWriter w (body);
        wire::writeBlock (w, true, info, nullptr, 0, ptrs.data());
        return body;
    }

    bool storeReply (ReplyRing& ring, std::uint64_t block, int samples, std::uint64_t mask, float base)
    {
        const auto body = encodeReply (block, samples, mask, base);
        wire::BlockView v;
        return wire::readBlock (body.data(), body.size(), true, 0, v) && ring.write (v);
    }

    void testReplyRing()
    {
        std::printf ("ReplyRing\n");
        const int n = 16, channels = 4;
        ReplyRing ring;
        ring.prepare (channels, n, 8);

        std::vector<std::vector<float>> dest ((std::size_t) channels, std::vector<float> ((std::size_t) n));
        std::vector<float*> ptrs;
        for (auto& d : dest)
            ptrs.push_back (d.data());

        // Mask 0b1010: channels 1 and 3 carried, 0 and 2 silent.
        CHECK (storeReply (ring, 0, n, 0b1010, 0.0f));
        CHECK (! storeReply (ring, 0, n, 0b1010, 5.0f));    // duplicate
        CHECK (! storeReply (ring, 8, n, 0b1010, 0.0f));    // too far ahead
        CHECK (! storeReply (ring, 1, n + 1, 0b1010, 0.0f)); // wrong size
        CHECK (ring.has (0) && ! ring.has (1));

        CHECK (ring.read (0, ptrs.data()));
        CHECK (dest[0][3] == 0.0f && dest[2][3] == 0.0f);
        CHECK (dest[1][3] == 3.0f);        // first channel carried
        CHECK (dest[3][3] == 1003.0f);     // second channel carried

        CHECK (! storeReply (ring, 0, n, 0b1010, 0.0f));    // late: already read
        CHECK (! ring.read (1, ptrs.data()));               // never arrived
        CHECK (! storeReply (ring, 1, n, 0b1010, 0.0f));    // late now
        CHECK (storeReply (ring, 9, n, 0b0001, 7.0f));      // 2..9 fit in 8 slots
        CHECK (! storeReply (ring, 10, n, 0b0001, 7.0f));

        // A mask beyond the ring's channels: extra channels dropped.
        CHECK (storeReply (ring, 2, n, 0xF0F, 1.0f));
        CHECK (ring.read (2, ptrs.data()));
        CHECK (dest[0][0] == 1.0f && dest[3][0] == 3001.0f);

        for (std::uint64_t k = 3; k < 9; ++k)
            CHECK (! ring.read (k, ptrs.data()));
        CHECK (ring.read (9, ptrs.data()));
        CHECK (dest[0][2] == 9.0f && dest[1][2] == 0.0f);
    }
}

int main()
{
    testFixedSizes();
    testRandomSizes();
    testAlignmentLost();
    testConcealment();
    testReplyRing();

    std::printf ("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
