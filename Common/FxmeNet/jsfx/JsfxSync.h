/*
  ------------------------------------------------------------------------------
    JsfxSync.h

    The mirror of a JSFX script's state for its @gfx display on the client
    (doc/jsfx-plan.md, phase 3). The server runs the script's audio code; the
    client runs only its @gfx code, in a second instance of the same script,
    on a copy of the variables and of the memory @gfx reads:

      - while the slot's editor is open (TELEMETRY_ACTIVE on), the server
        sends JSFX_SYNC at the session's telemetry rate: the variables that
        changed, and the changed parts of the memory blocks the client
        subscribed to;
      - the first JSFX_SYNC after TELEMETRY_ACTIVE (on) is a reset: the
        variables' names (their order gives the indices used after), every
        value, and nothing subscribed. The client starts a fresh instance;
      - the client subscribes (JSFX_SUBSCRIBE) to a memory block when its
        @gfx first touches it. The server then sends what differs from zero,
        and after that what changed (in chunks of chunkItems).

    Each frame of a slot is bounded (budget): what does not fit waits for
    the next frame. A client then sees a slower display, not more traffic.

    JUCE-free: SyncSource (server side) and the codec are tested without it.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/net/TelemetryCodec.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fxmenet::jsfx
{

constexpr std::uint32_t blockItems = 65536;      ///< a block of the script's memory (EEL2)
constexpr std::uint32_t chunkItems = 256;        ///< the unit compared and sent
constexpr std::uint32_t maxBlocks = 2048;        ///< EEL2's limit: 128 Mi items
constexpr std::size_t maxSubscribedBlocks = 64;  ///< 32 MiB of copies on the server, per slot
constexpr std::size_t defaultFrameBudget = 64 * 1024;

/** One JSFX_SYNC. Values travel as the bits of their doubles. */
struct SyncFrame
{
    std::uint32_t slotId = 0;
    std::uint32_t frame = 0;
    bool reset = false;
    std::vector<std::string> names;   ///< reset only: the variables, by index

    struct Var
    {
        std::uint32_t index;
        double value;
    };
    std::vector<Var> vars;

    struct Range
    {
        std::uint32_t offset;           ///< the first item (block * blockItems + item)
        std::vector<double> values;     ///< never across a block's end
    };
    std::vector<Range> ranges;
};

namespace detail
{
    inline std::uint64_t bits (double v)       { std::uint64_t b; std::memcpy (&b, &v, 8); return b; }
    inline double fromBits (std::uint64_t b)   { double v; std::memcpy (&v, &b, 8); return v; }
}

/** JSFX_SYNC's payload: u32 slotId, u32 frame, u8 flags (1: reset),
    [reset: u32 count, count x (u16 length, bytes)], u32 count, count x (u32
    index, f64 value), u32 count, count x (u32 offset, u32 n, n x f64). */
inline void encode (const SyncFrame& f, std::vector<std::uint8_t>& out)
{
    out.clear();
    fxme::net::ByteWriter w (out);
    w.u32 (f.slotId);
    w.u32 (f.frame);
    w.u8 (f.reset ? 1 : 0);
    if (f.reset)
    {
        w.u32 ((std::uint32_t) f.names.size());
        for (auto& n : f.names)
        {
            const auto size = std::min<std::size_t> (n.size(), 65535);
            w.u16 ((std::uint16_t) size);
            w.bytes (n.data(), size);
        }
    }
    w.u32 ((std::uint32_t) f.vars.size());
    for (auto& v : f.vars)
    {
        w.u32 (v.index);
        w.u64 (detail::bits (v.value));
    }
    w.u32 ((std::uint32_t) f.ranges.size());
    for (auto& r : f.ranges)
    {
        w.u32 (r.offset);
        w.u32 ((std::uint32_t) r.values.size());
        for (auto v : r.values)
            w.u64 (detail::bits (v));
    }
}

/** False when the payload is not a whole SyncFrame, or a range would leave
    its block or the memory. */
inline bool decode (const std::uint8_t* data, std::size_t size, SyncFrame& f)
{
    fxme::net::ByteReader r (data, size);
    f = {};
    f.slotId = r.u32();
    f.frame = r.u32();
    f.reset = (r.u8() & 1) != 0;
    if (f.reset)
    {
        const auto n = r.u32();
        if (! r.ok() || n > 1u << 20)
            return false;
        f.names.reserve (n);
        for (std::uint32_t i = 0; i < n && r.ok(); ++i)
        {
            const auto length = r.u16();
            const auto* p = r.take (length);
            if (p == nullptr && length > 0)
                return false;
            f.names.emplace_back ((const char*) p, length);
        }
    }
    const auto numVars = r.u32();
    if (! r.ok() || (std::size_t) numVars * 12 > size)
        return false;
    f.vars.resize (numVars);
    for (auto& v : f.vars)
    {
        v.index = r.u32();
        v.value = detail::fromBits (r.u64());
    }
    const auto numRanges = r.u32();
    if (! r.ok() || (std::size_t) numRanges * 8 > size)
        return false;
    f.ranges.resize (numRanges);
    for (auto& range : f.ranges)
    {
        range.offset = r.u32();
        const auto n = r.u32();
        if (! r.ok() || n == 0 || n > blockItems
            || range.offset / blockItems >= maxBlocks
            || range.offset % blockItems + n > blockItems
            || (std::size_t) n * 8 > size)
            return false;
        range.values.resize (n);
        for (auto& v : range.values)
            v = detail::fromBits (r.u64());
    }
    return r.ok();
}

/** JSFX_SUBSCRIBE's payload: u32 slotId, u32 count, count x u32 block. */
inline std::vector<std::uint8_t> encodeSubscribe (std::uint32_t slotId, const std::vector<std::uint32_t>& blocks)
{
    std::vector<std::uint8_t> out;
    fxme::net::ByteWriter w (out);
    w.u32 (slotId);
    w.u32 ((std::uint32_t) blocks.size());
    for (auto b : blocks)
        w.u32 (b);
    return out;
}

inline bool decodeSubscribe (const std::vector<std::uint8_t>& in, std::uint32_t& slotId, std::vector<std::uint32_t>& blocks)
{
    fxme::net::ByteReader r (in.data(), in.size());
    slotId = r.u32();
    const auto n = r.u32();
    if (! r.ok() || n > maxBlocks)
        return false;
    blocks.resize (n);
    for (auto& b : blocks)
        b = r.u32();
    return r.ok();
}

//==============================================================================
/** The server's side of one slot's mirror: what the client was sent last,
    and the next frame. Not thread-safe: one thread (the session's), which
    holds the engine's lock (jsfx_embed_lock) while collect() reads memory. */
class SyncSource
{
public:
    /** Starts again: the table of variables (names and where their values
        are), everything to send, nothing subscribed. */
    void reset (std::vector<std::string> names, std::vector<const double*> values)
    {
        varNames = std::move (names);
        varValues = std::move (values);
        varSent.assign (varValues.size(), 0);
        varKnown.assign (varValues.size(), false);
        blocks.clear();
        needsReset = true;
        frame = 0;
    }

    /** The client's display touched these blocks. Ignored past the limit. */
    void subscribe (const std::vector<std::uint32_t>& list)
    {
        for (auto b : list)
            if (b < maxBlocks && blocks.size() < maxSubscribedBlocks && blocks.find (b) == blocks.end())
                blocks[b].assign (blockItems, 0.0);   // the client's copy starts at zero
    }

    std::size_t numSubscribed() const noexcept   { return blocks.size(); }

    /** The next frame, at most `budget` bytes of memory (variables and the
        table always go). `memory (block)`: the live block, or nullptr when
        the script has not allocated it (zeros). False: nothing to send. */
    bool collect (std::uint32_t slotId, const std::function<const double* (std::uint32_t)>& memory,
                  std::size_t budget, SyncFrame& out)
    {
        out = {};
        out.slotId = slotId;
        out.frame = frame;
        out.reset = needsReset;
        if (needsReset)
            out.names = varNames;

        for (std::uint32_t i = 0; i < varValues.size(); ++i)
        {
            const auto b = detail::bits (*varValues[i]);
            if (needsReset || ! varKnown[i] || b != varSent[i])
            {
                out.vars.push_back ({ i, detail::fromBits (b) });
                varSent[i] = b;
                varKnown[i] = true;
            }
        }

        // Memory: changed chunks, oldest block first after the last one served,
        // so that one busy block does not keep the others waiting.
        static const std::vector<double> zeros (chunkItems, 0.0);
        std::size_t used = 0;
        if (! blocks.empty())
        {
            auto start = blocks.upper_bound (lastBlock);
            for (std::size_t visited = 0; visited < blocks.size() && used < budget; ++visited)
            {
                if (start == blocks.end())
                    start = blocks.begin();
                auto& [block, copy] = *start;
                const double* live = memory (block);
                SyncFrame::Range* open = nullptr;
                for (std::uint32_t c = 0; c < blockItems; c += chunkItems)
                {
                    const double* now = live != nullptr ? live + c : zeros.data();
                    double* sent = copy.data() + c;
                    if (std::memcmp (now, sent, chunkItems * sizeof (double)) == 0)
                    {
                        open = nullptr;
                        continue;
                    }
                    if (used + chunkItems * 8 + 8 > budget)
                        break;   // the rest of this block waits
                    std::memcpy (sent, now, chunkItems * sizeof (double));
                    if (open == nullptr)
                    {
                        out.ranges.push_back ({ block * blockItems + c, {} });
                        open = &out.ranges.back();
                        used += 8;
                    }
                    open->values.insert (open->values.end(), now, now + chunkItems);
                    used += chunkItems * 8;
                }
                lastBlock = block;
                ++start;
            }
        }

        const bool any = needsReset || ! out.vars.empty() || ! out.ranges.empty();
        needsReset = false;
        if (any)
            ++frame;
        return any;
    }

private:
    std::vector<std::string> varNames;
    std::vector<const double*> varValues;
    std::vector<std::uint64_t> varSent;
    std::vector<bool> varKnown;
    std::map<std::uint32_t, std::vector<double>> blocks;   ///< the client's copy of each subscribed block
    std::uint32_t lastBlock = 0;
    std::uint32_t frame = 0;
    bool needsReset = true;
};

} // namespace fxmenet::jsfx
