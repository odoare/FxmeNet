/*
  ------------------------------------------------------------------------------
    Monitor.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Monitor.h"
#include "Log.h"
#include "Server.h"

namespace fxmenet::server
{

using control::Frame;
using control::Type;

//==============================================================================
/** Reads the connection's frames and posts them to the message thread. */
class Monitor::Reader : public juce::Thread
{
public:
    Reader (std::weak_ptr<Monitor> m, std::shared_ptr<control::ControlConnection> c)
        : juce::Thread ("FxmeNet monitor"), monitor (std::move (m)), connection (std::move (c)) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            Frame frame;
            const auto r = connection->read (frame, 200);
            if (r == control::ControlConnection::ReadResult::timeout)
                continue;

            auto weak = monitor;
            if (r == control::ControlConnection::ReadResult::closed)
            {
                juce::MessageManager::callAsync ([weak]
                {
                    if (auto m = weak.lock())
                        m->onClosed();
                });
                return;
            }

            juce::MessageManager::callAsync ([weak, frame]
            {
                if (auto m = weak.lock())
                    m->handleFrame (frame);
            });
        }
    }

private:
    std::weak_ptr<Monitor> monitor;
    std::shared_ptr<control::ControlConnection> connection;
};

//==============================================================================
Monitor::Monitor (Server& owner, int id, std::shared_ptr<control::ControlConnection> c, bool isAdmin)
    : server (owner), monitorId (id), connection (std::move (c)), admin (isAdmin)
{
}

Monitor::~Monitor()
{
    connection->close();   // unblocks the reader
    if (reader != nullptr)
        reader->stopThread (2000);
}

void Monitor::start (std::uint32_t helloRequestId)
{
    control::Welcome w;
    w.udpPort = server.getConfig().udpPort;
    w.serverName = server.getConfig().name;
    w.os = juce::SystemStats::getOperatingSystemName();
    w.capabilities = { control::roleMonitor };
    if (admin)
        w.capabilities.add (control::capabilityAdmin);
    connection->sendJson (Type::welcome, helloRequestId, w.toVar());

    reader = std::make_unique<Reader> (weak_from_this(), connection);
    reader->startThread();
}

void Monitor::onClosed()
{
    server.removeMonitor (monitorId);   // may destroy this: nothing after
}

void Monitor::handleFrame (const Frame& f)
{
    const auto message = f.isJson() ? control::fromPayload (f.payload) : juce::var();

    if (f.type == Type::monitorStatus)
    {
        const auto since = (std::uint64_t) (juce::int64) control::json::get (message, "logSince", (juce::int64) 0);
        const int knownRevision = control::json::get (message, "catalogRevision", -1);
        connection->sendJson (Type::monitorStatus, f.requestId, server.describeStatus (since, knownRevision));
    }
    else if (f.type == Type::monitorPaths || f.type == Type::monitorRescan)
    {
        if (! admin)
        {
            connection->sendError (f.requestId, control::ErrorMessage::notAllowed,
                                   "changing the plugin folders is allowed from the server's machine only "
                                   "(or start the server with --remote-admin)");
            return;
        }

        auto& catalog = server.getCatalog();
        if (f.type == Type::monitorRescan)
        {
            const bool full = control::json::get (message, "full", false);
            log ("fxmenet-top: " + juce::String (full ? "full rescan" : "rescan") + " asked");
            catalog.rescan (full);
        }
        else
        {
            juce::String error;
            if (! catalog.setPaths (control::json::toStringArray (message["paths"]),
                                    control::json::get (message, "useDefaultPaths", true), error))
            {
                connection->sendError (f.requestId, control::ErrorMessage::badRequest, error);
                return;
            }
        }
        connection->send (f.type, 0, f.requestId, nullptr, 0);   // acknowledged: the scan runs
    }
    else if (f.type == Type::ping)
    {
        connection->send (Type::pong, 0, f.requestId, f.payload.data(), f.payload.size());
    }
    else if (f.type == Type::bye)
    {
        connection->close();   // the reader sees it and reports the close
    }
    else
    {
        connection->sendError (f.requestId, control::ErrorMessage::unsupported, "not available to a monitor");
    }
}

} // namespace fxmenet::server
