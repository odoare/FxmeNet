/*
  ------------------------------------------------------------------------------
    RoutingEngine.h

    Processing a chain through a routing graph (doc/routing.md, section 5),
    JUCE-free. The server's Chain builds a RoutingPlan off the audio thread
    whenever the chain, the routing, a bus layout or a latency changes, and
    swaps it in with its snapshot; the audio thread runs the plan for each
    block. The plugins themselves are reached through a callback, so the
    engine can be tested with fake processors.

    A plan holds one buffer per box: INPUTS (a copy of FxmeNet's inputs),
    one per slot (as wide as the plugin's enabled channels, the buffer its
    processBlock gets), OUTPUTS (the result). For each block:

      1. INPUTS: the session's input channels copied in;
      2. each slot, top to bottom: its buffer cleared, each input channel
         gathered (the sum of its sources, each read directly or through a
         delay line), the plugin called on the buffer (unless muted: then
         its outputs stay silent);
      3. OUTPUTS gathered the same way.

    After a box's buffer is final, the delay lines of its output pins that
    need one are written. A delay line belongs to a source pin and serves
    all its delayed connections (one write, one read per connection, each at
    its own delay), sized for the largest with headroom. A new plan takes
    over the delay lines of the plan it replaces (`previous`) when they are
    large enough, so a rebuild (a plugin added elsewhere, a latency change)
    does not empty the delayed paths.

    Pins are numbered as in Routing.h (across a box's buses, each at its
    full width). In a slot's buffer only the enabled buses have channels:
    the plan maps each pin to its buffer channel, and a pin of a disabled
    bus (or past the width a bus was given) is ignored.

    Not thread-safe: built on one thread, then used by one audio thread. No
    allocation in process().

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeNet/chain/Routing.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace fxmenet::chain
{

/** A slot as the engine sees it. */
struct SlotLayout
{
    Box box;                                ///< its buses at full width (pin numbering)
    std::vector<int> inputWidths;           ///< channels each input bus has now (0: disabled), box order
    std::vector<int> outputWidths;          ///< the same for the output buses
    int latency = 0;
    bool muted = false;                     ///< not processed (its layout is being changed): silent outputs

    /** All buses enabled at full width. */
    static SlotLayout full (const Box& b, int latency = 0)
    {
        SlotLayout s;
        s.box = b;
        for (auto& bus : b.inputs)
            s.inputWidths.push_back (bus.channels);
        for (auto& bus : b.outputs)
            s.outputWidths.push_back (bus.channels);
        s.latency = latency;
        return s;
    }
};

/** The connections that fit the chain `boxes` (top to bottom): boxes that
    exist, pins in range, going downward. ROUTING is kept whole by the
    server and filtered through this each time the chain changes. */
inline std::vector<Connection> fittingConnections (const std::vector<Box>& boxes,
                                                   const std::vector<Connection>& connections)
{
    Routing r;
    r.setBoxes (boxes);
    std::vector<Connection> out;
    for (auto& c : connections)
        if (r.isValid (c.from, c.to) && std::find (out.begin(), out.end(), c) == out.end())
            out.push_back (c);
    return out;
}

/** Which buses of `box` should be enabled with these connections: main
    buses always, the others when one of their channels is connected. */
inline void wantedBuses (const Box& box, const std::vector<Connection>& connections,
                         std::vector<bool>& inputs, std::vector<bool>& outputs)
{
    for (const bool input : { true, false })
    {
        const auto& buses = input ? box.inputs : box.outputs;
        auto& wanted = input ? inputs : outputs;
        wanted.assign (buses.size(), false);
        for (std::size_t b = 0; b < buses.size(); ++b)
        {
            if (buses[b].main)
            {
                wanted[b] = true;
                continue;
            }
            const int first = box.firstChannel (input, (int) b);
            const int last = first + std::max (0, buses[b].channels);
            for (auto& c : connections)
            {
                const auto& p = input ? c.to : c.from;
                if (p.box == box.id && p.channel >= first && p.channel < last)
                {
                    wanted[b] = true;
                    break;
                }
            }
        }
    }
}

//==============================================================================
/** A ring buffer of one source pin's past samples. */
class DelayLine
{
public:
    explicit DelayLine (int capacity)
    {
        int size = 1;
        while (size < capacity)
            size <<= 1;
        ring.assign ((std::size_t) size, 0.0f);
        mask = (std::uint32_t) size - 1;
    }

    int getCapacity() const noexcept   { return (int) ring.size(); }

    /** This block's samples; read() then reads relative to them. */
    void write (const float* src, int n) noexcept
    {
        start = pos;
        for (int i = 0; i < n; ++i)
            ring[(pos + (std::uint32_t) i) & mask] = src[i];
        pos += (std::uint32_t) n;
    }

    /** Adds this block's samples delayed by `delay` (n + delay <= capacity). */
    void addTo (float* dest, int n, int delay) const noexcept
    {
        const std::uint32_t from = start - (std::uint32_t) delay;
        for (int i = 0; i < n; ++i)
            dest[i] += ring[(from + (std::uint32_t) i) & mask];
    }

    void clear() noexcept   { std::fill (ring.begin(), ring.end(), 0.0f); }

private:
    std::vector<float> ring;
    std::uint32_t mask = 0, pos = 0, start = 0;
};

//==============================================================================
class RoutingPlan
{
public:
    /** Builds the plan. Not on the audio thread. `connections` are filtered
        (fittingConnections) and mapped to the slots' buffers; `previous`,
        if any, lends its delay lines. */
    RoutingPlan (int numInputs, int numOutputs, const std::vector<SlotLayout>& slots,
                 const std::vector<Connection>& connections, int blockSize,
                 const RoutingPlan* previous = nullptr)
        : n (std::max (1, blockSize))
    {
        // The boxes, top to bottom, and each pin's buffer channel.
        std::vector<Box> boxes { Box::makeInputs (std::max (0, numInputs)) };
        for (auto& s : slots)
            boxes.push_back (s.box);
        boxes.push_back (Box::makeOutputs (std::max (0, numOutputs)));

        nodes.resize (boxes.size());
        for (std::size_t i = 0; i < boxes.size(); ++i)
        {
            auto& node = nodes[i];
            node.id = boxes[i].id;
            if (i == 0)
            {
                node.outputMap = identity (numInputs);
                node.width = std::max (0, numInputs);
            }
            else if (i + 1 == boxes.size())
            {
                node.inputMap = identity (numOutputs);
                node.width = std::max (0, numOutputs);
            }
            else
            {
                const auto& s = slots[i - 1];
                int ins = 0, outs = 0;
                node.inputMap = pinMap (s.box.inputs, s.inputWidths, ins);
                node.outputMap = pinMap (s.box.outputs, s.outputWidths, outs);
                node.width = std::max ({ 1, ins, outs });
                node.muted = s.muted;
                node.slot = (int) i - 1;
            }
            node.data.assign ((std::size_t) (node.width * n), 0.0f);
            for (int c = 0; c < node.width; ++c)
                node.channels.push_back (node.data.data() + c * n);
            node.inputs.resize ((std::size_t) node.width);
        }

        // The connections that reach a channel at both ends.
        std::vector<Connection> used;
        for (auto& c : fittingConnections (boxes, connections))
        {
            const int f = indexOf (c.from.box), t = indexOf (c.to.box);
            if (bufferChannel (nodes[(std::size_t) f].outputMap, c.from.channel) >= 0
                && bufferChannel (nodes[(std::size_t) t].inputMap, c.to.channel) >= 0)
                used.push_back (c);
        }

        // Timing, from the slots' latencies.
        std::vector<std::uint32_t> order;
        std::vector<int> latencies;
        for (std::size_t i = 0; i < boxes.size(); ++i)
        {
            order.push_back (boxes[i].id);
            latencies.push_back (i == 0 || i + 1 == boxes.size() ? 0 : std::max (0, slots[i - 1].latency));
        }
        const auto timing = computeTiming (order, latencies, used);
        latency = timing.latency;
        slotLatencies.assign (latencies.begin() + 1, latencies.end() - 1);

        // Each source pin's largest delay: its delay line.
        std::map<Pin, int> longest;
        for (std::size_t k = 0; k < used.size(); ++k)
            if (timing.delay[k] > 0)
                longest[used[k].from] = std::max (longest[used[k].from], timing.delay[k]);

        for (auto& [pin, delay] : longest)
        {
            std::shared_ptr<DelayLine> line;
            if (previous != nullptr)
            {
                const auto it = previous->lines.find (pin);
                if (it != previous->lines.end() && it->second->getCapacity() >= n + delay)
                    line = it->second;
            }
            if (line == nullptr)
                line = std::make_shared<DelayLine> (n + 2 * delay);   // room for the delay to grow
            lines[pin] = line;
            const int f = indexOf (pin.box);
            nodes[(std::size_t) f].writes.push_back ({ line.get(), bufferChannel (nodes[(std::size_t) f].outputMap, pin.channel) });
        }

        // Each input channel's sources.
        for (std::size_t k = 0; k < used.size(); ++k)
        {
            const auto& c = used[k];
            const int f = indexOf (c.from.box), t = indexOf (c.to.box);
            Tap tap;
            tap.delay = timing.delay[k];
            if (tap.delay > 0)
                tap.line = lines[c.from].get();
            else
                tap.source = nodes[(std::size_t) f].channels[(std::size_t) bufferChannel (nodes[(std::size_t) f].outputMap, c.from.channel)];
            auto& to = nodes[(std::size_t) t];
            to.inputs[(std::size_t) bufferChannel (to.inputMap, c.to.channel)].push_back (tap);
        }
    }

    // The taps point into the nodes' buffers.
    RoutingPlan (const RoutingPlan&) = delete;
    RoutingPlan& operator= (const RoutingPlan&) = delete;

    int getBlockSize() const noexcept           { return n; }

    /** The graph's latency (the path reaching OUTPUTS last). */
    int getLatency() const noexcept             { return latency; }

    /** The latency a slot had when the plan was built (its index in the
        slots given). */
    int getSlotLatency (int slot) const noexcept
    {
        return slot >= 0 && slot < (int) slotLatencies.size() ? slotLatencies[(std::size_t) slot] : 0;
    }

    /** A slot's buffer width (what its processBlock gets). */
    int getSlotWidth (int slot) const noexcept
    {
        return slot >= 0 && slot + 2 < (int) nodes.size() ? nodes[(std::size_t) slot + 1].width : 0;
    }

    //==============================================================================
    /** One block of up to getBlockSize() samples. `inputs`: the session's
        `numInputs` channels. processSlot (int slot, float* const* channels,
        int numChannels, int numSamples) processes a slot in place. Audio
        thread. */
    template <typename ProcessSlot>
    void process (const float* const* inputs, int numInputs, int numSamples, ProcessSlot&& processSlot)
    {
        const int count = std::min (std::max (0, numSamples), n);

        auto& in = nodes.front();
        for (int c = 0; c < in.width; ++c)
        {
            float* dest = in.channels[(std::size_t) c];
            if (c < numInputs && inputs[c] != nullptr)
                std::copy (inputs[c], inputs[c] + count, dest);
            else
                std::fill (dest, dest + count, 0.0f);
        }
        writeLines (in, count);

        for (std::size_t i = 1; i < nodes.size(); ++i)
        {
            auto& node = nodes[i];
            gather (node, count);
            if (i + 1 < nodes.size())
            {
                if (! node.muted)
                    processSlot (node.slot, node.channels.data(), node.width, count);
                else
                    for (auto* ch : node.channels)
                        std::fill (ch, ch + count, 0.0f);
                writeLines (node, count);
            }
        }
    }

    /** OUTPUTS' channel c after process(), or null past the outputs. */
    const float* getOutput (int c) const noexcept
    {
        const auto& out = nodes.back();
        return c >= 0 && c < out.width ? out.channels[(std::size_t) c] : nullptr;
    }

    int getNumOutputs() const noexcept      { return nodes.back().width; }

    /** OUTPUTS' channels that something feeds (bit c: channel c). */
    std::uint64_t getOutputsFed() const noexcept
    {
        std::uint64_t mask = 0;
        const auto& out = nodes.back();
        for (int c = 0; c < out.width && c < 64; ++c)
            if (! out.inputs[(std::size_t) c].empty())
                mask |= std::uint64_t (1) << c;
        return mask;
    }

    /** Empties the delay lines (the stream was reset). Audio thread. */
    void clearDelays() noexcept
    {
        for (auto& [pin, line] : lines)
            line->clear();
    }

private:
    struct Tap
    {
        const float* source = nullptr;   ///< read directly (no delay)
        const DelayLine* line = nullptr; ///< or through a delay line
        int delay = 0;
    };

    struct Write
    {
        DelayLine* line = nullptr;
        int channel = 0;
    };

    struct Node
    {
        std::uint32_t id = 0;
        int slot = -1, width = 0;
        bool muted = false;
        std::vector<int> inputMap, outputMap;   ///< pin -> buffer channel, -1 when it has none
        std::vector<float> data;
        std::vector<float*> channels;
        std::vector<std::vector<Tap>> inputs;   ///< per buffer channel
        std::vector<Write> writes;
    };

    static std::vector<int> identity (int count)
    {
        std::vector<int> m;
        for (int c = 0; c < count; ++c)
            m.push_back (c);
        return m;
    }

    /** Pin -> buffer channel for buses at `widths` (missing: 0). */
    static std::vector<int> pinMap (const std::vector<Bus>& buses, const std::vector<int>& widths, int& total)
    {
        std::vector<int> m;
        total = 0;
        for (std::size_t b = 0; b < buses.size(); ++b)
        {
            const int width = b < widths.size() ? std::max (0, widths[b]) : 0;
            for (int k = 0; k < std::max (0, buses[b].channels); ++k)
                m.push_back (k < width ? total + k : -1);
            total += width;
        }
        return m;
    }

    static int bufferChannel (const std::vector<int>& map, int pin) noexcept
    {
        return pin >= 0 && pin < (int) map.size() ? map[(std::size_t) pin] : -1;
    }

    int indexOf (std::uint32_t id) const noexcept
    {
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].id == id)
                return (int) i;
        return -1;
    }

    static void gather (Node& node, int count) noexcept
    {
        for (int c = 0; c < node.width; ++c)
        {
            float* dest = node.channels[(std::size_t) c];
            std::fill (dest, dest + count, 0.0f);
            for (auto& tap : node.inputs[(std::size_t) c])
            {
                if (tap.line != nullptr)
                    tap.line->addTo (dest, count, tap.delay);
                else
                    for (int i = 0; i < count; ++i)
                        dest[i] += tap.source[i];
            }
        }
    }

    static void writeLines (Node& node, int count) noexcept
    {
        for (auto& w : node.writes)
            w.line->write (node.channels[(std::size_t) w.channel], count);
    }

    int n = 128;
    int latency = 0;
    std::vector<int> slotLatencies;
    std::vector<Node> nodes;
    std::map<Pin, std::shared_ptr<DelayLine>> lines;
};

} // namespace fxmenet::chain
