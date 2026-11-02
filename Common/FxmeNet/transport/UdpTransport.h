/*
  ------------------------------------------------------------------------------
    UdpTransport.h

    AudioTransport over UDP for one session, talking to one peer: messages
    are split into datagrams of the session's budget, reassembled on the way
    in, and datagrams of another session or with the wrong token are
    ignored. This is the client's side; the server shares one socket among
    its sessions and routes datagrams itself, with the same Fragmenter and
    Reassembler.

    Header-only on purpose: JUCE code compiled into a static library and then
    into several JUCE targets gives duplicate symbols.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

#include <FxmeNet/protocol/Fragmentation.h>
#include <FxmeNet/transport/AudioTransport.h>

#include <atomic>
#include <vector>

namespace fxmenet
{

class UdpTransport : public AudioTransport
{
public:
    struct Settings
    {
        std::uint32_t sessionId = 0;
        std::uint64_t token = 0;
        std::size_t datagramBudget = wire::defaultDatagramBudget;
        std::size_t maxMessageSize = 1 << 18;   ///< 64 channels x 1024 samples, with room
    };

    /** `socket` must outlive the transport; it may be shared with another
        transport only if that one never receives. */
    UdpTransport (juce::DatagramSocket& socketToUse, juce::String remoteHostName,
                  int remotePortNumber, const Settings& settingsToUse)
        : socket (socketToUse), remoteHost (std::move (remoteHostName)), remotePort (remotePortNumber),
          settings (settingsToUse),
          fragmenter (settings.datagramBudget),
          reassembler (settings.datagramBudget, settings.maxMessageSize),
          datagram (wire::headerSize + settings.datagramBudget + 64)
    {
    }

    bool send (wire::Kind kind, std::uint16_t flags,
               const std::uint8_t* body, std::size_t size) override
    {
        wire::DatagramHeader h;
        h.kind = kind;
        h.flags = flags;
        h.sessionId = settings.sessionId;
        h.token = settings.token;
        h.messageId = nextMessageId++;

        return fragmenter.split (h, body, size, [this] (const std::uint8_t* data, std::size_t n)
        {
            return socket.write (remoteHost, remotePort, data, (int) n) == (int) n;
        });
    }

    bool receive (ReceivedMessage& out, int timeoutMs) override
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (0, timeoutMs);

        for (;;)
        {
            const auto now = juce::Time::getMillisecondCounter();
            const int remaining = now >= deadline ? 0 : (int) (deadline - now);
            if (socket.waitUntilReady (true, remaining) != 1)
                return false;

            juce::String senderHost;
            int senderPort = 0;
            const int n = socket.read (datagram.data(), (int) datagram.size(), false, senderHost, senderPort);
            if (n <= 0)
                return false;

            wire::Reassembler::Message m;
            if (! reassembler.add (datagram.data(), (std::size_t) n, m))
                continue;

            if (m.header.sessionId != settings.sessionId || m.header.token != settings.token)
            {
                foreign.fetch_add (1);
                continue;
            }

            out = { m.header.kind, m.header.flags, m.body, m.size };
            return true;
        }
    }

    const wire::Reassembler::Statistics& getReassemblyStatistics() const noexcept
    {
        return reassembler.getStatistics();
    }

    /** Datagrams of another session or with the wrong token, ignored. */
    std::uint64_t getForeignCount() const noexcept  { return foreign.load(); }

private:
    juce::DatagramSocket& socket;
    juce::String remoteHost;
    int remotePort;
    Settings settings;

    wire::Fragmenter fragmenter;
    wire::Reassembler reassembler;
    std::vector<std::uint8_t> datagram;
    std::uint32_t nextMessageId = 1;
    std::atomic<std::uint64_t> foreign { 0 };

    JUCE_DECLARE_NON_COPYABLE (UdpTransport)
};

} // namespace fxmenet
