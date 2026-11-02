/*
  ------------------------------------------------------------------------------
    Discovery.h

    Finding servers on the LAN (protocol.md section 7), broadcast part. A
    client sends FXMENET_DISCOVER to the broadcast address on the discovery
    port; every server answers by unicast with FXMENET_ANNOUNCE. Both are
    small JSON datagrams. mDNS / DNS-SD is the planned second way, and a
    manual address always works.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <vector>

namespace fxmenet::discovery
{

constexpr int defaultPort = 54320;

inline juce::String discoverMessage()
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("cmd", "FXMENET_DISCOVER");
    o->setProperty ("version", 1);
    return juce::JSON::toString (juce::var (o), true);
}

inline bool isDiscover (const juce::var& v)
{
    return v["cmd"].toString() == "FXMENET_DISCOVER";
}

struct Announce
{
    juce::String name, os, version;
    juce::String host;          ///< filled by the receiver from the datagram's sender
    int controlPort = 54322;
    int udpPort = 54321;
    int sessions = 0;

    juce::String toJson() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("cmd", "FXMENET_ANNOUNCE");
        o->setProperty ("name", name);
        o->setProperty ("os", os);
        o->setProperty ("version", version);
        o->setProperty ("controlPort", controlPort);
        o->setProperty ("udpPort", udpPort);
        o->setProperty ("sessions", sessions);
        return juce::JSON::toString (juce::var (o), true);
    }

    /** False when `v` is not an announce. */
    static bool fromVar (const juce::var& v, Announce& a)
    {
        if (v["cmd"].toString() != "FXMENET_ANNOUNCE")
            return false;
        a.name = v["name"].toString();
        a.os = v["os"].toString();
        a.version = v["version"].toString();
        a.controlPort = v.hasProperty ("controlPort") ? (int) v["controlPort"] : a.controlPort;
        a.udpPort = v.hasProperty ("udpPort") ? (int) v["udpPort"] : a.udpPort;
        a.sessions = (int) v["sessions"];
        return true;
    }
};

/** Broadcasts a discover and collects the answers for `waitMs`. Blocks the
    calling thread (not an audio or message thread). */
inline std::vector<Announce> discover (int waitMs, int port = defaultPort)
{
    std::vector<Announce> found;
    juce::DatagramSocket socket (true);
    if (! socket.bindToPort (0))
        return found;

    const auto message = discoverMessage();
    socket.write ("255.255.255.255", port, message.toRawUTF8(), (int) message.getNumBytesAsUTF8());

    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) waitMs;
    char buffer[2048];
    for (;;)
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (now >= deadline || socket.waitUntilReady (true, (int) (deadline - now)) != 1)
            break;

        juce::String host;
        int senderPort = 0;
        const int n = socket.read (buffer, (int) sizeof (buffer), false, host, senderPort);
        Announce a;
        if (n > 0 && Announce::fromVar (juce::JSON::parse (juce::String::fromUTF8 (buffer, n)), a))
        {
            a.host = host;
            const bool duplicate = std::any_of (found.begin(), found.end(), [&] (const Announce& b)
                                                { return b.host == a.host && b.controlPort == a.controlPort; });
            if (! duplicate)
                found.push_back (a);
        }
    }
    return found;
}

} // namespace fxmenet::discovery
