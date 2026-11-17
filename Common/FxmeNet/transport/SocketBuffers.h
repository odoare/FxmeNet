/*
  ------------------------------------------------------------------------------
    SocketBuffers.h

    Larger send and receive buffers for a UDP socket (todo item 21). The
    server sends every session's replies from one socket; with the system's
    default send buffer (about 208 KB on Linux) a burst of replies under
    load fills it and the kernel drops datagrams, each one a concealed block
    on a client.

    The system may grant less than asked:
      - Linux caps the size at net.core.wmem_max / rmem_max (unless the
        process may force it, SO_SNDBUFFORCE / SO_RCVBUFFORCE, as root or
        with CAP_NET_ADMIN), silently; getsockopt then reports twice the
        size kept (the kernel's bookkeeping), halved here;
      - macOS refuses a size above kern.ipc.maxsockbuf: the size is halved
        until it is accepted;
      - Windows takes what it is given.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
 #include <winsock2.h>
#else
 #include <sys/socket.h>
 #include <sys/types.h>
#endif

namespace fxmenet::transport
{

struct SocketBufferSizes
{
    int send = 0, receive = 0;   ///< bytes, as the system kept them (0: unknown)
};

namespace detail
{
    inline int getBuffer (juce::DatagramSocket& socket, int option)
    {
        int value = 0;
       #if JUCE_WINDOWS
        int size = sizeof (value);
        if (::getsockopt ((SOCKET) socket.getRawSocketHandle(), SOL_SOCKET, option, (char*) &value, &size) != 0)
            return 0;
       #else
        socklen_t size = sizeof (value);
        if (::getsockopt (socket.getRawSocketHandle(), SOL_SOCKET, option, &value, &size) != 0)
            return 0;
       #endif
       #if JUCE_LINUX || JUCE_ANDROID
        value /= 2;   // Linux reports the size kept doubled
       #endif
        return value;
    }

    inline bool setBuffer (juce::DatagramSocket& socket, int option, int bytes)
    {
       #if JUCE_WINDOWS
        return ::setsockopt ((SOCKET) socket.getRawSocketHandle(), SOL_SOCKET, option, (const char*) &bytes, sizeof (bytes)) == 0;
       #else
        return ::setsockopt (socket.getRawSocketHandle(), SOL_SOCKET, option, &bytes, sizeof (bytes)) == 0;
       #endif
    }

    /** Asks for `bytes` (never less than the current size), halving on a
        refusal; returns the size kept. */
    inline int growBuffer (juce::DatagramSocket& socket, int option, int forceOption, int bytes)
    {
        const int current = getBuffer (socket, option);
        if (current >= bytes)
            return current;
        juce::ignoreUnused (forceOption);
       #if JUCE_LINUX || JUCE_ANDROID
        if (setBuffer (socket, forceOption, bytes))   // past the system's cap, when allowed
            return getBuffer (socket, option);
       #endif
        for (int b = bytes; b > current; b /= 2)
            if (setBuffer (socket, option, b))
                break;
        return getBuffer (socket, option);
    }
}

/** Grows a bound socket's buffers to the sizes asked, as far as the
    system allows; returns the sizes it kept. */
inline SocketBufferSizes growSocketBuffers (juce::DatagramSocket& socket, int sendBytes, int receiveBytes)
{
    SocketBufferSizes got;
    if (socket.getRawSocketHandle() < 0)
        return got;
   #if JUCE_LINUX || JUCE_ANDROID
    got.send = detail::growBuffer (socket, SO_SNDBUF, SO_SNDBUFFORCE, sendBytes);
    got.receive = detail::growBuffer (socket, SO_RCVBUF, SO_RCVBUFFORCE, receiveBytes);
   #else
    got.send = detail::growBuffer (socket, SO_SNDBUF, 0, sendBytes);
    got.receive = detail::growBuffer (socket, SO_RCVBUF, 0, receiveBytes);
   #endif
    return got;
}

} // namespace fxmenet::transport
