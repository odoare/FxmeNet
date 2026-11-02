/*
  ------------------------------------------------------------------------------
    Server.h

    fxmenet-server's top level: the plugin catalog, the TCP listener (each
    new connection's HELLO is read on a small thread pool, then handed to
    the message thread, which creates or resumes a session), the shared UDP
    socket and its receive thread (datagrams routed to sessions by id and
    token), and a 1 Hz timer that removes expired orphan sessions and logs
    activity.

    Sessions are created and destroyed on the message thread only. The UDP
    thread reaches them under `sessionsLock`, which removal also takes, so a
    session cannot disappear while a datagram is being handed to it.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include "Log.h"
#include "PluginCatalog.h"
#include "ServerConfig.h"
#include "Session.h"

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

private:
    class AcceptThread;
    class UdpThread;

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

    juce::ThreadPool handshakes { juce::ThreadPoolOptions{}.withNumberOfThreads (2) };
    std::unique_ptr<AcceptThread> acceptThread;
    std::unique_ptr<UdpThread> udpThread;
    int statsTick = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Server)
    JUCE_DECLARE_NON_COPYABLE (Server)
};

} // namespace fxmenet::server
