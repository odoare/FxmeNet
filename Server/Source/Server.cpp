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
/** Answers FXMENET_DISCOVER broadcasts with an FXMENET_ANNOUNCE. */
class Server::DiscoveryThread : public juce::Thread
{
public:
    explicit DiscoveryThread (Server& s) : juce::Thread ("FxmeNet discovery"), server (s), socket (true) {}

    bool open (int port)   { return socket.bindToPort (port); }

    void run() override
    {
        char buffer[2048];
        while (! threadShouldExit())
        {
            if (socket.waitUntilReady (true, 200) != 1)
                continue;

            juce::String host;
            int port = 0;
            const int n = socket.read (buffer, (int) sizeof (buffer), false, host, port);
            if (n <= 0 || ! discovery::isDiscover (juce::JSON::parse (juce::String::fromUTF8 (buffer, n))))
                continue;

            discovery::Announce a;
            a.name = server.config.name;
            a.os = juce::SystemStats::getOperatingSystemName();
            a.version = FXMENET_VERSION;
            a.controlPort = server.config.controlPort;
            a.udpPort = server.config.udpPort;
            {
                const std::lock_guard<std::mutex> lock (server.sessionsLock);
                a.sessions = (int) server.sessions.size();
            }
            const auto reply = a.toJson();
            socket.write (host, port, reply.toRawUTF8(), (int) reply.getNumBytesAsUTF8());
        }
    }

private:
    Server& server;
    juce::DatagramSocket socket;
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

    catalog.configure (config.pluginPaths, ! config.useDefaultPaths);
    catalog.scan (config.rescan);

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
    // Above the sessions' audio threads (8): while they process, the
    // socket must still be emptied, or the kernel drops datagrams.
    if (! udpThread->startRealtimeThread (juce::Thread::RealtimeOptions{}.withPriority (9)))
    {
        udpThread->startThread (juce::Thread::Priority::high);
        log ("warning: real-time priority refused for the UDP thread (see the audio threads' warning)");
    }
    if (config.discovery)
    {
        auto d = std::make_unique<DiscoveryThread> (*this);
        if (d->open (config.discoveryPort))
        {
            discoveryThread = std::move (d);
            discoveryThread->startThread();
        }
        else
        {
            log ("warning: cannot bind the discovery port " + juce::String (config.discoveryPort)
                 + " (another server on this machine?): clients must give this server's address");
        }
    }

    startTimer (1000);

    log ("listening: control TCP " + juce::String (config.controlPort)
         + ", audio UDP " + juce::String (config.udpPort)
         + (discoveryThread != nullptr ? ", discovery UDP " + juce::String (config.discoveryPort) : juce::String()));
    return true;
}

void Server::stop()
{
    stopTimer();
    monitors.clear();

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

    if (discoveryThread != nullptr)
    {
        discoveryThread->stopThread (2000);
        discoveryThread.reset();
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

    if (hello.role == control::roleMonitor)
    {
        const int id = nextMonitorId++;
        const bool admin = allowsAdmin (peer);
        auto monitor = std::make_shared<Monitor> (*this, id, connection, admin);
        monitors[id] = monitor;
        monitor->start (helloFrame.requestId);
        log ("monitor " + juce::String (id) + " connected from " + peer + " (" + hello.clientName + ", "
             + hello.os + (admin ? ", may change the plugin folders)" : ", read only)"));
        return;
    }

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
    session->setClient (peer, hello.clientName, hello.os);
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

void Server::removeMonitor (int monitorId)
{
    const auto it = monitors.find (monitorId);
    if (it == monitors.end())
        return;
    // The caller may be that monitor: it is destroyed once its call returns
    // (the caller holds it), not here.
    monitors.erase (it);
    log ("monitor " + juce::String (monitorId) + " disconnected");
}

bool Server::allowsAdmin (const juce::String& host) const
{
    if (config.remoteAdmin)
        return true;
    return host.startsWith ("127.") || host == "::1" || host.startsWith ("::ffff:127.")
        || host.equalsIgnoreCase ("localhost");
}

void Server::route (const std::uint8_t* data, std::size_t size, const juce::String& host, int port)
{
    udpBytesIn.fetch_add (size, std::memory_order_relaxed);
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

    std::vector<std::shared_ptr<Session>> current;
    {
        const std::lock_guard<std::mutex> lock (sessionsLock);
        for (auto& [id, session] : sessions)
            current.push_back (session);
    }

    // Once a second: the load, the traffic and each session's figures.
    load = systemLoad.sample();
    const auto in = udpBytesIn.load();
    if (sampledAt != 0 && now != sampledAt)
        udpInPerSecond = (double) (in - sampledUdpIn) * 1000.0 / (double) (now - sampledAt);
    sampledUdpIn = in;
    sampledAt = now;
    for (auto& s : current)
        s->sampleSecond();

    if (++statsTick % 10 == 0)
    {
        for (auto& s : current)
        {
            const auto line = s->describeActivity();
            if (line.isNotEmpty())
                log (line);
        }
    }
}

//==============================================================================
juce::var Server::describeStatus (std::uint64_t logSince, int knownRevision)
{
    const auto folders = [] (const juce::StringArray& list)
    {
        juce::Array<juce::var> out;
        for (auto& p : list)
        {
            auto* f = new juce::DynamicObject();
            f->setProperty ("path", p);
            f->setProperty ("exists", juce::File::isAbsolutePath (p) && juce::File (p).isDirectory());
            out.add (f);
        }
        return out;
    };

    auto* status = new juce::DynamicObject();

    auto* srv = new juce::DynamicObject();
    srv->setProperty ("name", config.name);
    srv->setProperty ("version", FXMENET_VERSION);
    srv->setProperty ("os", juce::SystemStats::getOperatingSystemName());
    srv->setProperty ("host", juce::SystemStats::getComputerName());
    srv->setProperty ("controlPort", config.controlPort);
    srv->setProperty ("udpPort", config.udpPort);
    srv->setProperty ("discovery", discoveryThread != nullptr);
    srv->setProperty ("seconds", (juce::int64) ((juce::Time::currentTimeMillis() - startedAt) / 1000));
    srv->setProperty ("monitors", (int) monitors.size());
    status->setProperty ("server", srv);

    auto* cpu = new juce::DynamicObject();
    cpu->setProperty ("process", load.processPercent);
    cpu->setProperty ("total", load.totalPercent);
    juce::Array<juce::var> cores;
    for (auto c : load.corePercent)
        cores.add (c);
    cpu->setProperty ("cores", cores);
    cpu->setProperty ("numCores", load.numCores);
    cpu->setProperty ("resident", (juce::int64) load.residentBytes);
    status->setProperty ("load", cpu);

    std::vector<std::shared_ptr<Session>> current;
    {
        const std::lock_guard<std::mutex> lock (sessionsLock);
        for (auto& [id, session] : sessions)
            current.push_back (session);
    }
    double out = 0.0;
    juce::Array<juce::var> list;
    for (auto& s : current)
    {
        auto v = s->describeStatus();
        out += (double) v["outBytesPerSecond"];
        list.add (v);
    }
    auto* net = new juce::DynamicObject();
    net->setProperty ("inBytesPerSecond", udpInPerSecond);
    net->setProperty ("outBytesPerSecond", out);
    status->setProperty ("network", net);
    status->setProperty ("sessions", list);

    const auto progress = catalog.getProgress();
    const auto& paths = catalog.getPaths();
    auto* cat = new juce::DynamicObject();
    cat->setProperty ("revision", catalog.getRevision());
    cat->setProperty ("count", catalog.getAll().size());
    cat->setProperty ("scanning", progress.scanning);
    cat->setProperty ("done", progress.done);
    cat->setProperty ("total", progress.total);
    cat->setProperty ("file", progress.file);
    cat->setProperty ("summary", progress.summary);
    cat->setProperty ("useDefaultPaths", paths.useDefaults);
    cat->setProperty ("defaultPaths", folders (catalog.getDefaultPaths()));
    cat->setProperty ("savedPaths", folders (paths.saved));
    cat->setProperty ("commandLinePaths", folders (paths.commandLine));
    if (knownRevision != catalog.getRevision())
    {
        juce::Array<juce::var> plugins;
        for (auto& e : catalog.getAll())
        {
            const auto& d = e.description;
            auto* p = new juce::DynamicObject();
            p->setProperty ("name", d.name);
            p->setProperty ("vendor", d.manufacturerName);
            p->setProperty ("version", d.version);
            p->setProperty ("category", d.category);
            p->setProperty ("inputs", d.numInputChannels);
            p->setProperty ("outputs", d.numOutputChannels);
            p->setProperty ("fxmenet", e.fxmenetAbi > 0);
            p->setProperty ("file", d.fileOrIdentifier);
            plugins.add (p);
        }
        cat->setProperty ("plugins", plugins);
    }
    status->setProperty ("catalog", cat);

    std::uint64_t next = 0;
    const auto lines = LogRing::instance().since (logSince, 500, next);
    juce::Array<juce::var> logLines;
    for (auto& l : lines)
        logLines.add (l);
    auto* lg = new juce::DynamicObject();
    lg->setProperty ("next", (juce::int64) next);
    lg->setProperty ("lines", logLines);
    status->setProperty ("log", lg);

    return status;
}

} // namespace fxmenet::server
