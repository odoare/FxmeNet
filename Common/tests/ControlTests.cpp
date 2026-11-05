/*
  ------------------------------------------------------------------------------
    ControlTests.cpp

    The control channel with JUCE: every JSON message round-trips through
    its text (including 64-bit tokens and masks, which JSON numbers could
    not carry exactly), and a TCP exchange on the loopback interface: HELLO
    and WELCOME, a 1 MB plugin state in SLOT_ADD echoed back as STATE, a
    48 MB one echoed from another thread while the server's thread keeps
    reading and the client reads nothing for a while (send() must wait for
    room in the socket buffer, not fail, as the real server's message
    thread sends while its reader thread reads), an ERROR reply, and the server seeing the client close.

    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include <juce_core/juce_core.h>

#include <FxmeNet/control/ControlConnection.h>

#include <cstdio>
#include <thread>

namespace
{
    using namespace fxmenet::control;

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

    /** Through JSON text and back, as the wire does. */
    juce::var viaText (const juce::var& v)
    {
        return fromPayload (toPayload (v));
    }

    /** Accepts one client: answers HELLO, echoes SLOT_ADD's state as STATE,
        refuses SLOT_REMOVE with an ERROR, notes when the client closes. */
    class TestServer : public juce::Thread
    {
    public:
        explicit TestServer (juce::StreamingSocket& l) : juce::Thread ("control server"), listener (l) {}

        std::atomic<bool> sawClose { false };
        std::atomic<int> framesHandled { 0 };

        std::atomic<bool> bigSent { false };

        void run() override
        {
            std::unique_ptr<juce::StreamingSocket> s (listener.waitForNextConnection());
            if (s == nullptr)
                return;
            ControlConnection c (std::move (s));
            std::thread sender;   // big echoes: sent while this thread reads
            struct Join { std::thread& t; ~Join() { if (t.joinable()) t.join(); } } join { sender };

            while (! threadShouldExit())
            {
                Frame f;
                const auto r = c.read (f, 100);
                if (r == ControlConnection::ReadResult::closed)
                {
                    sawClose = true;
                    return;
                }
                if (r != ControlConnection::ReadResult::frame)
                    continue;
                ++framesHandled;

                if (f.type == Type::hello)
                {
                    const auto hello = Hello::fromVar (fromPayload (f.payload));
                    Welcome w;
                    w.sessionId = 77;
                    w.token = 0xfeedfacecafebeefull;
                    w.serverName = "test server, client " + hello.clientName;
                    c.sendJson (Type::welcome, f.requestId, w.toVar());
                }
                else if (f.type == Type::slotAdd)
                {
                    SlotAdd add;
                    if (! decode (f.payload, add))
                        continue;
                    if (add.state.size() < ((std::size_t) 8 << 20))
                        c.sendBinary (Type::state, f.requestId, encode (SlotState { add.slotId, add.state }));
                    else
                        sender = std::thread ([this, &c, id = f.requestId, reply = encode (SlotState { add.slotId, add.state })]
                                              { bigSent = c.sendBinary (Type::state, id, reply); });
                }
                else if (f.type == Type::slotRemove)
                {
                    c.sendError (f.requestId, ErrorMessage::noSuchSlot, "no slot 9");
                }
            }
        }

    private:
        juce::StreamingSocket& listener;
    };
}

int main()
{
    //--------------------------------------------------------------------------
    std::printf ("JSON messages round-trip\n");
    {
        Hello h;
        h.clientName = "Reaper track 3";
        h.os = "Linux";
        h.resume = true;
        h.resumeSessionId = 12;
        h.resumeToken = 0xffffffffffffff01ull;   // beyond what a double holds exactly
        const auto h2 = Hello::fromVar (viaText (h.toVar()));
        CHECK (h2.clientName == h.clientName && h2.os == "Linux" && h2.versions.size() == 1);
        CHECK (h2.resume && h2.resumeSessionId == 12 && h2.resumeToken == h.resumeToken);

        Welcome w;
        w.sessionId = 99;
        w.token = 0x8000000000000001ull;
        w.capabilities.add ("tcp-audio");
        const auto w2 = Welcome::fromVar (viaText (w.toVar()));
        CHECK (w2.sessionId == 99 && w2.token == w.token && w2.udpPort == 54321);
        CHECK (w2.capabilities.contains ("udp-audio") && w2.capabilities.contains ("tcp-audio"));
        CHECK (w2.error.isEmpty() && ! w2.resumed);

        Configure c;
        c.sampleRate = 44100.0;
        c.blockSize = 256;
        c.numInputs = 16;
        c.inputMask = 0b0111;
        c.outputMask = 0xf000000000000000ull;
        const auto c2 = Configure::fromVar (viaText (c.toVar()));
        CHECK (c2.blockSize == 256 && c2.numInputs == 16);
        CHECK (c2.inputMask == 0b0111 && c2.outputMask == c.outputMask);
        CHECK (juce::exactlyEqual (c2.sampleRate, 44100.0));

        PluginEntry p;
        p.uid = 0x1668ba35;
        p.name = "FxmeCompressor";
        p.vendor = "FX-Mechanics";
        p.fxmenetAbi = 1;
        p.platforms.add ("x86_64-linux");
        PluginEntry q;
        q.uid = 0xffffffffu;
        q.name = "ThirdParty";
        const auto list = pluginListFromVar (viaText (pluginListToVar ({ p, q })));
        CHECK (list.size() == 2);
        if (list.size() == 2)
        {
            CHECK (list[0].uid == 0x1668ba35 && list[0].name == "FxmeCompressor" && list[0].fxmenetAbi == 1);
            CHECK (list[0].platforms.contains ("x86_64-linux"));
            CHECK (list[1].uid == 0xffffffffu && list[1].fxmenetAbi == 0);
        }

        SlotReady s;
        s.slotId = 4;
        s.latency = 64;
        ParamInfo pi;
        pi.index = 2;
        pi.paramId = "ratio";
        pi.name = "Ratio";
        pi.defaultValue = 0.25f;
        pi.value = 0.75f;
        pi.numSteps = 3;
        pi.stepTexts.addArray ({ "1:1", "2:1", "4:1" });
        s.params.push_back (pi);
        const std::uint8_t schema[] = { 'F', 'X', 'T', 'S', 1, 0, 0, 0 };
        s.telemetrySchema.append (schema, sizeof (schema));
        s.fxmenetAbi = 1;
        const auto s2 = SlotReady::fromVar (viaText (s.toVar()));
        CHECK (s2.slotId == 4 && s2.latency == 64 && s2.fxmenetAbi == 1);
        CHECK (s2.params.size() == 1);
        if (! s2.params.empty())
        {
            CHECK (s2.params[0].paramId == "ratio" && s2.params[0].numSteps == 3);
            CHECK (juce::exactlyEqual (s2.params[0].defaultValue, 0.25f));
            CHECK (juce::exactlyEqual (s2.params[0].value, 0.75f));
            CHECK (s2.params[0].stepTexts.size() == 3 && s2.params[0].automatable);
        }
        CHECK (s2.telemetrySchema == s.telemetrySchema);

        // A newer peer's extra fields are ignored, missing ones keep defaults.
        juce::var extra (new juce::DynamicObject());
        extra.getDynamicObject()->setProperty ("blockSize", 64);
        extra.getDynamicObject()->setProperty ("someFutureField", "x");
        const auto c3 = Configure::fromVar (extra);
        CHECK (c3.blockSize == 64 && c3.telemetryHz == 30 && c3.inputMask == 3);

        // An older server's parameter, without "value": the default.
        juce::var oldParam (new juce::DynamicObject());
        oldParam.getDynamicObject()->setProperty ("default", 0.5);
        CHECK (juce::exactlyEqual (ParamInfo::fromVar (oldParam).value, 0.5f));

        CHECK (fromPayload ({ '{', 'b', 'a', 'd' }).isVoid());
    }

    //--------------------------------------------------------------------------
    std::printf ("TCP exchange on the loopback interface\n");
    {
        juce::StreamingSocket listener;
        CHECK (listener.createListener (0, "127.0.0.1"));
        const int port = listener.getBoundPort();

        TestServer server (listener);
        server.startThread();

        auto client = ControlConnection::connect ("127.0.0.1", port, 2000);
        CHECK (client != nullptr);
        if (client != nullptr)
        {
            Hello h;
            h.clientName = "tester";
            CHECK (client->sendJson (Type::hello, 1, h.toVar()));

            Frame f;
            CHECK (client->read (f, 2000) == ControlConnection::ReadResult::frame);
            CHECK (f.type == Type::welcome && f.requestId == 1 && f.isJson());
            const auto w = Welcome::fromVar (fromPayload (f.payload));
            CHECK (w.sessionId == 77 && w.token == 0xfeedfacecafebeefull);
            CHECK (w.serverName == "test server, client tester");

            SlotAdd add;
            add.slotId = 5;
            add.uid = 0xabcdef01;
            add.name = "Big";
            add.state.resize (1 << 20);
            for (std::size_t i = 0; i < add.state.size(); ++i)
                add.state[i] = (std::uint8_t) (i * 31 + 7);
            CHECK (client->sendBinary (Type::slotAdd, 2, encode (add)));

            CHECK (client->read (f, 5000) == ControlConnection::ReadResult::frame);
            SlotState echoed;
            CHECK (f.type == Type::state && f.requestId == 2 && decode (f.payload, echoed));
            CHECK (echoed.slotId == 5 && echoed.state == add.state);

            // A frame far larger than the socket buffers, while the client
            // reads nothing for a while: the server's send must wait for
            // room, not fail (both sockets are non-blocking by now).
            add.slotId = 6;
            add.state.assign ((std::size_t) 48 << 20, 0x5a);
            CHECK (client->sendBinary (Type::slotAdd, 4, encode (add)));
            juce::Thread::sleep (500);
            CHECK (client->read (f, 10000) == ControlConnection::ReadResult::frame);
            CHECK (f.type == Type::state && f.requestId == 4 && decode (f.payload, echoed));
            CHECK (echoed.slotId == 6 && echoed.state == add.state);
            for (int i = 0; i < 50 && ! server.bigSent; ++i)   // send() returns just after the last byte
                juce::Thread::sleep (20);
            CHECK (server.bigSent.load());

            CHECK (client->sendBinary (Type::slotRemove, 3, encode (SlotValue { 9, 0 })));
            CHECK (client->read (f, 2000) == ControlConnection::ReadResult::frame);
            CHECK (f.type == Type::error && f.requestId == 3);
            const auto e = ErrorMessage::fromVar (fromPayload (f.payload));
            CHECK (e.code == ErrorMessage::noSuchSlot && e.text == "no slot 9");

            CHECK (client->read (f, 100) == ControlConnection::ReadResult::timeout);

            client->close();
            client.reset();
            for (int i = 0; i < 50 && ! server.sawClose; ++i)
                juce::Thread::sleep (20);
            CHECK (server.sawClose.load());
            CHECK (server.framesHandled.load() == 4);
        }

        server.stopThread (2000);
    }

    std::printf ("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
