/*
  ------------------------------------------------------------------------------
    ProtocolTests.cpp

    The audio-path wire format and fragmentation (Common/FxmeNet/protocol),
    without JUCE and without a network: headers, transport, MIDI and audio
    blocks round-trip; malformed input is refused; messages split into
    datagrams come back whole whatever the order, and incomplete ones are
    dropped as protocol.md section 3 says; one over the size limit is
    dropped and counted once as too large.

    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeNet/jsfx/JsfxBundle.h>
#include <map>
#include <FxmeNet/jsfx/JsfxSync.h>
#include <FxmeNet/control/ControlFrame.h>
#include <FxmeNet/protocol/Fragmentation.h>
#include <FxmeNet/protocol/Wire.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
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

    using namespace fxmenet::wire;
    using Bytes = std::vector<std::uint8_t>;
    using Datagrams = std::vector<Bytes>;

    /** numChannels planar channels, sample i of channel c = c * 1000 + i. */
    std::vector<std::vector<float>> makeAudio (int numChannels, int samples)
    {
        std::vector<std::vector<float>> a ((std::size_t) numChannels, std::vector<float> ((std::size_t) samples));
        for (int c = 0; c < numChannels; ++c)
            for (int i = 0; i < samples; ++i)
                a[(std::size_t) c][(std::size_t) i] = (float) (c * 1000 + i) * 0.001f;
        return a;
    }

    std::vector<const float*> pointers (const std::vector<std::vector<float>>& a)
    {
        std::vector<const float*> p;
        for (auto& ch : a)
            p.push_back (ch.data());
        return p;
    }

    Bytes encodeRequest (std::uint64_t block, int channels, int samples)
    {
        auto audio = makeAudio (channels, samples);
        auto ptrs = pointers (audio);
        BlockInfo info;
        info.block = block;
        info.samples = (std::uint16_t) samples;
        info.mask = maskOfFirst (channels);
        Bytes body;
        ByteWriter w (body);
        writeBlock (w, false, info, nullptr, 0, ptrs.data());
        return body;
    }

    Datagrams split (Fragmenter& f, std::uint32_t messageId, const Bytes& body)
    {
        DatagramHeader h;
        h.kind = Kind::audioRequest;
        h.sessionId = 7;
        h.token = 0x1122334455667788ull;
        h.messageId = messageId;
        Datagrams out;
        f.split (h, body.data(), body.size(), [&] (const std::uint8_t* d, std::size_t n)
        {
            out.emplace_back (d, d + n);
            return true;
        });
        return out;
    }

    /** Feeds datagrams; returns the bodies of the messages completed. */
    std::vector<Bytes> feed (Reassembler& r, const Datagrams& datagrams)
    {
        std::vector<Bytes> done;
        for (auto& d : datagrams)
        {
            Reassembler::Message m;
            if (r.add (d.data(), d.size(), m))
                done.emplace_back (m.body, m.body + m.size);
        }
        return done;
    }
}

int main()
{
    //--------------------------------------------------------------------------
    std::printf ("Header\n");
    {
        DatagramHeader h;
        h.kind = Kind::audioReply;
        h.flags = 0x8005;
        h.sessionId = 42;
        h.token = 0xfedcba9876543210ull;
        h.messageId = 0xffffffffu;
        h.fragment = 3;
        h.fragments = 9;

        Bytes b;
        ByteWriter w (b);
        writeHeader (w, h);
        CHECK (b.size() == headerSize);
        CHECK (b[0] == 'F' && b[1] == 'X' && b[2] == 'N' && b[3] == 'A');

        DatagramHeader g;
        ByteReader r (b.data(), b.size());
        CHECK (readHeader (r, g));
        CHECK (g.kind == Kind::audioReply && g.flags == 0x8005 && g.sessionId == 42);
        CHECK (g.token == h.token && g.messageId == h.messageId);
        CHECK (g.fragment == 3 && g.fragments == 9);

        b[4] = 2;   // another protocol version
        ByteReader r2 (b.data(), b.size());
        CHECK (! readHeader (r2, g));

        b[4] = protocolVersion;
        b[24] = 9; b[25] = 0;   // fragment 9 of 9: out of range
        ByteReader r3 (b.data(), b.size());
        CHECK (! readHeader (r3, g));
    }

    //--------------------------------------------------------------------------
    std::printf ("Audio request with transport, MIDI and a channel mask\n");
    {
        constexpr int samples = 128;
        auto audio = makeAudio (3, samples);
        auto ptrs = pointers (audio);

        BlockInfo info;
        info.block = 123456789012ull;
        info.samples = samples;
        info.mask = 0b1011;   // channels 0, 1 and 3; channel 2 disabled
        info.hasTransport = true;
        info.transport.bpm = 97.5;
        info.transport.ppqPosition = 12.25;
        info.transport.timeInSamples = -64;
        info.transport.timeSigNumerator = 7;
        info.transport.timeSigDenominator = 8;
        info.transport.playing = true;
        info.transport.looping = true;

        Bytes midi;
        MidiWriter mw (midi);
        const std::uint8_t noteOn[] = { 0x90, 60, 100 };
        const std::uint8_t noteOff[] = { 0x80, 60, 0 };
        CHECK (mw.add (5, noteOn, 3));
        CHECK (mw.add (127, noteOff, 3));
        CHECK (! mw.add (0, noteOn, 0));

        Bytes body;
        ByteWriter w (body);
        writeBlock (w, false, info, midi.data(), midi.size(), ptrs.data());
        CHECK (body.size() == 8 + 2 + 8 + 2 + transportSize + midi.size() + 3 * samples * 4);

        BlockView v;
        CHECK (readBlock (body.data(), body.size(), false, requestFlags::transport, v));
        CHECK (v.info.block == info.block && v.info.samples == samples && v.info.mask == 0b1011);
        CHECK (v.numChannels() == 3);
        CHECK (v.info.hasTransport && v.info.transport.bpm == 97.5 && v.info.transport.ppqPosition == 12.25);
        CHECK (v.info.transport.timeInSamples == -64);
        CHECK (v.info.transport.timeSigNumerator == 7 && v.info.transport.timeSigDenominator == 8);
        CHECK (v.info.transport.playing && ! v.info.transport.recording && v.info.transport.looping);

        MidiReader mr (v.midi, v.midiBytes);
        MidiEvent e;
        CHECK (mr.next (e) && e.sampleOffset == 5 && e.size == 3 && e.data[0] == 0x90);
        CHECK (mr.next (e) && e.sampleOffset == 127 && e.data[0] == 0x80);
        CHECK (! mr.next (e));

        std::vector<float> out (samples);
        bool same = true;
        for (int c = 0; c < 3; ++c)
        {
            v.copyChannel (c, out.data());
            same = same && out == audio[(std::size_t) c];
        }
        CHECK (same);

        CHECK (! readBlock (body.data(), body.size() - 1, false, requestFlags::transport, v));
        CHECK (! readBlock (body.data(), 10, false, requestFlags::transport, v));
    }

    //--------------------------------------------------------------------------
    std::printf ("Audio reply\n");
    {
        auto audio = makeAudio (2, 64);
        auto ptrs = pointers (audio);
        BlockInfo info;
        info.block = 3;
        info.samples = 64;
        info.mask = maskOfFirst (2);
        info.chainLatency = 512;
        info.processMicros = 37;

        Bytes body;
        ByteWriter w (body);
        writeBlock (w, true, info, nullptr, 0, ptrs.data());

        BlockView v;
        CHECK (readBlock (body.data(), body.size(), true, 0, v));
        CHECK (v.info.chainLatency == 512 && v.info.processMicros == 37);
        CHECK (! v.info.hasTransport && v.midiBytes == 0);
        std::vector<float> out (64);
        v.copyChannel (1, out.data());
        CHECK (out == audio[1]);
    }

    //--------------------------------------------------------------------------
    std::printf ("Masks\n");
    {
        CHECK (countChannels (0) == 0);
        CHECK (countChannels (~std::uint64_t (0)) == 64);
        CHECK (maskOfFirst (16) == 0xffffu);
        CHECK (maskOfFirst (64) == ~std::uint64_t (0));
    }

    //--------------------------------------------------------------------------
    std::printf ("Telemetry\n");
    {
        const std::uint8_t frame[] = { 1, 2, 3, 4, 5 };
        Bytes body;
        ByteWriter w (body);
        writeTelemetry (w, 9, 77, frame, sizeof (frame));
        TelemetryView v;
        CHECK (readTelemetry (body.data(), body.size(), v));
        CHECK (v.slotId == 9 && v.frame == 77 && v.size == 5 && v.codecFrame[4] == 5);
    }

    //--------------------------------------------------------------------------
    std::printf ("Fragmentation\n");
    {
        Fragmenter f (1400);
        const auto big = encodeRequest (1, 16, 256);   // 16 channels: about 16 kB
        const auto pieces = split (f, 10, big);
        CHECK (pieces.size() == (big.size() + 1399) / 1400);
        CHECK (pieces.front().size() == headerSize + 1400);

        // In order.
        {
            Reassembler r (1400, 1 << 18);
            auto done = feed (r, pieces);
            CHECK (done.size() == 1 && done[0] == big);
        }

        // Reversed, with a duplicate in the middle.
        {
            Reassembler r (1400, 1 << 18);
            Datagrams shuffled (pieces.rbegin(), pieces.rend());
            shuffled.insert (shuffled.begin() + 3, shuffled[1]);
            auto done = feed (r, shuffled);
            CHECK (done.size() == 1 && done[0] == big);
            CHECK (r.getStatistics().duplicates == 1);
        }

        // Two messages interleaved.
        {
            Reassembler r (1400, 1 << 18);
            const auto other = encodeRequest (2, 16, 256);
            const auto otherPieces = split (f, 11, other);
            Datagrams mixed;
            for (std::size_t i = 0; i < pieces.size(); ++i)
            {
                mixed.push_back (pieces[i]);
                mixed.push_back (otherPieces[i]);
            }
            auto done = feed (r, mixed);
            CHECK (done.size() == 2 && done[0] == big && done[1] == other);
        }

        // A lost fragment: the message never completes, and is dropped as
        // incomplete once the next one does.
        {
            Reassembler r (1400, 1 << 18);
            Datagrams lossy = pieces;
            lossy.erase (lossy.begin() + 2);
            CHECK (feed (r, lossy).empty());
            const auto next = encodeRequest (2, 16, 256);
            auto done = feed (r, split (f, 11, next));
            CHECK (done.size() == 1 && done[0] == next);
            CHECK (r.getStatistics().incomplete == 1);
        }

        // Single-datagram messages, including an empty body.
        {
            Reassembler r (1400, 1 << 18);
            const auto small = encodeRequest (3, 2, 128);
            auto one = split (f, 12, small);
            CHECK (one.size() == 1);
            auto done = feed (r, one);
            CHECK (done.size() == 1 && done[0] == small);

            auto empty = split (f, 13, Bytes{});
            CHECK (empty.size() == 1 && empty[0].size() == headerSize);
            done = feed (r, empty);
            CHECK (done.size() == 1 && done[0].empty());
        }

        // Malformed: a short non-last fragment, garbage, a truncated header.
        {
            Reassembler r (1400, 1 << 18);
            Datagrams bad = { pieces[0] };
            bad[0].pop_back();
            CHECK (feed (r, bad).empty());
            CHECK (feed (r, { Bytes (40, 0xab) }).empty());
            CHECK (feed (r, { Bytes (pieces[0].begin(), pieces[0].begin() + 20) }).empty());
            CHECK (r.getStatistics().malformed >= 3);
            CHECK (r.getStatistics().tooLarge == 0);
        }

        // A message over the limit: dropped, counted once as too large
        // (not once per fragment, not as malformed).
        {
            Reassembler r (1400, 4096);
            CHECK (feed (r, pieces).empty());           // 16 kB > the 4 kB limit
            CHECK (r.getStatistics().tooLarge == 1);
            CHECK (r.getStatistics().malformed == 0);
        }
    }

    //--------------------------------------------------------------------------
    std::printf ("Control frames\n");
    {
        using namespace fxmenet::control;

        SlotAdd add;
        add.slotId = 3;
        add.position = 1;
        add.uid = 0x1668ba35;
        add.name = "FxmeCompressor";
        add.vendor = "FX-Mechanics";
        add.state.assign (100000, 0x5a);

        std::vector<ParamChange> params { { 3, 7, 0.25f }, { 4, 0, 1.0f } };
        SlotState st { 3, { 1, 2, 3 } };
        std::vector<Route> routes { { routingInputs, 0, 3, 0 }, { 3, 1, routingOutputs, 1 }, { 7, 65535, 9, 2 } };

        Bytes stream;
        writeFrame (stream, Type::hello, flagJson, 1, (const std::uint8_t*) "{}", 2);
        const auto addBytes = encode (add);
        writeFrame (stream, Type::slotAdd, 0, 2, addBytes.data(), addBytes.size());
        const auto paramBytes = encode (params);
        writeFrame (stream, Type::paramSet, 0, 0, paramBytes.data(), paramBytes.size());
        const auto stateBytes = encode (st);
        writeFrame (stream, Type::stateSet, 0, 3, stateBytes.data(), stateBytes.size());
        const auto routeBytes = encode (routes);
        writeFrame (stream, Type::routing, 0, 4, routeBytes.data(), routeBytes.size());
        writeFrame (stream, Type::bye, 0, 0, nullptr, 0);

        // The same frames whatever the read boundaries: one byte at a time,
        // then in chunks of 1000.
        for (std::size_t chunk : { std::size_t (1), std::size_t (1000) })
        {
            FrameAssembler a;
            std::vector<Frame> frames;
            for (std::size_t i = 0; i < stream.size(); i += chunk)
            {
                a.append (stream.data() + i, std::min (chunk, stream.size() - i));
                Frame f;
                while (a.next (f) == FrameAssembler::Result::frame)
                    frames.push_back (f);
            }
            CHECK (frames.size() == 6);
            if (frames.size() != 6)
                continue;

            CHECK (frames[0].type == Type::hello && frames[0].isJson() && frames[0].requestId == 1);
            CHECK (frames[5].type == Type::bye && frames[5].payload.empty());

            SlotAdd got;
            CHECK (frames[1].type == Type::slotAdd && decode (frames[1].payload, got));
            CHECK (got.slotId == 3 && got.position == 1 && got.uid == 0x1668ba35);
            CHECK (got.name == "FxmeCompressor" && got.vendor == "FX-Mechanics" && got.state == add.state);

            std::vector<ParamChange> gotParams;
            CHECK (decode (frames[2].payload, gotParams) && gotParams.size() == 2);
            CHECK (gotParams.size() == 2 && gotParams[1].slotId == 4 && gotParams[0].value == 0.25f);

            SlotState gotState;
            CHECK (decode (frames[3].payload, gotState) && gotState.slotId == 3 && gotState.state == st.state);

            std::vector<Route> gotRoutes;
            CHECK (frames[4].type == Type::routing && frames[4].requestId == 4);
            CHECK (decode (frames[4].payload, gotRoutes) && gotRoutes == routes);
        }

        // Corrupt lengths: below 8, over the limit.
        {
            FrameAssembler a (1024);
            const std::uint8_t tooShort[] = { 3, 0, 0, 0, 1, 0 };
            a.append (tooShort, sizeof (tooShort));
            Frame f;
            CHECK (a.next (f) == FrameAssembler::Result::corrupt);

            FrameAssembler b (1024);
            Bytes big, payload (2000, 0);
            writeFrame (big, Type::stateSet, 0, 0, payload.data(), payload.size());   // 2008 > 1024
            b.append (big.data(), 16);
            CHECK (b.next (f) == FrameAssembler::Result::corrupt);
        }

        // Truncated payloads are refused.
        {
            SlotAdd bad;
            Bytes cut (addBytes.begin(), addBytes.begin() + 20);
            CHECK (! decode (cut, bad));
            std::vector<ParamChange> badParams;
            Bytes lying { 255, 255, 0, 0 };   // 65535 changes, no data
            CHECK (! decode (lying, badParams));

            std::vector<Route> badRoutes;
            CHECK (! decode (lying, badRoutes));
            Bytes cutRoutes (routeBytes.begin(), routeBytes.end() - 1);
            CHECK (! decode (cutRoutes, badRoutes));
            std::vector<Route> none;
            CHECK (decode (encode (none), badRoutes) && badRoutes.empty());   // an empty graph
        }
    }

    //--------------------------------------------------------------------------
    // A JSFX slot's state: the script bundle, the sliders, @serialize's bytes.
    {
        namespace jsfx = fxmenet::jsfx;
        jsfx::SlotState s;
        s.bundle.main = "utility/volume";
        s.bundle.files.push_back ({ "utility/volume", false, { 'd', 'e', 's', 'c', ':' } });
        s.bundle.files.push_back ({ "lib/common.jsfx-inc", false, { 1, 2, 3 } });
        s.bundle.files.push_back ({ "img/knob.png", true, {} });
        s.sliders = "-6.0 0.0";
        s.serialized = { 9, 8, 7, 6 };

        const auto bytes = jsfx::encode (s);
        jsfx::SlotState g;
        CHECK (jsfx::decode (bytes.data(), bytes.size(), g));
        CHECK (g.bundle.main == "utility/volume" && g.bundle.files.size() == 3);
        CHECK (g.bundle.files[1].path == "lib/common.jsfx-inc" && g.bundle.files[1].bytes.size() == 3 && ! g.bundle.files[1].data);
        CHECK (g.bundle.files[2].data && g.bundle.files[2].bytes.empty());
        CHECK (g.sliders == "-6.0 0.0" && g.serialized.size() == 4 && g.serialized[3] == 6);

        // Cut short, or not a bundle at all.
        CHECK (! jsfx::decode (bytes.data(), bytes.size() - 1, g));
        CHECK (! jsfx::decode (bytes.data(), 6, g));
        const std::uint8_t junk[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        CHECK (! jsfx::decode (junk, sizeof (junk), g));

        // The main script must be one of the files.
        auto noMain = s;
        noMain.bundle.main = "utility/other";
        const auto noMainBytes = jsfx::encode (noMain);
        CHECK (! jsfx::decode (noMainBytes.data(), noMainBytes.size(), g));

        // Paths that would leave the folder they are written to.
        CHECK (jsfx::isSafePath ("a/b c/d.jsfx-inc") && jsfx::isSafePath ("volume"));
        for (const char* bad : { "", "/etc/passwd", "../x", "a/../../x", "a//b", "a/./b", "a/", "C:/x", "a\\b", "a/b\n" })
            CHECK (! jsfx::isSafePath (bad));
        auto escaping = s;
        escaping.bundle.files[1].path = "../../.bashrc";
        const auto escapingBytes = jsfx::encode (escaping);
        CHECK (! jsfx::decode (escapingBytes.data(), escapingBytes.size(), g));
    }

    //--------------------------------------------------------------------------
    // The JSFX @gfx mirror: SyncSource on one side, a copy kept on the other.
    {
        namespace jsfx = fxmenet::jsfx;
        double a = 1.0, b = 2.0;
        std::vector<double> mem (jsfx::blockItems * 2, 0.0);   // the server's blocks 3 and 7
        const auto memory = [&mem] (std::uint32_t block) -> const double*
        {
            return block == 3 ? mem.data() : block == 7 ? mem.data() + jsfx::blockItems : nullptr;
        };

        // The client's copy: variables by index, blocks by number.
        std::vector<double> vars;
        std::map<std::uint32_t, std::vector<double>> blocks;
        std::vector<std::uint8_t> wire;
        const auto apply = [&] (const jsfx::SyncFrame& sent)
        {
            jsfx::encode (sent, wire);
            jsfx::SyncFrame f;
            if (! jsfx::decode (wire.data(), wire.size(), f))
                return false;
            if (f.reset)
                vars.assign (f.names.size(), 0.0);
            for (auto& v : f.vars)
                vars.at (v.index) = v.value;
            for (auto& r : f.ranges)
            {
                auto& blk = blocks[r.offset / jsfx::blockItems];
                blk.resize (jsfx::blockItems, 0.0);
                std::copy (r.values.begin(), r.values.end(), blk.begin() + r.offset % jsfx::blockItems);
            }
            return true;
        };

        jsfx::SyncSource source;
        source.reset ({ "a", "b" }, { &a, &b });
        jsfx::SyncFrame f;
        CHECK (source.collect (5, memory, 1 << 20, f) && f.reset && f.names.size() == 2 && f.vars.size() == 2);
        CHECK (apply (f) && vars[0] == 1.0 && vars[1] == 2.0);
        CHECK (! source.collect (5, memory, 1 << 20, f));   // nothing changed

        b = 3.5;
        CHECK (source.collect (5, memory, 1 << 20, f) && ! f.reset && f.vars.size() == 1 && f.vars[0].index == 1);
        CHECK (apply (f) && vars[1] == 3.5);

        // Memory: only what differs from what the client has (zeros at first).
        mem[10] = 0.5;
        mem[jsfx::blockItems + 1000] = -1.0;
        source.subscribe ({ 3, 7, 9 });   // 9: not allocated on the server, all zeros
        CHECK (source.collect (5, memory, 1 << 20, f) && f.ranges.size() == 2);
        CHECK (apply (f) && blocks[3][10] == 0.5 && blocks[7][1000] == -1.0);
        CHECK (! source.collect (5, memory, 1 << 20, f));

        // A small budget: the rest waits for the next frames, nothing is lost.
        for (std::size_t i = 0; i < jsfx::blockItems; ++i)
            mem[i] = (double) i;
        int frames = 0;
        while (source.collect (5, memory, 8 * 1024, f))
        {
            CHECK (wire.size() < 9 * 1024 || frames == 0);
            CHECK (apply (f));
            ++frames;
            if (frames > 1000)
                break;
        }
        CHECK (frames > 1 && frames < 1000);
        bool same = true;
        for (std::size_t i = 0; i < jsfx::blockItems; ++i)
            same = same && blocks[3][i] == mem[i];
        CHECK (same);

        // Malformed frames: a range past its block, cut short.
        jsfx::SyncFrame bad;
        bad.ranges.push_back ({ jsfx::blockItems - 2, { 1.0, 2.0, 3.0 } });
        jsfx::encode (bad, wire);
        CHECK (! jsfx::decode (wire.data(), wire.size(), f));
        jsfx::SyncFrame good;
        good.vars.push_back ({ 0, 1.0 });
        jsfx::encode (good, wire);
        CHECK (! jsfx::decode (wire.data(), wire.size() - 1, f));

        // The display wrote: into the script, and not sent back.
        jsfx::SyncFrame w;
        w.slotId = 5;
        w.vars.push_back ({ 0, 42.0 });
        w.ranges.push_back ({ 3 * jsfx::blockItems + 20, std::vector<double> (jsfx::chunkItems, 7.0) });
        double* const block3 = mem.data();
        CHECK (source.applyWrite (w, [block3] (std::uint32_t block) { return block == 3 ? block3 : nullptr; }));
        CHECK (a == 42.0 && mem[20] == 7.0 && mem[20 + jsfx::chunkItems - 1] == 7.0);
        while (source.collect (5, memory, 1 << 20, f))   // what the budget left behind, then nothing of the write
            CHECK (f.vars.empty() && apply (f) && blocks[3][20] != 7.0);
        jsfx::SyncFrame unknown;
        unknown.vars.push_back ({ 99, 1.0 });
        CHECK (! source.applyWrite (unknown, [] (std::uint32_t) -> double* { return nullptr; }));

        std::uint32_t slot = 0;
        std::vector<std::uint32_t> list;
        CHECK (jsfx::decodeSubscribe (jsfx::encodeSubscribe (4, { 1, 2, 2047 }), slot, list) && slot == 4 && list.size() == 3);
    }

    //--------------------------------------------------------------------------
    std::printf ("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
