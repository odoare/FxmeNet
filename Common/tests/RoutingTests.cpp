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

    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include <FxmeNet/chain/Routing.h>

#include <cstdio>
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

    std::printf ("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
