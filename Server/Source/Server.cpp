/*
  ------------------------------------------------------------------------------
    Server.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Server.h"

namespace fxmenet::server
{

using control::ControlConnection;
using control::Frame;
using control::Type;

//==============================================================================
/** Accepts control connections; each one's HELLO is read on the pool. */
class Server::AcceptThread : public juce::Thread
{
public:
    explicit AcceptThread (Server& s) : juce::Thread ("FxmeNet accept"), server (s) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            std::unique_ptr<juce::StreamingSocket> socket (server.listener.waitForNextConnection());
            if (socket == nullptr)
                continue;   // the listener was closed, or a failed accept

            auto connection = std::make_shared<ControlConnection> (std::move (socket));
            juce::WeakReference<Server> weak (&server);

            server.handshakes.addJob ([connection, weak]
            {
                Frame hello;
                const auto r = connection->read (hello, 5000);
                if (r != ControlConnection::ReadResult::frame || hello.type != Type::hello)
                {
                    connection->close();
                    return;
                }
                juce::MessageManager::callAsync ([connection, weak, hello]
                {
                    if (auto* s = weak.get())
                        s->onHello (connection, hello);
                });
            });
        }
    }

private:
    Server& server;
};

//==============================================================================
/** Receives every datagram of every session on the shared socket. */
class Server::UdpThread : public juce::Thread
{
public:
    explicit UdpThread (Server& s) : juce::Thread ("FxmeNet UDP"), server (s) {}

    void run() override
    {
        std::vector<std::uint8_t> buffer (65536);
        juce::String host;
        int port = 0;

        while (! threadShouldExit())
        {
            if (server.udp.waitUntilReady (true, 100) != 1)
                continue;
            const int n = server.udp.read (buffer.data(), (int) buffer.size(), false, host, port);
            if (n > 0)
                server.route (buffer.data(), (std::size_t) n, host, port);
        }
    }

private:
    Server& server;
};

//==============================================================================
Server::Server (const ServerConfig& c) : config (c) {}

Server::~Server()
{
    stop();
}

bool Server::start()
{
    log ("fxmenet-server " FXMENET_VERSION " \"" + config.name + "\" on "
         + juce::SystemStats::getOperatingSystemName());

    catalog.scan (config.pluginPaths, config.useDefaultPaths);

    if (! listener.createListener (config.controlPort))
    {
        log ("error: cannot listen on TCP port " + juce::String (config.controlPort));
        return false;
    }
    if (! udp.bindToPort (config.udpPort))
    {
        log ("error: cannot bind UDP port " + juce::String (config.udpPort));
        return false;
    }

    acceptThread = std::make_unique<AcceptThread> (*this);
    acceptThread->startThread();
    udpThread = std::make_unique<UdpThread> (*this);
    udpThread->startThread (juce::Thread::Priority::high);
    startTimer (1000);

    log ("listening: control TCP " + juce::String (config.controlPort)
         + ", audio UDP " + juce::String (config.udpPort));
    return true;
}

void Server::stop()
{
    stopTimer();

    if (acceptThread != nullptr)
    {
        acceptThread->signalThreadShouldExit();
        listener.close();   // unblocks waitForNextConnection
        acceptThread->stopThread (2000);
        acceptThread.reset();
    }
    handshakes.removeAllJobs (true, 2000);

    if (udpThread != nullptr)
    {
        udpThread->stopThread (2000);
        udpThread.reset();
    }

    std::map<std::uint32_t, std::shared_ptr<Session>> closing;
    {
        const std::lock_guard<std::mutex> lock (sessionsLock);
        closing.swap (sessions);
    }
    closing.clear();   // destroyed here, on the message thread
}

//==============================================================================
void Server::onHello (std::shared_ptr<ControlConnection> connection, const Frame& helloFrame)
{
    const auto hello = control::Hello::fromVar (control::fromPayload (helloFrame.payload));
    const auto peer = connection->getPeerHost();

    if (! hello.versions.contains (1))
    {
        control::Welcome refusal;
        refusal.error = "protocol version 1 is the only one this server speaks";
        connection->sendJson (Type::welcome, helloFrame.requestId, refusal.toVar());
        connection->close();
        return;
    }

    if (hello.resume)
    {
        std::shared_ptr<Session> orphan;
        {
            const std::lock_guard<std::mutex> lock (sessionsLock);
            const auto it = sessions.find (hello.resumeSessionId);
            if (it != sessions.end() && it->second->getToken() == hello.resumeToken && it->second->isOrphan())
                orphan = it->second;
        }

        if (orphan != nullptr)
        {
            orphan->attach (connection);
            orphan->sendWelcome (helloFrame.requestId, true);
            log ("session " + juce::String ((int) orphan->getId()) + " resumed by " + peer);
            return;
        }
    }

    const auto id = nextSessionId++;
    auto token = (std::uint64_t) juce::Random::getSystemRandom().nextInt64();
    if (token == 0)
        token = 1;

    auto session = std::make_shared<Session> (*this, id, token, catalog, udp);
    {
        const std::lock_guard<std::mutex> lock (sessionsLock);
        sessions[id] = session;
    }
    session->attach (connection);
    session->sendWelcome (helloFrame.requestId, false);
    log ("session " + juce::String ((int) id) + " opened by " + peer + " (" + hello.clientName
         + ", " + hello.os + ")");
}

void Server::removeSession (std::uint32_t sessionId)
{
    std::shared_ptr<Session> removed;
    {
        const std::lock_guard<std::mutex> lock (sessionsLock);
        const auto it = sessions.find (sessionId);
        if (it == sessions.end())
            return;
        removed = std::move (it->second);
        sessions.erase (it);
    }
    // `removed` may be the last owner: the session is destroyed here, on the
    // message thread, out of the lock (its destructor stops threads).
}

void Server::route (const std::uint8_t* data, std::size_t size, const juce::String& host, int port)
{
    wire::ByteReader r (data, size);
    wire::DatagramHeader h;
    if (size < wire::headerSize || ! wire::readHeader (r, h))
        return;

    const std::lock_guard<std::mutex> lock (sessionsLock);
    const auto it = sessions.find (h.sessionId);
    if (it != sessions.end() && it->second->getToken() == h.token)
        it->second->onDatagram (data, size, host, port);
}

void Server::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const auto grace = (juce::uint32) config.orphanGraceSeconds * 1000u;

    std::vector<std::uint32_t> expired;
    juce::StringArray activity;
    {
        const std::lock_guard<std::mutex> lock (sessionsLock);
        for (auto& [id, session] : sessions)
            if (session->isOrphan() && now - session->getOrphanedSince() > grace)
                expired.push_back (id);
    }
    for (auto id : expired)
        removeSession (id);

    if (++statsTick % 10 == 0)
    {
        std::vector<std::shared_ptr<Session>> current;
        {
            const std::lock_guard<std::mutex> lock (sessionsLock);
            for (auto& [id, session] : sessions)
                current.push_back (session);
        }
        for (auto& s : current)
        {
            const auto line = s->describeActivity();
            if (line.isNotEmpty())
                log (line);
        }
    }
}

} // namespace fxmenet::server
