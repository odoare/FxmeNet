/*
  ------------------------------------------------------------------------------
    Session.h

    One client's session (protocol.md): its control connection, its chain
    and its audio worker.

    Threads:
      - a reader thread per connection reads control frames and posts each
        to the message thread, where handleFrame() acts on it (plugin
        instances are only ever touched there);
      - the server's UDP thread calls onDatagram() (reassembly, then the
        worker's queue);
      - the worker's audio thread processes and replies.

    When the control connection drops the session becomes an orphan: the
    server keeps it for a grace period, so the client can resume it
    (protocol.md section 8), then destroys it.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include "AudioWorker.h"
#include "Chain.h"
#include "PluginCatalog.h"

#include <FxmeNet/control/ControlConnection.h>
#include <FxmeNet/protocol/Fragmentation.h>

#include <memory>
#include <mutex>

namespace fxmenet::server
{

class Server;

class Session : public std::enable_shared_from_this<Session>,
                private juce::Timer
{
public:
    Session (Server& owner, std::uint32_t id, std::uint64_t token,
             PluginCatalog& catalog, const juce::DatagramSocket& udpSocket);
    ~Session();

    std::uint32_t getId() const noexcept        { return sessionId; }
    std::uint64_t getToken() const noexcept     { return sessionToken; }

    //==============================================================================
    // Message thread.

    /** Takes a (new or resumed) control connection and starts reading it. */
    void attach (std::shared_ptr<control::ControlConnection> connection);

    void sendWelcome (std::uint32_t requestId, bool resumed);

    /** True once the control connection is gone; since when (ms counter). */
    bool isOrphan() const noexcept              { return connection == nullptr; }
    juce::uint32 getOrphanedSince() const noexcept  { return orphanedSince; }

    /** One line of statistics for the log, or {} when nothing happened. */
    juce::String describeActivity();

    //==============================================================================
    // The server's UDP thread.

    /** A datagram of this session (id and token already checked). */
    void onDatagram (const std::uint8_t* data, std::size_t size,
                     const juce::String& senderHost, int senderPort);

private:
    class Reader;

    void handleFrame (const control::Frame&, int generation);
    void onConnectionClosed (int generation);
    void detach();

    /** The session's timer, at its telemetry rate: one telemetry frame per
        slot whose editor is open, and the parameters plugins changed. */
    void timerCallback() override;
    void pumpTelemetry();
    void flushParamChanges();

    void handleConfigure (const control::Frame&);
    void handleTelemetryActive (const control::Frame&);
    void handleSlotAdd (const control::Frame&);
    void handleParamSet (const control::Frame&);
    void handleState (const control::Frame&);

    void ack (const control::Frame&);
    void fail (const control::Frame&, int code, const juce::String& text);

    Server& server;
    const std::uint32_t sessionId;
    const std::uint64_t sessionToken;
    PluginCatalog& catalog;

    Chain chain;
    AudioWorker worker;
    control::Configure config;
    bool configured = false;

    std::shared_ptr<control::ControlConnection> connection;
    std::unique_ptr<Reader> reader;
    int generation = 0;   ///< which connection a posted frame came from
    juce::uint32 orphanedSince = 0;

    std::mutex udpLock;   ///< the reassembler, between the UDP thread and a reconfiguration
    std::unique_ptr<wire::Reassembler> reassembler;

    std::uint64_t lastProcessed = 0;

    // Telemetry and parameter changes, message thread.
    const juce::DatagramSocket& udp;
    wire::Fragmenter telemetryFragmenter;
    std::uint32_t telemetryMessageId = 1;
    std::vector<std::uint8_t> telemetryFrame, telemetryBody;
    std::vector<control::ParamChange> paramChanges;
    std::uint64_t telemetrySent = 0, paramChangesSent = 0;

    JUCE_DECLARE_NON_COPYABLE (Session)
};

} // namespace fxmenet::server
