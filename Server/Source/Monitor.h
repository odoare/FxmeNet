/*
  ------------------------------------------------------------------------------
    Monitor.h

    A monitor connection: fxmenet-top (Tools/Top), which said HELLO with the
    role "monitor". It opens no session and never touches the audio; it asks
    for the server's status (MONITOR_STATUS, about once a second) and, when
    allowed (WELCOME's "admin" capability: it connected from this machine,
    or the server runs with --remote-admin), changes the plugin folders
    (MONITOR_PATHS) or rescans (MONITOR_RESCAN). protocol.md section 4.7.

    Like a session's, its reader thread posts each frame to the message
    thread, where it is answered.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeNet/control/ControlConnection.h>

#include <memory>

namespace fxmenet::server
{

class Server;

class Monitor : public std::enable_shared_from_this<Monitor>
{
public:
    Monitor (Server& owner, int id, std::shared_ptr<control::ControlConnection> connection, bool admin);
    ~Monitor();

    /** Answers the HELLO and starts reading. Message thread. */
    void start (std::uint32_t helloRequestId);

    int getId() const noexcept   { return monitorId; }

private:
    class Reader;

    void handleFrame (const control::Frame&);
    void onClosed();

    Server& server;
    const int monitorId;
    std::shared_ptr<control::ControlConnection> connection;
    const bool admin;
    std::unique_ptr<Reader> reader;

    JUCE_DECLARE_NON_COPYABLE (Monitor)
};

} // namespace fxmenet::server
