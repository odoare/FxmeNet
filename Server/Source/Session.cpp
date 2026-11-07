/*
  ------------------------------------------------------------------------------
    Session.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Session.h"
#include "Log.h"
#include "Server.h"

namespace fxmenet::server
{

using control::ErrorMessage;
using control::Frame;
using control::Type;

//==============================================================================
/** Reads one connection's frames and posts them to the message thread. */
class Session::Reader : public juce::Thread
{
public:
    Reader (std::weak_ptr<Session> s, std::shared_ptr<control::ControlConnection> c, int gen)
        : juce::Thread ("FxmeNet control"), session (std::move (s)), connection (std::move (c)), generation (gen) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            Frame frame;
            const auto r = connection->read (frame, 200);
            if (r == control::ControlConnection::ReadResult::timeout)
                continue;

            auto weak = session;
            const int gen = generation;
            if (r == control::ControlConnection::ReadResult::closed)
            {
                juce::MessageManager::callAsync ([weak, gen]
                {
                    if (auto s = weak.lock())
                        s->onConnectionClosed (gen);
                });
                return;
            }

            juce::MessageManager::callAsync ([weak, gen, frame]
            {
                if (auto s = weak.lock())
                    s->handleFrame (frame, gen);
            });
        }
    }

private:
    std::weak_ptr<Session> session;
    std::shared_ptr<control::ControlConnection> connection;
    const int generation;
};

//==============================================================================
Session::Session (Server& owner, std::uint32_t id, std::uint64_t token,
                  PluginCatalog& pluginCatalog, const juce::DatagramSocket& udpSocket)
    : server (owner), sessionId (id), sessionToken (token), catalog (pluginCatalog),
      worker (chain, udpSocket), udp (udpSocket)
{
    telemetryFrame.resize (1 << 16);
    telemetryBody.reserve (1 << 16);
    startTimerHz (config.telemetryHz);
}

Session::~Session()
{
    stopTimer();
    detach();
    worker.stop();
    chain.clear();   // instances destroyed here, on the message thread
    log ("session " + juce::String ((int) sessionId) + " closed");
}

void Session::attach (std::shared_ptr<control::ControlConnection> newConnection)
{
    detach();
    connection = std::move (newConnection);
    reader = std::make_unique<Reader> (weak_from_this(), connection, ++generation);
    reader->startThread();
}

void Session::detach()
{
    if (connection != nullptr)
        connection->close();   // unblocks the reader
    if (reader != nullptr)
        reader->stopThread (2000);
    reader.reset();
    connection.reset();
    orphanedSince = juce::Time::getMillisecondCounter();
}

void Session::onConnectionClosed (int gen)
{
    if (gen != generation || connection == nullptr)
        return;   // an older connection, already replaced
    log ("session " + juce::String ((int) sessionId) + ": control connection lost, kept "
         + juce::String (server.getConfig().orphanGraceSeconds) + " s for a resume");
    detach();
}

void Session::sendWelcome (std::uint32_t requestId, bool resumed)
{
    control::Welcome w;
    w.sessionId = sessionId;
    w.token = sessionToken;
    w.udpPort = server.getConfig().udpPort;
    w.serverName = server.getConfig().name;
    w.os = juce::SystemStats::getOperatingSystemName();
    w.resumed = resumed;
    w.capabilities.add (control::capabilityRouting);
    if (connection != nullptr)
        connection->sendJson (Type::welcome, requestId, w.toVar());
}

juce::String Session::describeActivity()
{
    const auto& s = worker.getStatistics();
    const auto processed = s.processed.load();
    if (processed == lastProcessed)
        return {};
    lastProcessed = processed;

    const auto window = worker.takeWindow();
    const auto count = [] (const std::atomic<std::uint64_t>& a) { return juce::String ((juce::int64) a.load()); };
    const auto micros = [] (std::uint32_t us) { return juce::String ((juce::int64) us) + " us"; };

    // Counts are since the session started; "10 s" values are over the
    // last line's interval. Turnaround: request arrival to reply sent.
    return "session " + juce::String ((int) sessionId)
         + ": " + juce::String ((juce::int64) processed) + " blocks"
         + ", lost " + count (s.lostBlocks)
         + ", stale " + count (s.stale)
         + ", from cache " + count (s.fromCache)
         + ", queue full " + count (s.queueFull)
         + ", send errors " + count (s.sendErrors)
         + ", resets " + count (s.resets)
         + " | process " + micros (s.lastProcessMicros.load())
         + ", max 10 s " + micros (window.maxProcessMicros)
         + ", max " + micros (s.maxProcessMicros.load())
         + " | turnaround max 10 s " + micros (window.maxTurnaroundMicros)
         + ", over 1 block " + count (s.overOneBlock)
         + ", over 2 blocks " + count (s.overTwoBlocks)
         + " | chain latency " + juce::String (chain.getLatency())
         + ", telemetry frames " + juce::String ((juce::int64) telemetrySent)
         + ", parameter changes " + juce::String ((juce::int64) paramChangesSent);
}

//==============================================================================
void Session::timerCallback()
{
    chain.refreshLatencies();
    pumpTelemetry();
    flushParamChanges();
}

void Session::pumpTelemetry()
{
    const auto* peer = worker.getPeer();
    if (peer == nullptr)
        return;   // no datagram from the client yet: nowhere to send

    chain.forEachSlot ([this, peer] (Chain::Slot& s)
    {
        if (! s.telemetryActive || s.net == nullptr)
            return;

        int n = s.net->readTelemetry (telemetryFrame.data(), (std::int32_t) telemetryFrame.size());
        if (n < 0)
        {
            telemetryFrame.resize ((std::size_t) -n);   // the frame is kept by the plugin until read
            n = s.net->readTelemetry (telemetryFrame.data(), (std::int32_t) telemetryFrame.size());
        }
        if (n <= 0)
            return;

        telemetryBody.clear();
        wire::ByteWriter w (telemetryBody);
        wire::writeTelemetry (w, s.slotId, s.telemetryFrame++, telemetryFrame.data(), (std::size_t) n);

        wire::DatagramHeader h;
        h.kind = wire::Kind::telemetry;
        h.sessionId = sessionId;
        h.token = sessionToken;
        h.messageId = telemetryMessageId++;
        telemetryFragmenter.split (h, telemetryBody.data(), telemetryBody.size(),
                                   [this, peer] (const std::uint8_t* d, std::size_t size)
                                   {
                                       return peer->send (udp, d, size);
                                   });
        ++telemetrySent;
    });
}

void Session::flushParamChanges()
{
    if (connection == nullptr)
        return;

    paramChanges.clear();
    chain.forEachSlot ([this] (Chain::Slot& s)
    {
        if (s.watcher != nullptr)
            s.watcher->collect (s.slotId, paramChanges);
    });

    if (! paramChanges.empty())
    {
        connection->sendBinary (Type::paramChanged, 0, encode (paramChanges));
        paramChangesSent += paramChanges.size();
    }
}

//==============================================================================
void Session::onDatagram (const std::uint8_t* data, std::size_t size,
                          const juce::String& senderHost, int senderPort)
{
    const std::lock_guard<std::mutex> lock (udpLock);
    if (reassembler == nullptr)
        return;   // not configured yet

    wire::Reassembler::Message m;
    if (! reassembler->add (data, size, m))
        return;

    worker.updatePeer (senderHost, senderPort);

    if (m.header.kind == wire::Kind::audioRequest)
        worker.enqueue (m.header.flags, m.body, m.size);
}

//==============================================================================
void Session::handleFrame (const Frame& f, int gen)
{
    if (gen != generation || connection == nullptr)
        return;

    switch (f.type)
    {
        case Type::configure:   handleConfigure (f); break;
        case Type::slotAdd:     handleSlotAdd (f); break;
        case Type::paramSet:    handleParamSet (f); break;

        case Type::stateSet:
        case Type::stateGet:    handleState (f); break;

        case Type::ping:
            connection->send (Type::pong, 0, f.requestId, f.payload.data(), f.payload.size());
            break;

        case Type::listPlugins:
            connection->sendJson (Type::pluginList, f.requestId,
                                  control::pluginListToVar (catalog.getEntries()));
            break;

        case Type::slotRemove:
        case Type::slotMove:
        case Type::slotBypass:
        {
            control::SlotValue v;
            if (! decode (f.payload, v))
                return fail (f, ErrorMessage::badRequest, "malformed slot message");

            const bool ok = f.type == Type::slotRemove ? chain.removeSlot (v.slotId)
                          : f.type == Type::slotMove   ? chain.moveSlot (v.slotId, (int) v.value)
                                                       : chain.setBypass (v.slotId, v.value != 0);
            if (! ok)
                return fail (f, ErrorMessage::noSuchSlot, "no slot " + juce::String ((int) v.slotId));
            ack (f);
            break;
        }

        case Type::chainClear:
            chain.clear();
            ack (f);
            break;

        case Type::routing:
            handleRouting (f);
            break;

        case Type::telemetryActive:
            handleTelemetryActive (f);
            break;

        case Type::bye:
            log ("session " + juce::String ((int) sessionId) + ": bye");
            server.removeSession (sessionId);   // destroys this session: nothing after this line
            return;

        case Type::hello:
        case Type::welcome:
        case Type::pong:
        case Type::error:
        case Type::pluginList:
        case Type::slotReady:
        case Type::paramChanged:
        case Type::state:
        default:
            fail (f, ErrorMessage::unsupported, "unexpected message type " + juce::String ((int) f.type));
            break;
    }
}

void Session::handleConfigure (const Frame& f)
{
    const auto c = control::Configure::fromVar (control::fromPayload (f.payload));
    if (c.sampleRate < 8000.0 || c.sampleRate > 384000.0
        || c.blockSize < 16 || c.blockSize > 4096
        || c.numInputs < 1 || c.numInputs > wire::maxChannels
        || c.numOutputs < 1 || c.numOutputs > wire::maxChannels
        || c.datagramBudget < 512 || c.datagramBudget > 65000
        || c.telemetryHz < 1 || c.telemetryHz > 120)
        return fail (f, ErrorMessage::badRequest, "configuration out of range");

    {
        // No datagram reaches the worker while it is stopped and resized.
        const std::lock_guard<std::mutex> lock (udpLock);
        reassembler.reset();
    }
    worker.stop();
    chain.prepare (c.sampleRate, c.blockSize, c.numInputs, c.numOutputs);
    config = c;
    configured = true;
    worker.start (config, sessionId, sessionToken);
    {
        const std::lock_guard<std::mutex> lock (udpLock);
        reassembler = std::make_unique<wire::Reassembler> ((std::size_t) c.datagramBudget,
                                                           AudioWorker::maxRequestSize (c));
    }
    telemetryFragmenter = wire::Fragmenter ((std::size_t) config.datagramBudget);
    startTimerHz (config.telemetryHz);

    log ("session " + juce::String ((int) sessionId) + ": " + juce::String (c.sampleRate) + " Hz, block "
         + juce::String (c.blockSize) + ", " + juce::String (c.numInputs) + " in / "
         + juce::String (c.numOutputs) + " out"
         + (worker.isRealtime() ? ", real-time audio thread" : ""));
    if (! worker.isRealtime())
        log ("session " + juce::String ((int) sessionId) + ": warning: real-time priority refused, "
             "the audio thread runs as an ordinary thread (on Linux: give the user an rtprio limit, "
             "for example @audio - rtprio 95 in /etc/security/limits.d/audio.conf)");
    ack (f);
}

void Session::handleTelemetryActive (const Frame& f)
{
    control::SlotValue v;
    if (! decode (f.payload, v))
        return fail (f, ErrorMessage::badRequest, "malformed TELEMETRY_ACTIVE");

    auto* slot = chain.findSlot (v.slotId);
    if (slot == nullptr)
        return fail (f, ErrorMessage::noSuchSlot, "no slot " + juce::String ((int) v.slotId));

    slot->telemetryActive = v.value != 0;
    if (slot->net != nullptr)
        slot->net->setTelemetryActive (v.value != 0 ? 1 : 0);
    ack (f);   // also for a plugin with no telemetry: there is simply nothing to send
}

void Session::handleSlotAdd (const Frame& f)
{
    control::SlotAdd add;
    if (! decode (f.payload, add))
        return fail (f, ErrorMessage::badRequest, "malformed SLOT_ADD");

    const auto* description = catalog.find (add.uid, add.name, add.vendor);
    if (description == nullptr)
        return fail (f, ErrorMessage::pluginNotFound, juce::String (add.name) + " is not on this server");

    juce::String error;
    auto instance = catalog.createInstance (*description, config.sampleRate, config.blockSize, error);
    if (instance == nullptr)
        return fail (f, ErrorMessage::pluginLoadFailed, "cannot load " + description->name + ": " + error);

    control::SlotReady ready;
    error = chain.addSlot (add.slotId, (int) add.position, std::move (instance), add.state, ready);
    if (error.isNotEmpty())
        return fail (f, ErrorMessage::pluginLoadFailed, error);

    log ("session " + juce::String ((int) sessionId) + ": slot " + juce::String ((int) add.slotId) + " = "
         + description->name + (ready.fxmenetAbi > 0 ? " (FxmeNet-aware)" : ""));
    connection->sendJson (Type::slotReady, f.requestId, ready.toVar());
}

void Session::handleParamSet (const Frame& f)
{
    std::vector<control::ParamChange> changes;
    if (! decode (f.payload, changes))
        return fail (f, ErrorMessage::badRequest, "malformed PARAM_SET");

    for (const auto& c : changes)
        if (auto* slot = chain.findSlot (c.slotId))
        {
            const auto& params = slot->instance->getParameters();
            if (juce::isPositiveAndBelow ((int) c.index, params.size()))
                params[(int) c.index]->setValue (juce::jlimit (0.0f, 1.0f, c.value));
        }
}

void Session::handleState (const Frame& f)
{
    control::SlotState s;
    if (! decode (f.payload, s))   // STATE_GET carries a SlotState with an empty state
        return fail (f, ErrorMessage::badRequest, "malformed state message");

    auto* slot = chain.findSlot (s.slotId);
    if (slot == nullptr)
        return fail (f, ErrorMessage::noSuchSlot, "no slot " + juce::String ((int) s.slotId));

    if (f.type == Type::stateSet)
    {
        slot->instance->setStateInformation (s.state.data(), (int) s.state.size());
        ack (f);
        return;
    }

    juce::MemoryBlock block;
    slot->instance->getStateInformation (block);
    control::SlotState reply;
    reply.slotId = s.slotId;
    reply.state.assign ((const std::uint8_t*) block.getData(),
                        (const std::uint8_t*) block.getData() + block.getSize());
    connection->sendBinary (Type::state, f.requestId, encode (reply));
}

void Session::handleRouting (const Frame& f)
{
    std::vector<control::Route> routes;
    if (! decode (f.payload, routes))
        return fail (f, ErrorMessage::badRequest, "malformed ROUTING");

    chain.setRouting (control::fromRoutes (routes));
    ack (f);
}

void Session::ack (const Frame& f)
{
    connection->send (f.type, 0, f.requestId, nullptr, 0);
}

void Session::fail (const Frame& f, int code, const juce::String& text)
{
    log ("session " + juce::String ((int) sessionId) + ": " + text);
    connection->sendError (f.requestId, code, text);
}

} // namespace fxmenet::server
