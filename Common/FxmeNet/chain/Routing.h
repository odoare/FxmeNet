/*
  ------------------------------------------------------------------------------
    Routing.h

    The routing between the plugins of a chain (doc/routing.md), JUCE-free.

    The chain is a list of boxes, top to bottom: INPUTS (FxmeNet's input
    channels, as outputs), one box per slot, OUTPUTS (FxmeNet's output
    channels, as inputs). A box's channels are numbered across its buses,
    in bus order, each bus counted with its full channel count (enabled or
    not). A connection goes from an output pin to an input pin of a box
    below, so the graph has no loop. Several connections into one input are
    summed; one output may feed several inputs.

    Each input pin is either automatic or manual:

      - automatic: fed by the default rule. Input channel c of a box's main
        bus comes from the nearest box above whose main output bus has a
        channel c. This is what the old serial chain did with its shared
        buffer (a plugin narrower than the buffer let the other channels
        through; a mono-to-stereo plugin overwrote channel 2), so a chain
        left on the default routing sounds as before. Aux buses (sidechains,
        extra inputs) are never fed by the rule.
      - manual: fed by the sources the user chose, possibly none. A pin
        becomes manual when the user connects to it (its current sources
        are kept, the new one is added) or disconnects from it.

    Chain edits (setBoxes) keep the manual pins that still make sense, and
    automatic pins simply follow the default rule in the new chain:

      - a box removed: a manual source from it is replaced by what fed the
        same channel of its input (as if bypassed for good), or dropped
        when it had no such input;
      - a box moved: manual sources that are no longer above are dropped;
      - a box whose channels changed: pins and sources that no longer exist
        are dropped;
      - a manual pin left without any source by an edit (not by the user)
        goes back to automatic, so the signal keeps flowing.

    What the graph gives (also used by the server, which receives the
    resolved connections):

      - which FxmeNet inputs are used (sent) and outputs fed (received);
      - which buses are enabled: main buses always, the others when used
        (an enabled sidechain with nothing on it changes what many plugins
        do, see catalog/Buses.h);
      - computeTiming(): with each box's latency, when each box's input
        arrives (the latest of its sources), the delay to add on each
        connection so that everything meeting at a box is aligned, and the
        latency of the whole graph (when OUTPUTS' input arrives).

    Not thread-safe: one owner (the client's model, the server's session).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fxmenet::chain
{

/** The ids of the two ends; slots are numbered from 1. */
constexpr std::uint32_t inputsBox = 0;
constexpr std::uint32_t outputsBox = 0xffffffffu;

struct Bus
{
    std::string name;
    int channels = 0;
    bool main = false;
};

struct Box
{
    std::uint32_t id = 0;
    std::vector<Bus> inputs, outputs;

    int numInputs() const noexcept      { return total (inputs); }
    int numOutputs() const noexcept     { return total (outputs); }

    /** The first channel and the channel count of the main bus (0, 0 when
        there is none). */
    std::pair<int, int> mainInputs() const noexcept     { return mainOf (inputs); }
    std::pair<int, int> mainOutputs() const noexcept    { return mainOf (outputs); }

    /** The first channel of bus `index` (in the numbering across buses). */
    int firstChannel (bool input, int index) const noexcept
    {
        const auto& buses = input ? inputs : outputs;
        int first = 0;
        for (int i = 0; i < index && i < (int) buses.size(); ++i)
            first += std::max (0, buses[(std::size_t) i].channels);
        return first;
    }

    /** INPUTS: n channels, sent to the server. */
    static Box makeInputs (int n)
    {
        Box b;
        b.id = inputsBox;
        b.outputs.push_back ({ "Inputs", n, true });
        return b;
    }

    /** OUTPUTS: n channels, received from the server. */
    static Box makeOutputs (int n)
    {
        Box b;
        b.id = outputsBox;
        b.inputs.push_back ({ "Outputs", n, true });
        return b;
    }

private:
    static int total (const std::vector<Bus>& buses) noexcept
    {
        int n = 0;
        for (auto& b : buses)
            n += std::max (0, b.channels);
        return n;
    }

    static std::pair<int, int> mainOf (const std::vector<Bus>& buses) noexcept
    {
        int first = 0;
        for (auto& b : buses)
        {
            if (b.main)
                return { first, std::max (0, b.channels) };
            first += std::max (0, b.channels);
        }
        return { 0, 0 };
    }
};

struct Pin
{
    std::uint32_t box = 0;
    int channel = 0;

    bool operator== (const Pin& o) const noexcept   { return box == o.box && channel == o.channel; }
    bool operator!= (const Pin& o) const noexcept   { return ! (*this == o); }
    bool operator< (const Pin& o) const noexcept    { return box != o.box ? box < o.box : channel < o.channel; }
};

/** From an output pin to an input pin. */
struct Connection
{
    Pin from, to;

    bool operator== (const Connection& o) const noexcept   { return from == o.from && to == o.to; }
    bool operator!= (const Connection& o) const noexcept   { return ! (*this == o); }
};

/** A manual input pin and its sources (what is saved with a project). */
struct ManualInput
{
    Pin input;
    std::vector<Pin> sources;
};

//==============================================================================
/** Each box's input arrival time, each connection's delay (in the order
    given) and the graph's latency, in samples. `order` lists the boxes top
    to bottom, `latencies` their latencies (INPUTS and OUTPUTS: 0). A box
    fed by nothing starts at 0. Connections that are not downward between
    listed boxes get no delay. */
struct Timing
{
    std::vector<int> arrival;   ///< per box, in `order`
    std::vector<int> delay;     ///< per connection
    int latency = 0;            ///< OUTPUTS' arrival (0 when nothing reaches it)
};

inline Timing computeTiming (const std::vector<std::uint32_t>& order, const std::vector<int>& latencies,
                             const std::vector<Connection>& connections)
{
    std::map<std::uint32_t, std::size_t> position;
    for (std::size_t i = 0; i < order.size(); ++i)
        position[order[i]] = i;
    auto latencyAt = [&] (std::size_t i) { return i < latencies.size() ? std::max (0, latencies[i]) : 0; };

    // Index the connections by destination; skip what is not downward.
    std::vector<std::vector<std::size_t>> into (order.size());
    std::vector<std::pair<std::size_t, std::size_t>> ends (connections.size(), { 0, 0 });
    std::vector<bool> usable (connections.size(), false);
    for (std::size_t k = 0; k < connections.size(); ++k)
    {
        const auto f = position.find (connections[k].from.box);
        const auto t = position.find (connections[k].to.box);
        if (f == position.end() || t == position.end() || f->second >= t->second)
            continue;
        ends[k] = { f->second, t->second };
        usable[k] = true;
        into[t->second].push_back (k);
    }

    Timing out;
    out.arrival.assign (order.size(), 0);
    for (std::size_t i = 0; i < order.size(); ++i)
        for (auto k : into[i])
        {
            const auto src = ends[k].first;
            out.arrival[i] = std::max (out.arrival[i], out.arrival[src] + latencyAt (src));
        }

    out.delay.assign (connections.size(), 0);
    for (std::size_t k = 0; k < connections.size(); ++k)
        if (usable[k])
        {
            const auto [src, dst] = ends[k];
            out.delay[k] = out.arrival[dst] - (out.arrival[src] + latencyAt (src));
        }

    const auto o = position.find (outputsBox);
    if (o != position.end() && ! into[o->second].empty())
        out.latency = out.arrival[o->second];
    return out;
}

//==============================================================================
class Routing
{
public:
    /** The chain, top to bottom: INPUTS first, OUTPUTS last (Box::makeInputs,
        makeOutputs). Applies the edit rules (header comment) to the manual
        pins. */
    void setBoxes (std::vector<Box> newBoxes)
    {
        std::set<std::uint32_t> kept;
        for (auto& b : newBoxes)
            kept.insert (b.id);

        // Sources through removed boxes, resolved in the old chain.
        std::map<Pin, std::vector<Pin>> next;
        std::set<Pin> hadSources;
        for (auto& [pin, sources] : manual)
        {
            if (kept.count (pin.box) == 0)
                continue;
            std::vector<Pin> expanded;
            for (auto& s : sources)
                bridge (s, kept, expanded, 0);
            next[pin] = std::move (expanded);
            if (! sources.empty())
                hadSources.insert (pin);
        }

        boxes = std::move (newBoxes);
        manual.clear();
        for (auto& [pin, sources] : next)
            keepManual (pin, sources, hadSources.count (pin) != 0);
    }

    const std::vector<Box>& getBoxes() const noexcept   { return boxes; }

    /** The box with this id, or null. */
    const Box* findBox (std::uint32_t id) const noexcept
    {
        const int i = indexOf (id);
        return i < 0 ? nullptr : &boxes[(std::size_t) i];
    }

    //==============================================================================
    /** True when `from` is an output pin of a box above the box of input pin
        `to`, both existing. */
    bool isValid (Pin from, Pin to) const noexcept
    {
        const int f = indexOf (from.box), t = indexOf (to.box);
        return f >= 0 && t >= 0 && f < t
            && from.channel >= 0 && from.channel < boxes[(std::size_t) f].numOutputs()
            && to.channel >= 0 && to.channel < boxes[(std::size_t) t].numInputs();
    }

    /** Adds a connection (summed with what already feeds `to`). False when
        invalid; true, and nothing changed, when it is there already. */
    bool connect (Pin from, Pin to)
    {
        if (! isValid (from, to))
            return false;
        auto sources = sourcesOf (to);
        if (std::find (sources.begin(), sources.end(), from) == sources.end())
            sources.push_back (from);
        manual[to] = std::move (sources);
        return true;
    }

    /** Removes a connection; false when there is none. */
    bool disconnect (Pin from, Pin to)
    {
        auto sources = sourcesOf (to);
        const auto it = std::find (sources.begin(), sources.end(), from);
        if (it == sources.end())
            return false;
        sources.erase (it);
        manual[to] = std::move (sources);
        return true;
    }

    /** One input pin back to the default rule; false when it was not manual. */
    bool makeAutomatic (Pin input)      { return manual.erase (input) != 0; }

    /** One input pin fed by nothing (manual, empty); false when it does not
        exist. */
    bool clearInput (Pin input)
    {
        const int t = indexOf (input.box);
        if (t < 0 || input.channel < 0 || input.channel >= boxes[(std::size_t) t].numInputs())
            return false;
        manual[input] = {};
        return true;
    }

    /** Back to the default routing everywhere. */
    void reset()            { manual.clear(); }

    /** True when an input pin was set by the user. */
    bool isManual (Pin input) const     { return manual.count (input) != 0; }

    /** What feeds an input pin (empty for a pin that does not exist). */
    std::vector<Pin> sourcesOf (Pin input) const
    {
        const auto it = manual.find (input);
        if (it != manual.end())
            return it->second;
        std::vector<Pin> out;
        Pin source;
        if (automaticSource (input, source))
            out.push_back (source);
        return out;
    }

    /** Every connection, by destination: top to bottom, then by input
        channel, then by the order the sources were added. */
    std::vector<Connection> getConnections() const
    {
        std::vector<Connection> out;
        for (auto& b : boxes)
            for (int c = 0; c < b.numInputs(); ++c)
                for (auto& s : sourcesOf ({ b.id, c }))
                    out.push_back ({ s, { b.id, c } });
        return out;
    }

    //==============================================================================
    /** FxmeNet's inputs that feed something (bit c: channel c). */
    std::uint64_t getInputsUsed() const
    {
        std::uint64_t mask = 0;
        for (auto& c : getConnections())
            if (c.from.box == inputsBox && c.from.channel < 64)
                mask |= std::uint64_t (1) << c.from.channel;
        return mask;
    }

    /** FxmeNet's outputs that are fed (bit c: channel c). */
    std::uint64_t getOutputsUsed() const
    {
        std::uint64_t mask = 0;
        for (auto& c : getConnections())
            if (c.to.box == outputsBox && c.to.channel < 64)
                mask |= std::uint64_t (1) << c.to.channel;
        return mask;
    }

    /** Whether a box's bus is to be enabled: a main bus always, another one
        when one of its channels is fed (an input) or feeds something (an
        output). */
    bool isBusEnabled (std::uint32_t boxId, bool input, int busIndex) const
    {
        const auto* b = findBox (boxId);
        if (b == nullptr)
            return false;
        const auto& buses = input ? b->inputs : b->outputs;
        if (busIndex < 0 || busIndex >= (int) buses.size())
            return false;
        if (buses[(std::size_t) busIndex].main)
            return true;

        const int first = b->firstChannel (input, busIndex);
        const int last = first + std::max (0, buses[(std::size_t) busIndex].channels);
        for (auto& c : getConnections())
        {
            const auto& p = input ? c.to : c.from;
            if (p.box == boxId && p.channel >= first && p.channel < last)
                return true;
        }
        return false;
    }

    /** computeTiming() for this chain; `latencies` in box order. */
    Timing getTiming (const std::vector<int>& latencies) const
    {
        std::vector<std::uint32_t> order;
        for (auto& b : boxes)
            order.push_back (b.id);
        return computeTiming (order, latencies, getConnections());
    }

    //==============================================================================
    /** The manual pins, to save. */
    std::vector<ManualInput> getManual() const
    {
        std::vector<ManualInput> out;
        for (auto& [pin, sources] : manual)
            out.push_back ({ pin, sources });
        return out;
    }

    /** Restores saved manual pins, after setBoxes(); what does not fit the
        chain is dropped as by an edit. */
    void setManual (const std::vector<ManualInput>& saved)
    {
        manual.clear();
        for (auto& m : saved)
            keepManual (m.input, m.sources, ! m.sources.empty());
    }

private:
    int indexOf (std::uint32_t id) const noexcept
    {
        for (std::size_t i = 0; i < boxes.size(); ++i)
            if (boxes[i].id == id)
                return (int) i;
        return -1;
    }

    /** The default rule (header comment). */
    bool automaticSource (Pin input, Pin& out) const
    {
        const int t = indexOf (input.box);
        if (t < 0)
            return false;
        const auto [first, count] = boxes[(std::size_t) t].mainInputs();
        const int c = input.channel - first;
        if (c < 0 || c >= count)
            return false;
        for (int i = t - 1; i >= 0; --i)
        {
            const auto [outFirst, outCount] = boxes[(std::size_t) i].mainOutputs();
            if (c < outCount)
            {
                out = { boxes[(std::size_t) i].id, outFirst + c };
                return true;
            }
        }
        return false;
    }

    /** `source` into `out`, through the boxes not in `kept` (the old chain):
        a removed box's output channel c becomes what fed its input c. */
    void bridge (Pin source, const std::set<std::uint32_t>& kept, std::vector<Pin>& out, int depth) const
    {
        if (kept.count (source.box) != 0)
        {
            if (std::find (out.begin(), out.end(), source) == out.end())
                out.push_back (source);
            return;
        }
        const auto* b = findBox (source.box);
        if (b == nullptr || depth > (int) boxes.size() || source.channel >= b->numInputs())
            return;
        for (auto& s : sourcesOf ({ source.box, source.channel }))
            bridge (s, kept, out, depth + 1);
    }

    /** Stores a manual pin with its valid sources, in the current chain. A
        pin that had sources and lost them all goes back to automatic. */
    void keepManual (Pin pin, const std::vector<Pin>& sources, bool hadAny)
    {
        const int t = indexOf (pin.box);
        if (t < 0 || pin.channel < 0 || pin.channel >= boxes[(std::size_t) t].numInputs())
            return;
        std::vector<Pin> valid;
        for (auto& s : sources)
            if (isValid (s, pin) && std::find (valid.begin(), valid.end(), s) == valid.end())
                valid.push_back (s);
        if (valid.empty() && hadAny)
            return;
        manual[pin] = std::move (valid);
    }

    std::vector<Box> boxes;
    std::map<Pin, std::vector<Pin>> manual;
};

} // namespace fxmenet::chain
