/*
  ------------------------------------------------------------------------------
    Server.h

    fxmenet-server's top level: the plugin catalog, the TCP listener (each
    new connection's HELLO is read on a small thread pool, then handed to
    the message thread, which creates or resumes a session), the shared UDP
    socket and its receive thread (datagrams routed to sessions by id and
    token), and a 1 Hz timer that removes expired orphan sessions, samples
    the sessions and the machine's load, and logs activity every 10 s.

    A HELLO with the role "monitor" (fxmenet-top) opens no session: it gets
    a Monitor, answered from describeStatus().

    Sessions are created and destroyed on the message thread only. The UDP
    thread reaches them under `sessionsLock`, which removal also takes, so a
    session cannot disappear while a datagram is being handed to it.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include "Log.h"
#include "Monitor.h"
#include "PluginCatalog.h"
#include "ServerConfig.h"
#include "Session.h"
#include "SystemLoad.h"

#include <FxmeNet/control/Discovery.h>

#include <map>
#include <memory>
#include <mutex>

namespace fxmenet::server
{

class Server : private juce::Timer
{
public:
    explicit Server (const ServerConfig&);
    ~Server() override;

    /** Scans the plugins, opens the sockets, starts the threads. Message thread. */
    bool start();

    /** Closes everything and destroys the sessions. Message thread. */
    void stop();

    const ServerConfig& getConfig() const noexcept   { return config; }

    /** Message thread: a connection said HELLO. */
    void onHello (std::shared_ptr<control::ControlConnection>, const control::Frame& hello);

    /** Message thread: forgets a session (BYE, or an expired orphan). */
    void removeSession (std::uint32_t sessionId);

    /** Message thread: a monitor's connection closed. */
    void removeMonitor (int monitorId);

    /** Message thread: everything fxmenet-top shows, as JSON (protocol.md
        section 4.7). The log lines numbered `logSince` and after; the
        plugin list only when the catalog's revision is not `knownRevision`. */
    juce::var describeStatus (std::uint64_t logSince, int knownRevision);

    PluginCatalog& getCatalog() noexcept   { return catalog; }

private:
    class AcceptThread;
    class UdpThread;
    class DiscoveryThread;

    void timerCallback() override;

    /** UDP thread: hands a datagram to its session, if id and token match. */
    void route (const std::uint8_t* data, std::size_t size, const juce::String& host, int port);

    ServerConfig config;
    PluginCatalog catalog;
    juce::StreamingSocket listener;
    juce::DatagramSocket udp;

    std::mutex sessionsLock;
    std::map<std::uint32_t, std::shared_ptr<Session>> sessions;
    std::uint32_t nextSessionId = 1;

    // Monitors, message thread.
    std::map<int, std::shared_ptr<Monitor>> monitors;
    int nextMonitorId = 1;

    /** True when a monitor connected from `host` may change the plugin folders. */
    bool allowsAdmin (const juce::String& host) const;

    // Load and traffic, sampled by the timer.
    SystemLoad systemLoad;
    SystemLoad::Sample load;
    std::atomic<std::uint64_t> udpBytesIn { 0 };   ///< every datagram, UDP thread
    std::uint64_t sampledUdpIn = 0;
    double udpInPerSecond = 0.0;
    juce::uint32 sampledAt = 0;
    const juce::int64 startedAt = juce::Time::currentTimeMillis();

    juce::ThreadPool handshakes { juce::ThreadPoolOptions{}.withNumberOfThreads (2) };
    std::unique_ptr<AcceptThread> acceptThread;
    std::unique_ptr<UdpThread> udpThread;
    std::unique_ptr<DiscoveryThread> discoveryThread;
    int statsTick = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Server)
    JUCE_DECLARE_NON_COPYABLE (Server)
};

} // namespace fxmenet::server
