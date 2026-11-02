/*
  ------------------------------------------------------------------------------
    UdpPeer.h

    A client's UDP address, resolved once, and a thread-safe send to it on
    the server's shared socket.

    Why not juce::DatagramSocket::write: it caches only the last address it
    resolved, in members, without a lock. With several sessions replying to
    different clients from their own audio threads it would race, and call
    getaddrinfo on nearly every datagram. Here each session resolves its
    peer once (a numeric address, no DNS) and sends with ::sendto on the raw
    handle, which the OS makes safe from several threads. Replies still leave
    from the server's port, so a stateful firewall on the client lets them in.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
 #include <winsock2.h>
 #include <ws2tcpip.h>
#else
 #include <netdb.h>
 #include <netinet/in.h>
 #include <sys/socket.h>
 #include <sys/types.h>
#endif

#include <cstring>

namespace fxmenet::server
{

class UdpPeer
{
public:
    /** Resolves a numeric address ("192.168.1.20", "::1") and port. */
    bool resolve (const juce::String& numericHost, int port)
    {
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

        addrinfo* info = nullptr;
        if (getaddrinfo (numericHost.toRawUTF8(), juce::String (port).toRawUTF8(), &hints, &info) != 0
            || info == nullptr)
            return false;

        std::memcpy (&address, info->ai_addr, (std::size_t) info->ai_addrlen);
        length = (int) info->ai_addrlen;
        freeaddrinfo (info);
        host = numericHost;
        portNumber = port;
        return true;
    }

    bool isValid() const noexcept   { return length > 0; }

    /** Sends one datagram. Any thread; no allocation. */
    bool send (const juce::DatagramSocket& socket, const void* data, std::size_t size) const
    {
       #if JUCE_WINDOWS
        const auto handle = (SOCKET) socket.getRawSocketHandle();
        return ::sendto (handle, (const char*) data, (int) size, 0,
                         (const sockaddr*) &address, length) == (int) size;
       #else
        const auto sent = ::sendto (socket.getRawSocketHandle(), data, size, 0,
                                    (const sockaddr*) &address, (socklen_t) length);
        return sent == (ssize_t) size;
       #endif
    }

    juce::String getHost() const    { return host; }
    int getPort() const noexcept    { return portNumber; }

private:
    sockaddr_storage address {};
    int length = 0;
    juce::String host;
    int portNumber = 0;
};

} // namespace fxmenet::server
