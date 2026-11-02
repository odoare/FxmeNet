/*
  ------------------------------------------------------------------------------
    TransportTests.cpp

    UdpTransport over the loopback interface: a "client" sends audio requests
    (16 channels x 256 samples, so each one is split into datagrams), an
    "echo server" thread answers each with a reply carrying the same samples
    scaled by 0.5 and a latency, and the client checks every sample. Also:
    a datagram with the wrong token is ignored, a receive with nothing to
    read times out.

    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include <juce_core/juce_core.h>

#include <FxmeNet/transport/UdpTransport.h>

#include <cstdio>
#include <vector>

namespace
{
    using namespace fxmenet;
    using namespace fxmenet::wire;

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

    constexpr int numChannels = 16;
    constexpr int numSamples  = 256;
    constexpr int numBlocks   = 200;

    float sampleValue (std::uint64_t block, int channel, int i)
    {
        return (float) ((block * 7 + (std::uint64_t) channel * 131 + (std::uint64_t) i) % 2000) * 0.0005f - 0.5f;
    }

    /** Answers each request with its samples halved, until told to stop. */
    class EchoServer : public juce::Thread
    {
    public:
        EchoServer (juce::DatagramSocket& s, int clientPort, const UdpTransport::Settings& settings)
            : juce::Thread ("echo server"), transport (s, "127.0.0.1", clientPort, settings) {}

        void run() override
        {
            std::vector<std::vector<float>> audio ((std::size_t) numChannels, std::vector<float> (numSamples));
            std::vector<const float*> ptrs;
            for (auto& ch : audio)
                ptrs.push_back (ch.data());
            std::vector<std::uint8_t> reply;
            reply.reserve (1 << 17);

            while (! threadShouldExit())
            {
                ReceivedMessage m;
                if (! transport.receive (m, 50) || m.kind != Kind::audioRequest)
                    continue;

                BlockView v;
                if (! readBlock (m.body, m.size, false, m.flags, v))
                    continue;

                for (int c = 0; c < v.numChannels(); ++c)
                {
                    v.copyChannel (c, audio[(std::size_t) c].data());
                    for (auto& x : audio[(std::size_t) c])
                        x *= 0.5f;
                }

                BlockInfo info = v.info;
                info.chainLatency = 333;
                reply.clear();
                ByteWriter w (reply);
                writeBlock (w, true, info, nullptr, 0, ptrs.data());
                transport.send (Kind::audioReply, 0, reply.data(), reply.size());
            }
        }

    private:
        UdpTransport transport;
    };
}

int main()
{
    juce::DatagramSocket clientSocket, serverSocket;
    CHECK (clientSocket.bindToPort (0, "127.0.0.1"));
    CHECK (serverSocket.bindToPort (0, "127.0.0.1"));
    const int clientPort = clientSocket.getBoundPort();
    const int serverPort = serverSocket.getBoundPort();
    CHECK (clientPort > 0 && serverPort > 0);

    UdpTransport::Settings settings;
    settings.sessionId = 5;
    settings.token = 0x0123456789abcdefull;

    UdpTransport client (clientSocket, "127.0.0.1", serverPort, settings);

    //--------------------------------------------------------------------------
    std::printf ("Nothing to read: receive times out\n");
    {
        ReceivedMessage m;
        const auto start = juce::Time::getMillisecondCounter();
        CHECK (! client.receive (m, 100));
        CHECK (juce::Time::getMillisecondCounter() - start >= 90);
    }

    EchoServer server (serverSocket, clientPort, settings);
    server.startThread();

    //--------------------------------------------------------------------------
    std::printf ("%d fragmented requests and replies, sample-exact\n", numBlocks);
    {
        std::vector<std::vector<float>> audio ((std::size_t) numChannels, std::vector<float> (numSamples));
        std::vector<const float*> ptrs;
        for (auto& ch : audio)
            ptrs.push_back (ch.data());
        std::vector<std::uint8_t> request;
        request.reserve (1 << 17);
        std::vector<float> out (numSamples);

        int received = 0, exact = 0;
        for (std::uint64_t block = 0; block < (std::uint64_t) numBlocks; ++block)
        {
            for (int c = 0; c < numChannels; ++c)
                for (int i = 0; i < numSamples; ++i)
                    audio[(std::size_t) c][(std::size_t) i] = sampleValue (block, c, i);

            BlockInfo info;
            info.block = block;
            info.samples = numSamples;
            info.mask = maskOfFirst (numChannels);
            request.clear();
            ByteWriter w (request);
            writeBlock (w, false, info, nullptr, 0, ptrs.data());
            CHECK (client.send (Kind::audioRequest, 0, request.data(), request.size()));

            ReceivedMessage m;
            if (! client.receive (m, 1000))
                continue;
            ++received;

            BlockView v;
            if (! readBlock (m.body, m.size, true, m.flags, v) || v.info.block != block
                || v.info.chainLatency != 333 || v.numChannels() != numChannels)
                continue;

            bool same = true;
            for (int c = 0; c < numChannels && same; ++c)
            {
                v.copyChannel (c, out.data());
                for (int i = 0; i < numSamples; ++i)
                    same = same && juce::exactlyEqual (out[(std::size_t) i], sampleValue (block, c, i) * 0.5f);
            }
            exact += same ? 1 : 0;
        }

        std::printf ("  %d received, %d exact; reassembly: %llu complete, %llu incomplete\n",
                     received, exact,
                     (unsigned long long) client.getReassemblyStatistics().completed,
                     (unsigned long long) client.getReassemblyStatistics().incomplete);
        CHECK (received == numBlocks);   // loopback does not lose datagrams in practice
        CHECK (exact == received);
    }

    //--------------------------------------------------------------------------
    std::printf ("A datagram with the wrong token is ignored\n");
    {
        UdpTransport::Settings intruder = settings;
        intruder.token ^= 1;
        juce::DatagramSocket intruderSocket;
        CHECK (intruderSocket.bindToPort (0, "127.0.0.1"));
        UdpTransport stray (intruderSocket, "127.0.0.1", clientPort, intruder);
        const std::uint8_t nothing[4] = {};
        CHECK (stray.send (Kind::keepAlive, 0, nothing, sizeof (nothing)));

        ReceivedMessage m;
        CHECK (! client.receive (m, 200));
        CHECK (client.getForeignCount() == 1);
    }

    server.stopThread (2000);

    std::printf ("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
