/*
  ------------------------------------------------------------------------------
    ControlConnection.h

    One control channel connection (protocol.md section 4) over a TCP
    socket, either side: send frames from any thread (a lock keeps frames
    whole), read them from one thread with a timeout. Never used on an
    audio thread.

    JUCE's StreamingSocket already sets TCP_NODELAY, so small frames are not
    held back by Nagle's algorithm.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

#include <FxmeNet/control/ControlFrame.h>
#include <FxmeNet/control/ControlMessages.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace fxmenet::control
{

class ControlConnection
{
public:
    explicit ControlConnection (std::unique_ptr<juce::StreamingSocket> connectedSocket,
                                std::size_t maxFrameSize = defaultMaxFrameSize)
        : socket (std::move (connectedSocket)), assembler (maxFrameSize)
    {
    }

    /** Connects to a server's control port; nullptr on failure. */
    static std::unique_ptr<ControlConnection> connect (const juce::String& host, int port, int timeoutMs)
    {
        auto s = std::make_unique<juce::StreamingSocket>();
        if (! s->connect (host, port, timeoutMs))
            return nullptr;
        return std::make_unique<ControlConnection> (std::move (s));
    }

    bool isConnected() const    { return socket != nullptr && socket->isConnected() && ! failed; }

    void close()
    {
        if (socket != nullptr)
            socket->close();
    }

    //==============================================================================
    /** Sends one frame. Any thread. False when the connection is broken. */
    bool send (Type type, std::uint16_t flags, std::uint32_t requestId,
               const std::uint8_t* payload, std::size_t size)
    {
        const std::lock_guard<std::mutex> lock (sendLock);
        outgoing.clear();
        writeFrame (outgoing, type, flags, requestId, payload, size);

        std::size_t done = 0;
        while (done < outgoing.size())
        {
            const int n = socket->write (outgoing.data() + done, (int) (outgoing.size() - done));
            if (n <= 0)
            {
                failed = true;
                return false;
            }
            done += (std::size_t) n;
        }
        return true;
    }

    bool sendBinary (Type type, std::uint32_t requestId, const std::vector<std::uint8_t>& payload)
    {
        return send (type, 0, requestId, payload.data(), payload.size());
    }

    bool sendJson (Type type, std::uint32_t requestId, const juce::var& message)
    {
        const auto payload = toPayload (message);
        return send (type, flagJson, requestId, payload.data(), payload.size());
    }

    bool sendEmpty (Type type, std::uint32_t requestId = 0)
    {
        return send (type, 0, requestId, nullptr, 0);
    }

    bool sendError (std::uint32_t requestId, int code, const juce::String& text)
    {
        ErrorMessage e;
        e.code = code;
        e.text = text;
        return sendJson (Type::error, requestId, e.toVar());
    }

    //==============================================================================
    enum class ReadResult { frame, timeout, closed };

    /** Waits up to `timeoutMs` for the next frame. One reading thread. After
        `closed` (peer gone, socket error or a corrupt stream) the connection
        is finished. */
    ReadResult read (Frame& out, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (0, timeoutMs);

        for (;;)
        {
            switch (assembler.next (out))
            {
                case FrameAssembler::Result::frame:     return ReadResult::frame;
                case FrameAssembler::Result::corrupt:   failed = true; return ReadResult::closed;
                case FrameAssembler::Result::needMore:  break;
            }

            if (failed || socket == nullptr || ! socket->isConnected())
                return ReadResult::closed;

            const auto now = juce::Time::getMillisecondCounter();
            const int remaining = now >= deadline ? 0 : (int) (deadline - now);
            const int ready = socket->waitUntilReady (true, remaining);
            if (ready < 0)
            {
                failed = true;
                return ReadResult::closed;
            }
            if (ready == 0)
                return ReadResult::timeout;

            const int n = socket->read (readBuffer, (int) sizeof (readBuffer), false);
            if (n <= 0)   // ready to read but nothing: the peer closed
            {
                failed = true;
                return ReadResult::closed;
            }
            assembler.append (reinterpret_cast<const std::uint8_t*> (readBuffer), (std::size_t) n);
        }
    }

    /** The peer's address, for logs and for the server's UDP routing. */
    juce::String getPeerHost() const   { return socket != nullptr ? socket->getHostName() : juce::String(); }

private:
    std::unique_ptr<juce::StreamingSocket> socket;
    FrameAssembler assembler;
    char readBuffer[16384];
    std::mutex sendLock;
    std::vector<std::uint8_t> outgoing;
    std::atomic<bool> failed { false };

    JUCE_DECLARE_NON_COPYABLE (ControlConnection)
};

} // namespace fxmenet::control
