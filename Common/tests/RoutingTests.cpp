/*
  ------------------------------------------------------------------------------
    RoutingTests.cpp

    The routing model (Common/FxmeNet/chain/Routing.h, doc/routing.md),
    without JUCE and without audio:
      - the default rule on serial chains: same widths, a narrower plugin
        letting channels through, mono to stereo, stereo to mono, aux buses
        left unconnected (and disabled);
      - connect and disconnect: validation (downward only, existing pins),
        summing, a pin made manual keeps its source;
      - chain edits: insert, remove (bridged), move (upward sources
        dropped), channels changed, a pin emptied by an edit back to
        automatic, reset;
      - the inputs and outputs used, buses enabled;
      - timing: arrival, per-connection delays and latency on serial and
        parallel graphs;
      - saving and restoring the manual pins;
      - random chains and edits: every connection valid, delays never
        negative, everything meeting at a box aligned.

    And the engine (Common/FxmeNet/chain/RoutingEngine.h), with fake
    plugins that only delay, apply a gain and spread their inputs over
    their outputs:
      - the default routing gives exactly what the old serial chain gave
        (one shared buffer, each plugin in place), on random chains;
      - parallel paths aligned: dry plus wet, a channel going around a
        plugin with latency;
      - a plan rebuilt at every block (delay lines taken over) gives the
        same output as one plan; a latency change settles on the new delay;
      - disabled buses ignored, an enabled sidechain fed, a muted slot
        silent, unconnected outputs silent, delays cleared on reset;
      - the connections that fit a chain, the buses wanted.

    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include <FxmeNet/chain/Routing.h>
#include <FxmeNet/chain/RoutingEngine.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <random>

namespace
{
    using namespace fxmenet::chain;

    int failures = 0;
    int checks   = 0;

    void check (bool condition, const char* what, int line)
    {
        ++checks;
        if (! condition)
        {
            ++failures;
            std::printf ("  FAIL (line %d): %s\n", line, what);
        }
    }

    #define CHECK(cond) check ((cond), #cond, __LINE__)

    /** A fake plugin: output k is gain x input (k mod ins), delayed by
        `latency`; without inputs, gain x a ramp of its own. Processes in
        place, leaving the channels past its outputs alone (as a plugin
        leaves the buffer's extra channels). */
    struct Fake
    {
        int ins = 2, outs = 2, latency = 0;
        float gain = 1.0f;
        std::vector<std::vector<float>> history;   ///< per input, the last `latency` samples
        std::vector<float> temp;
        long counter = 0;

        Fake (int i, int o, int l, float g) : ins (i), outs (o), latency (l), gain (g) {}

        int width() const   { return std::max ({ 1, ins, outs }); }

        void setLatency (int l)
        {
            latency = l;
            history.clear();
        }

        void process (float* const* ch, int numChannels, int n)
        {
            if (history.size() != (std::size_t) ins)
                history.assign ((std::size_t) ins, std::vector<float> ((std::size_t) latency, 0.0f));
            std::vector<std::vector<float>> delayed ((std::size_t) ins);
            for (int c = 0; c < ins && c < numChannels; ++c)
            {
                auto& h = history[(std::size_t) c];
                h.insert (h.end(), ch[c], ch[c] + n);
                delayed[(std::size_t) c].assign (h.begin(), h.begin() + n);
                h.erase (h.begin(), h.begin() + n);
            }
            for (int k = 0; k < outs && k < numChannels; ++k)
                for (int i = 0; i < n; ++i)
                    ch[k][i] = ins > 0 ? gain * delayed[(std::size_t) (k % ins)][(std::size_t) i]
                                       : gain * (float) ((counter + i) % 97) / 97.0f;
            counter += n;
        }
    };

    Box fakeBox (std::uint32_t id, const Fake& f)
    {
        Box b;
        b.id = id;
        if (f.ins > 0)
            b.inputs.push_back ({ "Main", f.ins, true });
        if (f.outs > 0)
            b.outputs.push_back ({ "Main", f.outs, true });
        return b;
    }

    /** A random test signal per channel. */
    std::vector<std::vector<float>> signal (int channels, int length, std::mt19937& rng)
    {
        std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        std::vector<std::vector<float>> out ((std::size_t) channels, std::vector<float> ((std::size_t) length));
        for (auto& ch : out)
            for (auto& x : ch)
                x = d (rng);
        return out;
    }

    /** Runs a plan over a whole signal, block by block; returns OUTPUTS. */
    template <typename MakePlan>
    std::vector<std::vector<float>> runPlan (const std::vector<std::vector<float>>& in, int numOutputs, int n,
                                             std::vector<Fake>& fakes, MakePlan&& makePlan)
    {
        const int length = in.empty() ? 0 : (int) in[0].size();
        std::vector<std::vector<float>> out ((std::size_t) numOutputs, std::vector<float> ((std::size_t) length, 0.0f));
        std::unique_ptr<RoutingPlan> plan;
        for (int start = 0, block = 0; start < length; start += n, ++block)
        {
            plan = makePlan (block, plan.get());
            std::vector<const float*> inputs;
            for (auto& ch : in)
                inputs.push_back (ch.data() + start);
            plan->process (inputs.data(), (int) inputs.size(), n, [&fakes] (int slot, float* const* ch, int w, int count)
            {
                fakes[(std::size_t) slot].process (ch, w, count);
            });
            for (int c = 0; c < numOutputs; ++c)
                if (auto* o = plan->getOutput (c))
                    std::copy (o, o + n, out[(std::size_t) c].begin() + start);
        }
        return out;
    }

    /** A plugin box: main buses of `in` and `out` channels, plus aux inputs
        (each `auxWidth` wide) and aux outputs. */
    Box plugin (std::uint32_t id, int in, int out, int auxIns = 0, int auxWidth = 2, int auxOuts = 0)
    {
        Box b;
        b.id = id;
        if (in > 0)
            b.inputs.push_back ({ "Main", in, true });
        for (int i = 0; i < auxIns; ++i)
            b.inputs.push_back ({ "Sidechain", auxWidth, false });
        if (out > 0)
            b.outputs.push_back ({ "Main", out, true });
        for (int i = 0; i < auxOuts; ++i)
            b.outputs.push_back ({ "Aux out", 2, false });
        return b;
    }

    std::vector<Box> chain (int ins, std::vector<Box> plugins, int outs)
    {
        std::vector<Box> boxes { Box::makeInputs (ins) };
        for (auto& p : plugins)
            boxes.push_back (p);
        boxes.push_back (Box::makeOutputs (outs));
        return boxes;
    }

    bool fedBy (const Routing& r, Pin input, std::vector<Pin> expected)
    {
        return r.sourcesOf (input) == expected;
    }

    constexpr auto I = inputsBox;
    constexpr auto O = outputsBox;

    /** Invariants of any routing: valid connections, non-negative delays,
        sources aligned at each box. */
    void checkInvariants (const Routing& r, const std::vector<int>& latencies)
    {
        const auto connections = r.getConnections();
        bool valid = true;
        for (auto& c : connections)
            valid = valid && r.isValid (c.from, c.to);
        CHECK (valid);

        const auto t = r.getTiming (latencies);
        const auto& boxes = r.getBoxes();
        auto position = [&] (std::uint32_t id)
        {
            for (std::size_t i = 0; i < boxes.size(); ++i)
                if (boxes[i].id == id)
                    return i;
            return boxes.size();
        };
        bool aligned = true, nonNegative = true;
        for (std::size_t k = 0; k < connections.size(); ++k)
        {
            const auto src = position (connections[k].from.box);
            const auto dst = position (connections[k].to.box);
            nonNegative = nonNegative && t.delay[k] >= 0;
            aligned = aligned && t.arrival[src] + latencies[src] + t.delay[k] == t.arrival[dst];
        }
        CHECK (nonNegative);
        CHECK (aligned);
    }
}

int main()
{
    //--------------------------------------------------------------------------
    std::printf ("Default routing, serial chains\n");
    {
        Routing r;
        r.setBoxes (chain (2, { plugin (1, 2, 2), plugin (2, 2, 2) }, 2));
        const std::vector<Connection> expected {
            { { I, 0 }, { 1, 0 } }, { { I, 1 }, { 1, 1 } },
            { { 1, 0 }, { 2, 0 } }, { { 1, 1 }, { 2, 1 } },
            { { 2, 0 }, { O, 0 } }, { { 2, 1 }, { O, 1 } } };
        CHECK (r.getConnections() == expected);
        CHECK (r.getInputsUsed() == 3 && r.getOutputsUsed() == 3);
        CHECK (! r.isManual ({ 1, 0 }));

        const auto t = r.getTiming ({ 0, 64, 100, 0 });
        CHECK (t.latency == 164);
        CHECK (t.arrival == std::vector<int> ({ 0, 0, 64, 164 }));
        CHECK (t.delay == std::vector<int> (6, 0));

        // An empty chain: inputs straight to outputs.
        Routing e;
        e.setBoxes (chain (2, {}, 2));
        CHECK (fedBy (e, { O, 0 }, { { I, 0 } }) && fedBy (e, { O, 1 }, { { I, 1 } }));
        CHECK (e.getTiming ({ 0, 0 }).latency == 0);

        // More inputs than outputs, and the reverse.
        e.setBoxes (chain (4, {}, 2));
        CHECK (e.getInputsUsed() == 3 && e.getOutputsUsed() == 3);
        e.setBoxes (chain (1, {}, 2));
        CHECK (e.getInputsUsed() == 1 && e.getOutputsUsed() == 1);
        CHECK (e.sourcesOf ({ O, 1 }).empty());
    }

    std::printf ("Default routing: narrower, mono to stereo, stereo to mono\n");
    {
        // A two-channel plugin between four-channel ones lets 3 and 4 by.
        Routing r;
        r.setBoxes (chain (4, { plugin (1, 4, 4), plugin (2, 2, 2), plugin (3, 4, 4) }, 4));
        CHECK (fedBy (r, { 3, 0 }, { { 2, 0 } }) && fedBy (r, { 3, 1 }, { { 2, 1 } }));
        CHECK (fedBy (r, { 3, 2 }, { { 1, 2 } }) && fedBy (r, { 3, 3 }, { { 1, 3 } }));
        CHECK (fedBy (r, { O, 2 }, { { 3, 2 } }));

        // Then the parallel paths are aligned: channels 3 and 4 wait for
        // the narrow plugin's latency.
        const auto t = r.getTiming ({ 0, 10, 100, 0, 0 });
        CHECK (t.latency == 110 && t.arrival[3] == 110);
        const auto connections = r.getConnections();
        for (std::size_t k = 0; k < connections.size(); ++k)
            if (connections[k].to.box == 3)
                CHECK (t.delay[k] == (connections[k].from.box == 1 ? 100 : 0));

        // Mono to stereo: its second output replaces channel 2, as the
        // shared buffer did; input 2 is then not sent.
        r.setBoxes (chain (2, { plugin (1, 1, 2), plugin (2, 2, 2) }, 2));
        CHECK (fedBy (r, { 1, 0 }, { { I, 0 } }));
        CHECK (fedBy (r, { 2, 1 }, { { 1, 1 } }));
        CHECK (r.getInputsUsed() == 1);

        // Stereo to mono: channel 2 goes around it.
        r.setBoxes (chain (2, { plugin (1, 2, 1), plugin (2, 2, 2) }, 2));
        CHECK (fedBy (r, { 2, 0 }, { { 1, 0 } }) && fedBy (r, { 2, 1 }, { { I, 1 } }));

        // A plugin without inputs (an instrument): fed by nothing, starts
        // at 0, and what is above it is cut off on its channels.
        r.setBoxes (chain (2, { plugin (1, 2, 2), plugin (2, 0, 2) }, 2));
        CHECK (fedBy (r, { O, 0 }, { { 2, 0 } }));
        CHECK (r.getOutputsUsed() == 3);
        CHECK (r.getTiming ({ 0, 50, 7, 0 }).latency == 7);
    }

    std::printf ("Aux buses\n");
    {
        Routing r;
        r.setBoxes (chain (4, { plugin (1, 2, 2, 1) }, 2));
        CHECK (r.findBox (1)->numInputs() == 4);
        CHECK (r.sourcesOf ({ 1, 2 }).empty() && r.sourcesOf ({ 1, 3 }).empty());
        CHECK (r.isBusEnabled (1, true, 0) && ! r.isBusEnabled (1, true, 1));
        CHECK (r.getInputsUsed() == 3);

        // The DAW's channels 3 and 4 into the sidechain.
        CHECK (r.connect ({ I, 2 }, { 1, 2 }) && r.connect ({ I, 3 }, { 1, 3 }));
        CHECK (r.isBusEnabled (1, true, 1));
        CHECK (r.getInputsUsed() == 15);
        CHECK (r.isManual ({ 1, 2 }) && ! r.isManual ({ 1, 0 }));

        // Aux outputs: unconnected by default, enabled when used.
        r.setBoxes (chain (2, { plugin (2, 2, 2, 0, 2, 1) }, 4));
        CHECK (r.findBox (2)->numOutputs() == 4 && r.findBox (2)->firstChannel (false, 1) == 2);
        CHECK (! r.isBusEnabled (2, false, 1) && r.sourcesOf ({ O, 2 }).empty());
        CHECK (r.connect ({ 2, 2 }, { O, 2 }));
        CHECK (r.isBusEnabled (2, false, 1) && r.getOutputsUsed() == 7);
        CHECK (! r.isBusEnabled (2, false, 2) && ! r.isBusEnabled (99, true, 0));
    }

    //--------------------------------------------------------------------------
    std::printf ("Connect and disconnect\n");
    {
        Routing r;
        r.setBoxes (chain (2, { plugin (1, 2, 2), plugin (2, 2, 2) }, 2));

        CHECK (! r.connect ({ 2, 0 }, { 1, 0 }));     // upward
        CHECK (! r.connect ({ 1, 0 }, { 1, 0 }));     // same box
        CHECK (! r.connect ({ 1, 2 }, { 2, 0 }));     // no such output
        CHECK (! r.connect ({ 1, 0 }, { 2, 5 }));     // no such input
        CHECK (! r.connect ({ 1, 0 }, { I, 0 }));     // INPUTS has no input
        CHECK (! r.connect ({ O, 0 }, { 2, 0 }));     // OUTPUTS has no output
        CHECK (! r.connect ({ 7, 0 }, { 2, 0 }));     // no such box
        CHECK (r.getManual().empty());

        // Parallel: the dry input added to the output (summed), around both.
        CHECK (r.connect ({ I, 0 }, { O, 0 }));
        CHECK (fedBy (r, { O, 0 }, { { 2, 0 }, { I, 0 } }));
        CHECK (r.connect ({ I, 0 }, { O, 0 }));       // already there
        CHECK (r.sourcesOf ({ O, 0 }).size() == 2);
        CHECK (r.isManual ({ O, 0 }) && ! r.isManual ({ O, 1 }));

        const auto t = r.getTiming ({ 0, 30, 20, 0 });
        CHECK (t.latency == 50);
        const auto connections = r.getConnections();
        for (std::size_t k = 0; k < connections.size(); ++k)
            if (connections[k].to == Pin { O, 0 })
                CHECK (t.delay[k] == (connections[k].from.box == I ? 50 : 0));

        // Fan-out: plugin 1's left also into plugin 2's right.
        CHECK (r.connect ({ 1, 0 }, { 2, 1 }));
        CHECK (fedBy (r, { 2, 1 }, { { 1, 1 }, { 1, 0 } }));

        CHECK (r.disconnect ({ 1, 1 }, { 2, 1 }));
        CHECK (fedBy (r, { 2, 1 }, { { 1, 0 } }));
        CHECK (! r.disconnect ({ 1, 1 }, { 2, 1 }));

        // Disconnecting an automatic pin leaves it manual and empty.
        CHECK (r.disconnect ({ I, 1 }, { 1, 1 }));
        CHECK (r.isManual ({ 1, 1 }) && r.sourcesOf ({ 1, 1 }).empty());
        CHECK (r.getInputsUsed() == 1);

        r.reset();
        CHECK (r.getManual().empty() && fedBy (r, { 1, 1 }, { { I, 1 } }));
    }

    //--------------------------------------------------------------------------
    std::printf ("Chain edits\n");
    {
        Routing r;
        r.setBoxes (chain (2, { plugin (1, 2, 2), plugin (2, 2, 2) }, 2));
        CHECK (r.connect ({ I, 0 }, { O, 0 }));      // dry, around everything
        CHECK (r.connect ({ 1, 1 }, { O, 1 }));      // plugin 1's right also out

        // Insert between 1 and 2: automatic pins follow, manual ones stay.
        r.setBoxes (chain (2, { plugin (1, 2, 2), plugin (3, 2, 2), plugin (2, 2, 2) }, 2));
        CHECK (fedBy (r, { 3, 0 }, { { 1, 0 } }) && fedBy (r, { 2, 0 }, { { 3, 0 } }));
        CHECK (fedBy (r, { O, 0 }, { { 2, 0 }, { I, 0 } }));
        CHECK (fedBy (r, { O, 1 }, { { 2, 1 }, { 1, 1 } }));

        // Remove 1: its manual use is bridged to what fed it.
        r.setBoxes (chain (2, { plugin (3, 2, 2), plugin (2, 2, 2) }, 2));
        CHECK (fedBy (r, { 3, 1 }, { { I, 1 } }));
        CHECK (fedBy (r, { O, 1 }, { { 2, 1 }, { I, 1 } }));

        // A manual pin fed through a removed plugin that was fed by another
        // removed plugin: bridged through both.
        r.reset();
        r.setBoxes (chain (2, { plugin (1, 2, 2), plugin (2, 2, 2), plugin (3, 2, 2) }, 2));
        CHECK (r.connect ({ 2, 0 }, { O, 1 }));
        r.setBoxes (chain (2, { plugin (3, 2, 2) }, 2));
        CHECK (fedBy (r, { O, 1 }, { { 3, 1 }, { I, 0 } }));

        // A removed plugin without the matching input: the source dropped.
        r.reset();
        r.setBoxes (chain (2, { plugin (1, 1, 2), plugin (2, 2, 2) }, 2));
        CHECK (r.connect ({ 1, 1 }, { O, 0 }));
        r.setBoxes (chain (2, { plugin (2, 2, 2) }, 2));
        CHECK (fedBy (r, { O, 0 }, { { 2, 0 } }));

        // Move 2 above 1: a manual source now below is dropped; the pin had
        // only that one, so it goes back to automatic.
        r.reset();
        r.setBoxes (chain (2, { plugin (1, 2, 2), plugin (2, 2, 2), plugin (3, 2, 2) }, 2));
        CHECK (r.connect ({ 1, 0 }, { 3, 1 }));
        CHECK (r.disconnect ({ 2, 1 }, { 3, 1 }));
        CHECK (fedBy (r, { 3, 1 }, { { 1, 0 } }));
        r.setBoxes (chain (2, { plugin (3, 2, 2), plugin (1, 2, 2), plugin (2, 2, 2) }, 2));
        CHECK (! r.isManual ({ 3, 1 }) && fedBy (r, { 3, 1 }, { { I, 1 } }));

        // A pin the user emptied stays empty across edits.
        r.reset();
        r.setBoxes (chain (2, { plugin (1, 2, 2) }, 2));
        CHECK (r.disconnect ({ 1, 1 }, { O, 1 }));
        r.setBoxes (chain (2, { plugin (1, 2, 2), plugin (2, 2, 2) }, 2));
        CHECK (r.isManual ({ O, 1 }) && r.sourcesOf ({ O, 1 }).empty());

        // Channels changed: a pin and a source that no longer exist go.
        r.reset();
        r.setBoxes (chain (4, { plugin (1, 4, 4), plugin (2, 4, 4) }, 4));
        CHECK (r.connect ({ 1, 3 }, { O, 0 }) && r.connect ({ I, 0 }, { 2, 3 }));
        r.setBoxes (chain (4, { plugin (1, 2, 2), plugin (2, 2, 2) }, 4));
        CHECK (fedBy (r, { O, 0 }, { { 2, 0 } }));
        CHECK (! r.isManual ({ 2, 3 }) && r.sourcesOf ({ 2, 3 }).empty());
        CHECK (fedBy (r, { O, 2 }, { { I, 2 } }));   // around the narrower plugins
    }

    //--------------------------------------------------------------------------
    std::printf ("Saving and restoring\n");
    {
        Routing r;
        r.setBoxes (chain (4, { plugin (1, 2, 2, 1), plugin (2, 2, 2) }, 2));
        CHECK (r.connect ({ I, 2 }, { 1, 2 }));
        CHECK (r.connect ({ I, 0 }, { O, 0 }));
        CHECK (r.disconnect ({ 2, 1 }, { O, 1 }));
        const auto saved = r.getManual();
        CHECK (saved.size() == 3);

        Routing back;
        back.setBoxes (r.getBoxes());
        back.setManual (saved);
        CHECK (back.getConnections() == r.getConnections());
        CHECK (back.isManual ({ O, 1 }) && back.sourcesOf ({ O, 1 }).empty());

        // Saved with a chain that changed meanwhile: what does not fit goes.
        Routing other;
        other.setBoxes (chain (2, { plugin (2, 2, 2) }, 2));
        other.setManual (saved);
        CHECK (! other.isManual ({ 1, 2 }));
        CHECK (fedBy (other, { O, 0 }, { { 2, 0 }, { I, 0 } }));
    }

    //--------------------------------------------------------------------------
    std::printf ("Random chains and edits\n");
    {
        std::mt19937 rng (1234);
        auto pick = [&rng] (int lo, int hi) { return std::uniform_int_distribution<int> (lo, hi) (rng); };

        for (int round = 0; round < 300; ++round)
        {
            Routing r;
            std::vector<Box> plugins;
            std::uint32_t nextId = 1;
            const int ins = pick (1, 8), outs = pick (1, 8);
            for (int i = pick (0, 6); i > 0; --i)
                plugins.push_back (plugin (nextId++, pick (0, 6), pick (0, 6), pick (0, 2), pick (1, 2), pick (0, 1)));
            r.setBoxes (chain (ins, plugins, outs));

            for (int step = 0; step < 40; ++step)
            {
                const auto& boxes = r.getBoxes();
                const int action = pick (0, 9);
                if (action < 5)
                {
                    const auto& a = boxes[(std::size_t) pick (0, (int) boxes.size() - 1)];
                    const auto& b = boxes[(std::size_t) pick (0, (int) boxes.size() - 1)];
                    const Pin from { a.id, pick (0, 6) }, to { b.id, pick (0, 6) };
                    const bool valid = r.isValid (from, to);
                    CHECK (r.connect (from, to) == valid);
                }
                else if (action < 7)
                {
                    const auto connections = r.getConnections();
                    if (! connections.empty())
                    {
                        const auto c = connections[(std::size_t) pick (0, (int) connections.size() - 1)];
                        CHECK (r.disconnect (c.from, c.to));
                    }
                }
                else if (action == 7 && ! plugins.empty())   // remove
                    plugins.erase (plugins.begin() + pick (0, (int) plugins.size() - 1));
                else if (action == 8)                        // insert
                    plugins.insert (plugins.begin() + pick (0, (int) plugins.size()),
                                    plugin (nextId++, pick (0, 6), pick (0, 6), pick (0, 1)));
                else if (plugins.size() > 1)                 // move
                {
                    const auto p = plugins[(std::size_t) pick (0, (int) plugins.size() - 1)];
                    plugins.erase (std::find_if (plugins.begin(), plugins.end(),
                                                 [&p] (const Box& b) { return b.id == p.id; }));
                    plugins.insert (plugins.begin() + pick (0, (int) plugins.size()), p);
                }
                if (action >= 7)
                    r.setBoxes (chain (ins, plugins, outs));

                std::vector<int> latencies;
                for (std::size_t i = 0; i < r.getBoxes().size(); ++i)
                    latencies.push_back (i == 0 || i + 1 == r.getBoxes().size() ? 0 : pick (0, 500));
                checkInvariants (r, latencies);
            }

            // Saved and restored, the same graph.
            Routing back;
            back.setBoxes (r.getBoxes());
            back.setManual (r.getManual());
            CHECK (back.getConnections() == r.getConnections());
        }
    }

    //--------------------------------------------------------------------------
    std::printf ("The client's model and the server's view\n");
    {
        // As ClientEngine keeps it: INPUTS and OUTPUTS 64 wide, every slot
        // whose buses are known; resolved for a server with 2 inputs and 2
        // outputs, and only the slots loaded there.
        auto modelBoxes = [] (std::vector<Box> plugins)   { return chain (64, std::move (plugins), 64); };
        auto serverBoxes = [] (std::vector<Box> plugins)  { return chain (2, std::move (plugins), 2); };

        Routing model;
        model.setBoxes (modelBoxes ({ plugin (1, 2, 2, 1), plugin (2, 2, 2) }));
        CHECK (model.connect ({ I, 2 }, { 1, 2 }));     // the DAW's channel 3 into the sidechain
        CHECK (model.connect ({ I, 0 }, { O, 0 }));     // dry path

        Routing server = model;
        server.setBoxes (serverBoxes ({ plugin (1, 2, 2, 1), plugin (2, 2, 2) }));
        CHECK (server.sourcesOf ({ 1, 2 }).empty());     // no channel 3 on this host: not sent
        CHECK (fedBy (server, { O, 0 }, { { 2, 0 }, { I, 0 } }));
        CHECK (server.getInputsUsed() == 3);
        CHECK (model.isManual ({ 1, 2 }) && fedBy (model, { 1, 2 }, { { I, 2 } }));   // kept in the model

        // Plugin 2 missing on the server: bridged there, kept in the model.
        server = model;
        server.setBoxes (serverBoxes ({ plugin (1, 2, 2, 1) }));
        CHECK (fedBy (server, { O, 1 }, { { 1, 1 } }));
        CHECK (model.findBox (2) != nullptr);

        // A project saved and loaded before the host is prepared: the
        // model's ends do not depend on it, so the pins survive.
        Routing loaded;
        loaded.setBoxes (modelBoxes ({ plugin (1, 2, 2, 1), plugin (2, 2, 2) }));
        loaded.setManual (model.getManual());
        CHECK (loaded.getConnections() == model.getConnections());
    }

    //--------------------------------------------------------------------------
    std::printf ("Engine: the default routing is the old serial chain\n");
    {
        std::mt19937 rng (99);
        auto pick = [&rng] (int lo, int hi) { return std::uniform_int_distribution<int> (lo, hi) (rng); };
        bool same = true;
        for (int round = 0; round < 200; ++round)
        {
            const int ins = pick (1, 6), outs = pick (1, 6), n = 32 * pick (1, 4);
            std::vector<Fake> spec;
            for (int i = pick (0, 5); i > 0; --i)
                spec.emplace_back (pick (0, 4), pick (0, 4), 0, 0.25f + 0.25f * (float) pick (0, 6));
            const auto in = signal (ins, n * 6, rng);

            // The old way: one 64-channel buffer, each plugin in place.
            auto oldFakes = spec;
            std::vector<std::vector<float>> expected ((std::size_t) outs, std::vector<float> (in[0].size(), 0.0f));
            for (int start = 0; start < (int) in[0].size(); start += n)
            {
                std::vector<std::vector<float>> work (64, std::vector<float> ((std::size_t) n, 0.0f));
                for (int c = 0; c < ins; ++c)
                    std::copy (in[(std::size_t) c].begin() + start, in[(std::size_t) c].begin() + start + n, work[(std::size_t) c].begin());
                for (auto& f : oldFakes)
                {
                    std::vector<float*> view;
                    for (int c = 0; c < f.width(); ++c)
                        view.push_back (work[(std::size_t) c].data());
                    f.process (view.data(), f.width(), n);
                }
                for (int c = 0; c < outs; ++c)
                    std::copy (work[(std::size_t) c].begin(), work[(std::size_t) c].end(), expected[(std::size_t) c].begin() + start);
            }

            // Through the routing's default graph.
            std::vector<Box> boxes { Box::makeInputs (ins) };
            std::vector<SlotLayout> layouts;
            for (std::size_t i = 0; i < spec.size(); ++i)
            {
                boxes.push_back (fakeBox ((std::uint32_t) i + 1, spec[i]));
                layouts.push_back (SlotLayout::full (boxes.back()));
            }
            boxes.push_back (Box::makeOutputs (outs));
            Routing r;
            r.setBoxes (boxes);
            auto newFakes = spec;
            const auto got = runPlan (in, outs, n, newFakes, [&] (int, RoutingPlan* previous)
            {
                return std::make_unique<RoutingPlan> (ins, outs, layouts, r.getConnections(), n, previous);
            });
            same = same && got == expected;
        }
        CHECK (same);
    }

    std::printf ("Engine: parallel paths aligned\n");
    {
        std::mt19937 rng (7);
        const int n = 64;
        const auto in = signal (4, n * 20, rng);

        // Dry plus wet: the dry path waits for the wet one.
        std::vector<Fake> fakes { Fake (1, 1, 100, 0.5f) };
        std::vector<SlotLayout> layouts { SlotLayout::full (fakeBox (1, fakes[0]), 100) };
        const std::vector<Connection> dryWet {
            { { I, 0 }, { 1, 0 } }, { { 1, 0 }, { O, 0 } }, { { I, 0 }, { O, 0 } } };
        int planLatency = -1;
        auto out = runPlan (in, 1, n, fakes, [&] (int, RoutingPlan* previous)
        {
            auto p = std::make_unique<RoutingPlan> (1, 1, layouts, dryWet, n, previous);
            planLatency = p->getLatency();
            return p;
        });
        CHECK (planLatency == 100);
        bool aligned = true;
        for (std::size_t i = 0; i < out[0].size(); ++i)
            aligned = aligned && std::abs (out[0][i] - (i >= 100 ? 1.5f * in[0][i - 100] : 0.0f)) < 1.0e-6f;
        CHECK (aligned);

        // The same graph rebuilt at every block (delay lines taken over):
        // the same output. Rebuilt without them: not the same.
        std::vector<Fake> third { Fake (1, 1, 100, 0.5f) };
        const auto rebuiltOut = runPlan (in, 1, n, third, [&] (int, RoutingPlan* previous)
        {
            return std::make_unique<RoutingPlan> (1, 1, layouts, dryWet, n, previous);
        });
        CHECK (rebuiltOut == out);
        std::vector<Fake> fresh { Fake (1, 1, 100, 0.5f) };
        const auto freshOut = runPlan (in, 1, n, fresh, [&] (int, RoutingPlan*)
        {
            return std::make_unique<RoutingPlan> (1, 1, layouts, dryWet, n, nullptr);
        });
        CHECK (freshOut != out);

        // A latency change: the plugin goes from 100 to 150 samples at block
        // 10; once both paths have caught up, the dry one follows.
        std::vector<Fake> changing { Fake (1, 1, 100, 0.5f) };
        const auto changed = runPlan (in, 1, n, changing, [&] (int block, RoutingPlan* previous)
        {
            if (block == 10)
                changing[0].setLatency (150);
            auto l = layouts;
            l[0].latency = changing[0].latency;
            return std::make_unique<RoutingPlan> (1, 1, l, dryWet, n, previous);
        });
        bool settled = true;
        for (std::size_t i = 10 * n + 150 + n; i < changed[0].size(); ++i)
            settled = settled && std::abs (changed[0][i] - 1.5f * in[0][i - 150]) < 1.0e-6f;
        CHECK (settled);

        // A channel around a narrower plugin waits for it: 4 channels into
        // A (latency 10), then X (2 channels, latency 100).
        std::vector<Fake> chainFakes { Fake (4, 4, 10, 0.5f), Fake (2, 2, 100, 2.0f) };
        std::vector<Box> boxes { Box::makeInputs (4), fakeBox (1, chainFakes[0]), fakeBox (2, chainFakes[1]), Box::makeOutputs (4) };
        Routing r;
        r.setBoxes (boxes);
        std::vector<SlotLayout> chainLayouts { SlotLayout::full (boxes[1], 10), SlotLayout::full (boxes[2], 100) };
        const auto around = runPlan (in, 4, n, chainFakes, [&] (int, RoutingPlan* previous)
        {
            auto p = std::make_unique<RoutingPlan> (4, 4, chainLayouts, r.getConnections(), n, previous);
            planLatency = p->getLatency();
            return p;
        });
        CHECK (planLatency == 110);
        bool allAligned = true;
        for (int c = 0; c < 4; ++c)
            for (std::size_t i = 0; i < around[0].size(); ++i)
            {
                const float expected = i >= 110 ? (c < 2 ? 1.0f : 0.5f) * in[(std::size_t) c][i - 110] : 0.0f;
                allAligned = allAligned && std::abs (around[(std::size_t) c][i] - expected) < 1.0e-6f;
            }
        CHECK (allAligned);
    }

    std::printf ("Engine: buses, mute, unconnected outputs, reset\n");
    {
        const int n = 16;
        Box side;
        side.id = 1;
        side.inputs = { { "Main", 2, true }, { "Sidechain", 2, false } };
        side.outputs = { { "Main", 2, true } };
        const std::vector<Connection> toSide { { { I, 0 }, { 1, 0 } }, { { I, 2 }, { 1, 2 } }, { { 1, 0 }, { O, 0 } } };

        std::vector<float> a (n, 1.0f), b (n, 2.0f), c3 (n, 3.0f);
        const float* inputs[] = { a.data(), b.data(), c3.data() };

        // Sidechain disabled: its pins are ignored, the buffer has 2 channels.
        SlotLayout disabled = SlotLayout::full (side);
        disabled.inputWidths = { 2, 0 };
        RoutingPlan p1 (3, 2, { disabled }, toSide, n);
        CHECK (p1.getSlotWidth (0) == 2);
        int seenWidth = 0;
        p1.process (inputs, 3, n, [&] (int, float* const*, int w, int) { seenWidth = w; });
        CHECK (seenWidth == 2);

        // Enabled: buffer channel 2 holds input 3.
        RoutingPlan p2 (3, 2, { SlotLayout::full (side) }, toSide, n);
        CHECK (p2.getSlotWidth (0) == 4);
        float sideSample = 0.0f, mainSample = 0.0f;
        p2.process (inputs, 3, n, [&] (int, float* const* ch, int, int) { mainSample = ch[0][5]; sideSample = ch[2][5]; });
        CHECK (mainSample == 1.0f && sideSample == 3.0f);
        CHECK (p2.getOutput (0)[3] == 1.0f);            // what the slot left on its output 1
        CHECK (p2.getOutput (1)[3] == 0.0f);            // output 2: nothing connected
        CHECK (p2.getOutput (2) == nullptr && p2.getNumOutputs() == 2);

        // Muted: not called, silent.
        SlotLayout muted = SlotLayout::full (side);
        muted.muted = true;
        RoutingPlan p3 (3, 2, { muted }, toSide, n);
        bool called = false;
        p3.process (inputs, 3, n, [&] (int, float* const*, int, int) { called = true; });
        CHECK (! called && p3.getOutput (0)[0] == 0.0f);

        // Reset: the delay lines are emptied.
        std::vector<Fake> fakes { Fake (1, 1, 20, 1.0f) };
        const std::vector<Connection> dryWet { { { I, 0 }, { 1, 0 } }, { { 1, 0 }, { O, 0 } }, { { I, 0 }, { O, 0 } } };
        RoutingPlan p4 (1, 1, { SlotLayout::full (fakeBox (1, fakes[0]), 20) }, dryWet, n);
        auto run = [&] { p4.process (inputs, 1, n, [&] (int s, float* const* ch, int w, int k) { fakes[(std::size_t) s].process (ch, w, k); }); };
        for (int i = 0; i < 4; ++i)
            run();
        CHECK (p4.getOutput (0)[0] == 2.0f);   // dry 1 + wet 1, both delayed
        p4.clearDelays();
        fakes[0].setLatency (20);
        run();
        CHECK (p4.getOutput (0)[0] == 0.0f);   // both paths empty again
    }

    std::printf ("Engine: fitting connections, wanted buses\n");
    {
        Box side;
        side.id = 1;
        side.inputs = { { "Main", 2, true }, { "Sidechain", 2, false } };
        side.outputs = { { "Main", 2, true }, { "Aux", 2, false } };
        const std::vector<Box> boxes { Box::makeInputs (4), side, Box::makeOutputs (2) };
        const std::vector<Connection> sent {
            { { I, 0 }, { 1, 0 } }, { { I, 0 }, { 1, 0 } },     // twice
            { { 1, 0 }, { 1, 1 } },                               // same box
            { { O, 0 }, { 1, 0 } },                               // upward
            { { 5, 0 }, { O, 0 } },                               // no such slot (yet)
            { { I, 9 }, { 1, 0 } },                               // no such pin
            { { I, 2 }, { 1, 3 } } };
        const auto fit = fittingConnections (boxes, sent);
        CHECK (fit.size() == 2 && fit[1] == (Connection { { I, 2 }, { 1, 3 } }));

        std::vector<bool> ins, outs;
        wantedBuses (side, fit, ins, outs);
        CHECK (ins == std::vector<bool> ({ true, true }) && outs == std::vector<bool> ({ true, false }));
        wantedBuses (side, {}, ins, outs);
        CHECK (ins == std::vector<bool> ({ true, false }));
    }

    std::printf ("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
