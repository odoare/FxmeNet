/*
  ------------------------------------------------------------------------------
    ClientEngine.h

    Everything FxmeNetPlugin does apart from its GUI and the DAW glue: the
    chain (client-authoritative, protocol.md section 8), the connection to a
    server, and the audio path. A console program drives it the same way
    (fxmenet-wavclient --engine).

    Threads:
      - the caller's threads (message thread in a plugin; never the audio
        thread): prepare(), connectTo(), the chain operations, getters;
      - the connection thread: connects, says HELLO (resuming the previous
        session when there is one), CONFIGURE, rebuilds the chain on the
        server when needed (CHAIN_CLEAR, then SLOT_ADD with each slot's state,
        its bypass and the parameter values set since that state), then reads
        the control channel (replies, PARAM_CHANGED, PONG) and pings every
        second. It reconnects every 2 s after a failure;
      - the audio thread: process().

    Chain operations made while connected go to the server at once
    (asynchronously, their replies update the slot); made while not, or when
    a send fails, they mark the chain dirty and the connection thread
    rebuilds it. A resumed session (protocol.md section 8) is kept as it is
    unless the chain is dirty. Every connection sends CONFIGURE, which
    restarts the server's block numbering, and gets a new AudioLink.

    Routing (doc/routing.md): the client holds the routing model (its
    rules, the user's manual pins) over every slot whose buses it knows
    (from SLOT_READY, or saved with the project), with INPUTS and OUTPUTS
    64 channels wide so that a project keeps its pins whatever the host's
    channel count. What the server gets is that model resolved on the chain
    the server really has: the host's channel counts, and only the slots
    loaded there (a slot missing on the server is bridged, as if bypassed
    for good, rule 2.4). It is sent (ROUTING) to a server announcing the
    "routing" capability, whenever it changes and after each (re)build of
    the session. FxmeNet's inputs that feed nothing are left out of each
    audio request (its channel mask), with no reconfiguration.

    Audio: BlockStream re-blocks and keeps the latency; blocks go to the
    current AudioLink, published to the audio thread with a hazard pointer
    (the connection thread waits until the audio thread no longer uses a
    link before deleting it). With no link (not connected, or rebuilding)
    or when bypassed, a block is played dry, delayed by the same latency, so
    timing never jumps; a new link restarts the stream at block 0.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

#include <FxmeNet/client/AudioLink.h>
#include <FxmeNet/client/BlockStream.h>
#include <FxmeNet/chain/Routing.h>
#include <FxmeNet/control/ControlConnection.h>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace fxmenet::client
{

/** One slot of the chain, as the client keeps it. */
struct SlotModel
{
    std::uint32_t slotId = 0;
    std::uint32_t uid = 0;
    juce::String name, vendor;
    std::vector<std::uint8_t> state;   ///< last known state, sent with SLOT_ADD
    bool bypassed = false;
    bool telemetryWanted = false;   ///< its editor is shown on the client (TELEMETRY_ACTIVE)

    /** Parameter values set (or reported by the server) since `state` was
        captured: re-applied after a rebuild. `seq` orders them against a
        state refresh. */
    struct Value { float value = 0.0f; std::uint64_t seq = 0; };
    std::map<std::uint32_t, Value> params;

    // The server's side.
    bool onServer = false;   ///< SLOT_ADD sent and not refused
    bool loaded = false;     ///< SLOT_READY received: `info` is valid
    juce::String error;      ///< why it is not loaded
    control::SlotReady info;

    /** Its buses for the routing: from SLOT_READY, or saved with the
        project (so the routing holds before the server answers). */
    chain::Box box;
    bool hasBox = false;
};

class ClientEngine : private juce::Thread
{
public:
    static constexpr const char* version = "0.1.0";
    static constexpr int reconnectMs = 2000;
    static constexpr int pingMs = 1000;

    /** In real time, how long a callback may wait for missing replies, as a
        fraction of its own duration (see AudioLink::fetch). */
    static constexpr double realtimeWaitFraction = 0.25;

    /** In real time, a host calling faster than real time (Reaper refilling
        its render-ahead buffer after a start, stop or seek) is ahead of its
        audio output: a callback may also wait this fraction of that margin,
        up to aheadWaitMaxSeconds. A margin above aheadMaxSeconds is taken
        for an error and measured again. */
    static constexpr double aheadWaitFraction = 0.5;
    static constexpr double aheadWaitMaxSeconds = 0.25;
    static constexpr double aheadMaxSeconds = 2.0;

    struct Settings
    {
        int blockSize = 128;       ///< N
        int budgetBlocks = 2;      ///< D
        std::uint64_t inputMask = ~std::uint64_t (0), outputMask = ~std::uint64_t (0);
        int telemetryHz = 30;
        int datagramBudget = (int) wire::defaultDatagramBudget;
    };

    enum class Status { idle, connecting, building, ready };

    explicit ClientEngine (juce::String nameOfClient)
        : juce::Thread ("FxmeNet connection"), clientName (std::move (nameOfClient))
    {
    }

    ~ClientEngine() override
    {
        signalThreadShouldExit();
        wake.signal();
        stopThread (8000);
    }

    /** Called after the status, the chain or the catalog changed: on the
        connection thread or the caller's. Set before connectTo(). */
    std::function<void()> onStateChange;

    /** A plugin on the server changed one of its parameters itself
        (PARAM_CHANGED), connection thread. Set before connectTo(). */
    std::function<void (std::uint32_t slotId, std::uint32_t index, float value)> onParamChanged;

    //==============================================================================
    /** The host's audio settings. With the audio stopped (prepareToPlay).
        Allocates, and reconfigures the session: CONFIGURE re-prepares the
        server's plugins, as the host re-prepares local ones (at the start of
        a render, typically), and a new link follows. The current link is
        unusable from here on (the audio thread plays dry, or waits when
        offline) until a CONFIGURE that saw this prepare is done: the stream
        never restarts on a session the server is still running. */
    void prepare (double sampleRate, int maxHostBlock, int numInputs, int numOutputs, const Settings& s)
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            host.sampleRate = sampleRate;
            host.maxBlock = juce::jmax (1, maxHostBlock);
            host.numInputs = juce::jlimit (0, wire::maxChannels, numInputs);
            host.numOutputs = juce::jlimit (0, wire::maxChannels, numOutputs);
            settings = s;
            settings.blockSize = juce::jlimit (16, 4096, s.blockSize);
            settings.budgetBlocks = juce::jlimit (1, 32, s.budgetBlocks);
            ++prepareGeneration;   // the current link is out of date
            reconfigureRequested = true;
            refreshRouting();      // the server's INPUTS and OUTPUTS follow the host
        }

        const int n = settings.blockSize;
        stream.prepare (host.numInputs, host.numOutputs, n, host.maxBlock, settings.budgetBlocks);
        maxInFlight = (BlockStream::latencyFor (n, host.maxBlock, settings.budgetBlocks, false) + host.maxBlock) / n + 3;
        dry.assign ((std::size_t) (maxInFlight * host.numInputs * n), 0.0f);
        dryBlock.assign ((std::size_t) maxInFlight, 0);
        sentBlock.assign ((std::size_t) maxInFlight, 0);
        audioSampleRate = sampleRate;
        audioGeneration = ~std::uint32_t (0);
        aheadAnchored = false;
        callbackCount = 0;
        prepared = true;
        wake.signal();
    }

    /** Connects to a server (and keeps reconnecting); an empty host
        disconnects. A different server starts a new session. */
    void connectTo (const juce::String& hostName, int port = 54322)
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            if (hostName == targetHost && port == targetPort)
                return;
            targetHost = hostName;
            targetPort = port;
            connectionWanted = hostName.isNotEmpty();
            ++targetVersion;
        }
        if (! isThreadRunning())
            startThread();
        wake.signal();
    }

    void disconnect()   { connectTo ({}, 0); }

    Status getStatus() const noexcept            { return status.load(); }
    juce::String getStatusText() const           { const std::lock_guard<std::mutex> l (lock); return statusText; }
    juce::String getServerName() const           { const std::lock_guard<std::mutex> l (lock); return serverName; }
    juce::String getServerOs() const             { const std::lock_guard<std::mutex> l (lock); return serverOs; }

    /** The latency to report: re-blocking and network budget, plus the
        server chain's own. Any thread. */
    int getLatency() const noexcept              { return stream.getLatency() + stats.chainLatency.load(); }
    int getStreamLatency() const noexcept        { return stream.getLatency(); }
    std::uint64_t getConcealedCount() const noexcept   { return stream.getConcealedCount(); }
    std::uint64_t getMidiDropped() const noexcept      { return stream.getMidiDropped(); }   ///< MIDI events not sent (too long, block full)

    /** How far ahead of real time the host was at the last callback, in ms
        (0 when it calls at real-time pace). */
    double getHostAheadMs() const noexcept       { return hostAheadMicros.load() / 1000.0; }

    /** The host reset the plugin (transport start, stop, seek, flush): the
        server resets its chain at the same point of the stream (the next
        block sent carries the new parity, wire::requestFlags::resetParity).
        Any thread; not while process() runs, as hosts guarantee. */
    void reset() noexcept
    {
        lastResetMs.store (juce::Time::getMillisecondCounterHiRes());
        resets.fetch_add (1);
    }
    std::uint32_t getResetCount() const noexcept { return resets.load(); }

    /** Whether a host reset also drops the audio in flight (silence for the
        latency, as a local plugin clearing its delay line). Any thread. */
    void setFlushOnReset (bool on) noexcept      { flushOnReset.store (on); }
    bool getFlushOnReset() const noexcept        { return flushOnReset.load(); }

    /** Diagnostics: what the audio thread saw around transport events. */
    struct AudioEvent
    {
        enum Bits : std::uint32_t
        {
            resetSeen  = 1,   ///< the reset count changed since the last callback
            playChange = 2,   ///< playing / stopped changed
            jump       = 4,   ///< playing, and the play head is not where the last callback ended
            concealed  = 8    ///< blocks concealed in this callback
        };
        std::uint32_t bits = 0;
        std::uint64_t callback = 0;        ///< callback count since prepare
        double wallMs = 0.0;               ///< juce::Time::getMillisecondCounterHiRes
        double resetDelayMs = 0.0;         ///< resetSeen: since reset() was called
        std::int64_t time = 0, expected = 0;   ///< play head, and where it would be without a jump
        bool playing = false, flushed = false;
        int numSamples = 0;
        std::uint32_t concealedBlocks = 0;
        std::uint32_t aheadMicros = 0;
    };

    /** Message thread: the events since the last call, oldest first (the
        oldest are lost when more than eventCapacity pile up). */
    template <typename Function>
    void drainEvents (Function&& fn)
    {
        const auto end = eventWrite.load (std::memory_order_acquire);
        if (end - eventRead > eventCapacity)
            eventRead = end - eventCapacity;
        for (; eventRead < end; ++eventRead)
            fn (events[(std::size_t) (eventRead % eventCapacity)]);
    }
    const AudioStatistics& getStatistics() const noexcept   { return stats; }
    std::uint32_t getControlRttMicros() const noexcept      { return controlRttMicros.load(); }

    //==============================================================================
    // The chain. Not on the audio thread.

    std::vector<SlotModel> getSlots() const      { const std::lock_guard<std::mutex> l (lock); return slots; }

    /** The slots without their state blobs (which can be large): for
        displays polling often. */
    std::vector<SlotModel> getSlotInfos() const
    {
        const std::lock_guard<std::mutex> l (lock);
        std::vector<SlotModel> out;
        out.reserve (slots.size());
        for (auto& s : slots)
        {
            SlotModel copy;
            copy.slotId = s.slotId;
            copy.uid = s.uid;
            copy.name = s.name;
            copy.vendor = s.vendor;
            copy.bypassed = s.bypassed;
            copy.telemetryWanted = s.telemetryWanted;
            copy.params = s.params;
            copy.onServer = s.onServer;
            copy.loaded = s.loaded;
            copy.error = s.error;
            copy.info = s.info;
            copy.box = s.box;
            copy.hasBox = s.hasBox;
            out.push_back (std::move (copy));
        }
        return out;
    }

    /** A slot parameter's current value, normalised: the last one set or
        reported, else the one SLOT_READY gave. */
    static float currentValue (const SlotModel& s, const control::ParamInfo& p)
    {
        const auto it = s.params.find ((std::uint32_t) p.index);
        return it != s.params.end() ? it->second.value : p.value;
    }

    /** Adds a plugin at `position` (clamped); returns its slot id. */
    std::uint32_t addSlot (std::uint32_t uid, const juce::String& name, const juce::String& vendor,
                           int position, std::vector<std::uint8_t> state = {})
    {
        std::uint32_t id;
        {
            const std::lock_guard<std::mutex> l (lock);
            SlotModel s;
            s.slotId = id = nextSlotId++;
            s.uid = uid;
            s.name = name;
            s.vendor = vendor;
            s.state = std::move (state);
            position = juce::jlimit (0, (int) slots.size(), position);
            slots.insert (slots.begin() + position, s);
            if (ready)
                sendSlotAdd (slots[(std::size_t) position], position);
            else
                dirty = true;
        }
        notify();
        return id;
    }

    void removeSlot (std::uint32_t slotId)
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            const int i = indexOf (slotId);
            if (i < 0)
                return;
            const bool wasOnServer = slots[(std::size_t) i].onServer;
            slots.erase (slots.begin() + i);
            if (ready && wasOnServer)
                sendSlotValue (control::Type::slotRemove, slotId, 0);
            else if (! ready)
                dirty = true;
            refreshRouting();
        }
        notify();
    }

    void moveSlot (std::uint32_t slotId, int newPosition)
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            const int i = indexOf (slotId);
            if (i < 0)
                return;
            auto s = slots[(std::size_t) i];
            slots.erase (slots.begin() + i);
            newPosition = juce::jlimit (0, (int) slots.size(), newPosition);
            slots.insert (slots.begin() + newPosition, s);
            if (ready && s.onServer)
                sendSlotValue (control::Type::slotMove, slotId, (std::uint32_t) serverPosition (newPosition));
            else if (! ready)
                dirty = true;
            refreshRouting();
        }
        notify();
    }

    void setBypass (std::uint32_t slotId, bool bypassed)
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            auto* s = find (slotId);
            if (s == nullptr || s->bypassed == bypassed)
                return;
            s->bypassed = bypassed;
            if (ready && s->onServer)
                sendSlotValue (control::Type::slotBypass, slotId, bypassed ? 1u : 0u);
            else if (! ready)
                dirty = true;
        }
        notify();
    }

    /** A parameter of a slot, normalised. Not on the audio thread. */
    void setParameter (std::uint32_t slotId, std::uint32_t index, float value)
    {
        setParameters ({ { slotId, index, value } });
    }

    /** Several parameter values, sent as one PARAM_SET. Not on the audio thread. */
    void setParameters (const std::vector<control::ParamChange>& changes)
    {
        const std::lock_guard<std::mutex> l (lock);
        std::vector<control::ParamChange> toSend;
        for (auto& c : changes)
            if (auto* s = find (c.slotId))
            {
                s->params[c.index] = { c.value, ++paramSeq };
                if (s->onServer)
                    toSend.push_back (c);
            }
        if (ready && ! toSend.empty() && ! sendFrame (control::Type::paramSet, 0, encode (toSend)))
            dirty = true;
    }

    /** Replaces the whole chain (a saved state being loaded); rebuilt on the
        server by the connection thread. */
    void setSlots (std::vector<SlotModel> newSlots)
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            slots = std::move (newSlots);
            nextSlotId = 1;
            for (auto& s : slots)
            {
                s.onServer = s.loaded = false;
                s.error = {};
                s.info = {};
                nextSlotId = juce::jmax (nextSlotId, s.slotId + 1);
            }
            routing.reset();   // the saved routing follows (setRoutingPins)
            dirty = true;
            refreshRouting();
        }
        wake.signal();
        notify();
    }

    //==============================================================================
    // Routing (doc/routing.md). Not on the audio thread.

    /** True when the server applies ROUTING (its WELCOME said so). */
    bool isRoutingAvailable() const noexcept     { return serverRoutes.load(); }

    /** The routing model, over every slot whose buses are known (INPUTS
        and OUTPUTS 64 channels wide): for the view. */
    chain::Routing getRouting() const            { const std::lock_guard<std::mutex> l (lock); return routing; }

    /** The connections as the server gets them (its chain, the host's
        channels). */
    std::vector<chain::Connection> getServerRouting() const
    {
        const std::lock_guard<std::mutex> l (lock);
        return serverConnections;
    }

    /** The user's edits; false when not valid. */
    bool connectPins (chain::Pin from, chain::Pin to)      { return editRouting ([&] { return routing.connect (from, to); }); }
    bool disconnectPins (chain::Pin from, chain::Pin to)   { return editRouting ([&] { return routing.disconnect (from, to); }); }
    void resetRouting()                                    { editRouting ([&] { routing.reset(); return true; }); }
    bool setPinAutomatic (chain::Pin input)                { return editRouting ([&] { return routing.makeAutomatic (input); }); }
    bool clearPin (chain::Pin input)                       { return editRouting ([&] { return routing.clearInput (input); }); }

    /** The manual pins, to save with the project, and back (after setSlots). */
    std::vector<chain::ManualInput> getRoutingPins() const
    {
        const std::lock_guard<std::mutex> l (lock);
        return routing.getManual();
    }

    void setRoutingPins (const std::vector<chain::ManualInput>& pins)
    {
        editRouting ([&] { routing.setManual (pins); return true; });
    }

    /** Fetches every loaded slot's state from the server (STATE_GET), so the
        model can be saved. Waits up to `timeoutMs`; false when some did not
        answer (their previous state is kept). */
    bool refreshStates (int timeoutMs)
    {
        struct Wait
        {
            juce::WaitableEvent done;
            std::atomic<int> remaining { 0 };
            std::atomic<bool> allOk { true };
        };
        auto wait = std::make_shared<Wait>();

        std::vector<std::pair<std::uint32_t, std::uint64_t>> asked;
        {
            const std::lock_guard<std::mutex> l (lock);
            if (! ready)
                return false;
            for (auto& s : slots)
                if (s.loaded)
                    asked.push_back ({ s.slotId, paramSeq });
        }
        if (asked.empty())
            return true;

        wait->remaining = (int) asked.size();
        for (auto [slotId, seq] : asked)
        {
            control::SlotState get;
            get.slotId = slotId;
            const auto id = slotId;
            const auto seqAtRequest = seq;
            const bool sent = sendAsync (control::Type::stateGet, 0, encode (get), [this, wait, id, seqAtRequest] (const control::Frame* f)
            {
                control::SlotState s;
                if (f != nullptr && f->type == control::Type::state && decode (f->payload, s) && s.slotId == id)
                {
                    const std::lock_guard<std::mutex> l (lock);
                    if (auto* slot = find (id))
                    {
                        slot->state = std::move (s.state);
                        for (auto it = slot->params.begin(); it != slot->params.end();)
                        {
                            if (it->second.seq > seqAtRequest)
                            {
                                ++it;
                                continue;
                            }
                            // In the state now; the value stays known for displays.
                            for (auto& p : slot->info.params)
                                if ((std::uint32_t) p.index == it->first)
                                    p.value = it->second.value;
                            it = slot->params.erase (it);
                        }
                    }
                }
                else
                {
                    wait->allOk = false;
                }
                if (--wait->remaining == 0)
                    wait->done.signal();
            });

            if (! sent)
            {
                wait->allOk = false;
                if (--wait->remaining == 0)
                    wait->done.signal();
            }
        }
        return wait->done.wait (timeoutMs) && wait->allOk;
    }

    /** Asks the server for a slot's telemetry, or stops it (its editor was
        shown or closed). Kept in the model: sent again after a rebuild. */
    void setTelemetryActive (std::uint32_t slotId, bool on)
    {
        const std::lock_guard<std::mutex> l (lock);
        auto* slot = find (slotId);
        if (slot == nullptr || slot->telemetryWanted == on)
            return;
        slot->telemetryWanted = on;
        if (ready && slot->onServer)
            sendFrame (control::Type::telemetryActive, 0, encode (control::SlotValue { slotId, on ? 1u : 0u }));
    }

    /** Message thread: the telemetry frames received since the last call,
        by slot, oldest first. */
    std::map<std::uint32_t, std::vector<std::vector<std::uint8_t>>> takeTelemetry()   { return telemetry.take(); }

    /** Fetches one slot's state from the server (STATE_GET). `done` gets it
        on the connection thread (empty when it failed); the model keeps it
        too. */
    void fetchState (std::uint32_t slotId, std::function<void (std::vector<std::uint8_t>)> done)
    {
        std::uint64_t seqAtRequest;
        {
            const std::lock_guard<std::mutex> l (lock);
            seqAtRequest = paramSeq;
        }
        control::SlotState get;
        get.slotId = slotId;
        const bool sent = sendAsync (control::Type::stateGet, 0, encode (get),
                                     [this, slotId, seqAtRequest, done] (const control::Frame* f)
        {
            control::SlotState s;
            if (f == nullptr || f->type != control::Type::state || ! decode (f->payload, s) || s.slotId != slotId)
            {
                done ({});
                return;
            }
            {
                const std::lock_guard<std::mutex> l (lock);
                if (auto* slot = find (slotId))
                {
                    slot->state = s.state;
                    for (auto it = slot->params.begin(); it != slot->params.end();)
                    {
                        if (it->second.seq > seqAtRequest)
                        {
                            ++it;
                            continue;
                        }
                        for (auto& p : slot->info.params)
                            if ((std::uint32_t) p.index == it->first)
                                p.value = it->second.value;
                        it = slot->params.erase (it);
                    }
                }
            }
            done (std::move (s.state));
        });
        if (! sent)
            done ({});
    }

    /** Replaces a slot's state, on the server (STATE_SET) and in the model:
        a non-parameter change made on the client (a mirror's editor loading
        a preset or a file). Not on the audio thread. */
    void setSlotState (std::uint32_t slotId, std::vector<std::uint8_t> state)
    {
        const std::lock_guard<std::mutex> l (lock);
        auto* slot = find (slotId);
        if (slot == nullptr)
            return;
        slot->state = state;
        // The parameter values set since are kept: a preset load also
        // changes parameters, which arrive as PARAM_SET anyway, and the
        // displays read their values from there.
        if (! ready || ! slot->onServer)
        {
            dirty = ! ready || dirty;
            return;
        }
        control::SlotState set;
        set.slotId = slotId;
        set.state = std::move (state);
        dirty |= ! sendAsync (control::Type::stateSet, 0, encode (set), [this] (const control::Frame* f)
        {
            if (f == nullptr || f->type == control::Type::error)
            {
                const std::lock_guard<std::mutex> later (lock);
                dirty = true;   // the server may not have it: rebuild from the model
            }
        });
    }

    /** Asks the server for its plugins; getCatalog() has them after the
        next onStateChange. */
    void requestCatalog()
    {
        sendAsync (control::Type::listPlugins, 0, {}, [this] (const control::Frame* f)
        {
            if (f == nullptr || f->type != control::Type::pluginList)
                return;
            {
                const std::lock_guard<std::mutex> l (lock);
                catalog = control::pluginListFromVar (control::fromPayload (f->payload));
            }
            notify();
        });
    }

    std::vector<control::PluginEntry> getCatalog() const   { const std::lock_guard<std::mutex> l (lock); return catalog; }

    //==============================================================================
    /** Audio thread. `in` has the prepared number of inputs, `out` of
        outputs; they may be the same buffers. `offline`: the host renders
        faster than real time (each reply is waited for). `transport` may be
        null. `midi`: the callback's events, in position order (may be null),
        sent with the blocks that hold them. No allocation; waits only when
        offline. */
    void process (const float* const* in, float* const* out, int numSamples, bool offline, bool bypassed,
                  const wire::Transport* transport, const MidiInput* midi = nullptr, int numMidi = 0)
    {
        if (! prepared)
            return;

        if (offline)
            waitForSessionOffline();

        AudioLink* link = acquireLink();
        const auto generation = link != nullptr ? link->getGeneration() : 0u;
        const auto resetCount = resets.load();
        AudioEvent ev;
        ev.callback = ++callbackCount;
        ev.numSamples = numSamples;
        if (resetCount != audioResets)
        {
            ev.bits |= AudioEvent::resetSeen;
            ev.resetDelayMs = juce::Time::getMillisecondCounterHiRes() - lastResetMs.load();
        }
        if (transport != nullptr)
        {
            ev.time = transport->timeInSamples;
            ev.expected = evNextTime;
            ev.playing = transport->playing;
            if (transport->playing != evPlaying)
                ev.bits |= AudioEvent::playChange;
            else if (transport->playing && transport->timeInSamples != evNextTime)
                ev.bits |= AudioEvent::jump;
            evPlaying = transport->playing;
            evNextTime = transport->timeInSamples + numSamples;
        }

        const bool flushNow = resetCount != audioResets && flushOnReset.load();
        if (generation != audioGeneration || flushNow)
        {
            // Back to block 0, the FIFO refilled with silence. After a host
            // reset the latency is kept, and the replies in flight (processed
            // before the server's chain was reset) are dropped: like a local
            // plugin clearing its delay line.
            if (generation != audioGeneration)
                stream.reset();
            else
                stream.flush();
            std::fill (dryBlock.begin(), dryBlock.end(), 0);   // k starts again: no old block may match
            std::fill (sentBlock.begin(), sentBlock.end(), 0);
            if (link != nullptr)
                link->rebase();      // same link: its numbering goes on, older replies refused
            audioGeneration = generation;
            ev.flushed = flushNow;
        }
        audioResets = resetCount;

        juce::int64 deadline = 0;
        if (! offline && audioSampleRate > 0.0)
        {
            const auto now = juce::Time::getHighResolutionTicks();
            const double wait = realtimeWaitFraction * numSamples / audioSampleRate
                              + aheadWaitFraction * juce::jmin (measureAhead (now, numSamples, transport), aheadWaitMaxSeconds);
            deadline = now + (juce::int64) (wait * (double) juce::Time::getHighResolutionTicksPerSecond());
        }
        else
        {
            aheadAnchored = false;
        }

        const bool resetParity = (resetCount & 1u) != 0;
        Network network { *this, link, offline, bypassed, resetParity, transport, deadline };
        const auto concealedBefore = stream.getConcealedCount();
        stream.process (in, out, numSamples, network, midi, numMidi);
        releaseLink();

        ev.concealedBlocks = (std::uint32_t) (stream.getConcealedCount() - concealedBefore);
        if (ev.concealedBlocks > 0)
            ev.bits |= AudioEvent::concealed;
        if (ev.bits != 0)
        {
            ev.wallMs = juce::Time::getMillisecondCounterHiRes();
            ev.aheadMicros = hostAheadMicros.load (std::memory_order_relaxed);
            const auto w = eventWrite.load (std::memory_order_relaxed);
            events[(std::size_t) (w % eventCapacity)] = ev;
            eventWrite.store (w + 1, std::memory_order_release);
        }
    }

    /** Audio thread, real time: how many seconds of audio the host has
        delivered beyond the wall clock, before this callback. Measured from
        an anchor taken again at every discontinuity (a reset, play / stop,
        a jump of the play head, the host falling behind): audio the host
        threw away (its render-ahead buffer at a seek) is never counted. */
    double measureAhead (juce::int64 now, int numSamples, const wire::Transport* transport)
    {
        const auto r = resets.load();
        bool restart = ! aheadAnchored || r != aheadResets;
        aheadResets = r;
        if (transport != nullptr)
        {
            restart = restart || transport->playing != aheadPlaying
                   || (transport->playing && transport->timeInSamples != aheadNextTime);
            aheadPlaying = transport->playing;
            aheadNextTime = transport->timeInSamples + numSamples;
        }

        double ahead = 0.0;
        if (! restart)
        {
            const double elapsed = (double) (now - aheadAnchorTicks) / (double) juce::Time::getHighResolutionTicksPerSecond();
            ahead = aheadDelivered - elapsed;
            restart = ahead < 0.0 || ahead > aheadMaxSeconds;
        }
        if (restart)
        {
            aheadAnchored = true;
            aheadAnchorTicks = now;
            aheadDelivered = 0.0;
            ahead = 0.0;
        }
        aheadDelivered += numSamples / audioSampleRate;
        hostAheadMicros.store ((std::uint32_t) (ahead * 1.0e6), std::memory_order_relaxed);
        return ahead;
    }

private:
    //==============================================================================
    /** Audio thread, offline only: a render must not run dry through the
        moment a session is being set up or reconfigured, so wait for it, up
        to offlineWaitMs per episode (a server that is gone costs one wait,
        then the render goes on dry). */
    void waitForSessionOffline()
    {
        auto sessionReady = [this]
        {
            return status.load() == Status::ready && published.load() != nullptr
                && usableGeneration.load() == prepareGeneration.load();
        };
        if (sessionReady())
        {
            offlineWaitLeftMs = offlineWaitMs;
            return;
        }
        if (! connectionWanted.load() || offlineWaitLeftMs <= 0)
            return;

        const auto start = juce::Time::getMillisecondCounter();
        while (! sessionReady() && (int) (juce::Time::getMillisecondCounter() - start) < offlineWaitLeftMs)
            juce::Thread::sleep (2);
        offlineWaitLeftMs -= (int) (juce::Time::getMillisecondCounter() - start);
    }

    //==============================================================================
    // Audio thread: BlockStream's network.
    struct Network
    {
        ClientEngine& e;
        AudioLink* link;
        bool offline, bypassed, resetParity;
        const wire::Transport* transport;
        juce::int64 deadline;   ///< real time: how long missing replies may be waited for

        void blockReady (std::uint64_t k, const float* const* block, int endOffset)
        {
            const int n = e.stream.getBlockSize();
            const auto slot = (std::size_t) (k % (std::uint64_t) e.maxInFlight);
            float* d = e.dry.data() + slot * (std::size_t) (e.host.numInputs * n);
            for (int c = 0; c < e.host.numInputs; ++c)
                std::copy (block[c], block[c] + n, d + c * n);
            e.dryBlock[slot] = k + 1;

            const bool send = link != nullptr && ! bypassed;
            e.sentBlock[slot] = send ? k + 1 : 0;
            if (! send)
                return;

            link->setInputsUsed (e.routedInputs.load (std::memory_order_relaxed));
            const auto* midi = e.stream.getBlockMidi();
            const auto midiBytes = e.stream.getBlockMidiBytes();
            if (transport == nullptr)
            {
                link->send (k, block, nullptr, offline, resetParity, midi, midiBytes);
                return;
            }

            // The play head at the block's first sample.
            auto t = *transport;
            const int start = endOffset - n;
            t.timeInSamples += start;
            if (e.audioSampleRate > 0.0)
                t.ppqPosition += start / e.audioSampleRate * t.bpm / 60.0;
            link->send (k, block, &t, offline, resetParity, midi, midiBytes);
        }

        bool fetchBlock (std::uint64_t k, float* const* dest)
        {
            const int n = e.stream.getBlockSize();
            const auto slot = (std::size_t) (k % (std::uint64_t) e.maxInFlight);
            if (link != nullptr && ! bypassed && e.sentBlock[slot] == k + 1)
                return link->fetch (k, dest, offline, deadline);

            if (link != nullptr)
                link->skip (k);

            const bool have = e.dryBlock[slot] == k + 1;
            const float* d = e.dry.data() + slot * (std::size_t) (e.host.numInputs * n);
            for (int c = 0; c < e.host.numOutputs; ++c)
            {
                if (have && c < e.host.numInputs)
                    std::copy (d + c * n, d + (c + 1) * n, dest[c]);
                else
                    std::fill (dest[c], dest[c] + n, 0.0f);
            }
            return true;
        }
    };

    /** The link, or null when there is none or it predates the last prepare(). */
    AudioLink* acquireLink() noexcept
    {
        if (usableGeneration.load() != prepareGeneration.load())
            return nullptr;
        for (;;)
        {
            auto* p = published.load();
            linkInUse.store (p);
            if (published.load() == p)
                return p;
        }
    }

    void releaseLink() noexcept    { linkInUse.store (nullptr); }

    /** Connection thread: replaces the link the audio thread uses, and
        deletes the old one once the audio thread has let go of it. */
    void publishLink (std::unique_ptr<AudioLink> next)
    {
        auto old = std::move (ownedLink);
        ownedLink = std::move (next);
        auto* previous = published.exchange (ownedLink.get());
        while (previous != nullptr && linkInUse.load() == previous)
            juce::Thread::sleep (1);
        old.reset();
    }

    //==============================================================================
    // Connection thread.

    void run() override
    {
        int servedVersion = -1;
        while (! threadShouldExit())
        {
            juce::String hostName;
            int port = 0, wanted = 0;
            {
                const std::lock_guard<std::mutex> l (lock);
                hostName = targetHost;
                port = targetPort;
                wanted = targetVersion;
            }
            if (wanted != servedVersion)
            {
                servedVersion = wanted;
                sessionId = 0;   // another server: nothing to resume
                token = 0;
            }

            if (hostName.isEmpty() || ! prepared)
            {
                setStatus (Status::idle, hostName.isEmpty() ? "Not connected" : "Waiting for the audio settings");
                wake.wait (500);
                continue;
            }

            setStatus (Status::connecting, "Connecting to " + hostName + ":" + juce::String (port));
            auto c = control::ControlConnection::connect (hostName, port, 2000);
            if (c == nullptr)
            {
                setStatus (Status::connecting, "No answer from " + hostName + ":" + juce::String (port) + ", retrying");
                wake.wait (reconnectMs);
                continue;
            }

            {
                const std::lock_guard<std::mutex> l (connectionLock);
                connection = std::move (c);
            }
            runSession (hostName, wanted);

            publishLink (nullptr);
            {
                const std::lock_guard<std::mutex> l (lock);
                ready = false;
            }
            std::shared_ptr<control::ControlConnection> closing;
            {
                const std::lock_guard<std::mutex> l (connectionLock);
                closing = std::move (connection);
            }
            if (closing != nullptr)
                closing->close();
            failPending();

            if (! threadShouldExit() && wanted == currentTargetVersion())
            {
                setStatus (Status::connecting, statusTextCopy() + "; reconnecting");
                wake.wait (reconnectMs);
            }
        }
        publishLink (nullptr);
    }

    /** One connection, from HELLO until it is lost or not wanted any more. */
    void runSession (const juce::String& hostName, int servedVersion)
    {
        control::Hello hello;
        hello.clientName = clientName;
        hello.os = juce::SystemStats::getOperatingSystemName();
        hello.fxmenetVersion = ClientEngine::version;
        hello.resume = sessionId != 0;
        hello.resumeSessionId = sessionId;
        hello.resumeToken = token;

        control::Frame reply;
        if (call (control::Type::hello, control::flagJson, control::toPayload (hello.toVar()),
                  control::Type::welcome, reply, 5000) != CallResult::ok)
            return setStatus (Status::connecting, "No welcome from " + hostName + ": " + lastError);

        const auto welcome = control::Welcome::fromVar (control::fromPayload (reply.payload));
        if (welcome.error.isNotEmpty())
            return setStatus (Status::connecting, "Refused by " + hostName + ": " + welcome.error);

        sessionId = welcome.sessionId;
        token = welcome.token;
        udpPort = welcome.udpPort;
        {
            const std::lock_guard<std::mutex> l (lock);
            serverName = welcome.serverName;
            serverOs = welcome.os;
            serverRoutes = welcome.capabilities.contains (control::capabilityRouting);
            routingSent = false;   // a new connection: sent again once the chain is there
        }

        if (! configure (hostName) || ! syncChain (welcome.resumed))
            return;
        setStatus (Status::ready, "Connected to " + welcome.serverName + (welcome.resumed ? " (resumed)" : ""));

        auto lastPing = juce::Time::getMillisecondCounter() - (juce::uint32) pingMs;
        while (! threadShouldExit() && servedVersion == currentTargetVersion())
        {
            if (reconfigureRequested.exchange (false) && ! configure (hostName))
                return;
            if (isDirty() && ! syncChain (false))
                return;

            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastPing >= (juce::uint32) pingMs)
            {
                lastPing = now;
                sendFrame (control::Type::ping, 0, control::encodePing ((std::uint64_t) (juce::Time::getMillisecondCounterHiRes() * 1000.0)));
            }

            control::Frame f;
            const auto r = connectionPtr()->read (f, 50);
            if (r == control::ControlConnection::ReadResult::closed)
                return setStatus (Status::connecting, "Connection to " + hostName + " lost");
            if (r == control::ControlConnection::ReadResult::frame)
                dispatch (f);
        }

        // Leaving on purpose: the server can drop the session now.
        sendFrame (control::Type::bye, 0, {});
        sessionId = 0;
    }

    /** CONFIGURE, then a new link. The audio plays dry meanwhile. */
    bool configure (const juce::String& hostName)
    {
        reconfigureRequested = false;
        publishLink (nullptr);

        control::Configure c;
        AudioLink::Settings ls;
        {
            const std::lock_guard<std::mutex> l (lock);
            c.sampleRate = host.sampleRate;
            c.blockSize = settings.blockSize;
            c.numInputs = juce::jmax (1, host.numInputs);
            c.numOutputs = juce::jmax (1, host.numOutputs);
            c.inputMask = settings.inputMask & wire::maskOfFirst (host.numInputs);
            c.outputMask = settings.outputMask & wire::maskOfFirst (host.numOutputs);
            c.telemetryHz = settings.telemetryHz;
            c.datagramBudget = settings.datagramBudget;
            ls.numInputs = host.numInputs;
            ls.numOutputs = host.numOutputs;
            configuredGeneration = prepareGeneration.load();
        }

        control::Frame reply;
        if (call (control::Type::configure, control::flagJson, control::toPayload (c.toVar()),
                  control::Type::configure, reply, 10000) != CallResult::ok)
        {
            setStatus (Status::connecting, "Configuration refused: " + lastError);
            return false;
        }

        ls.host = hostName;
        ls.udpPort = udpPort;
        ls.sessionId = sessionId;
        ls.token = token;
        ls.config = c;
        ls.maxInFlight = maxInFlight;
        ls.generation = ++linkGeneration;
        auto link = std::make_unique<AudioLink> (ls, stats, telemetry);
        if (! link->start())
        {
            setStatus (Status::connecting, "Cannot open a UDP socket");
            return false;
        }
        publishLink (std::move (link));
        usableGeneration = configuredGeneration;   // usable unless prepared again meanwhile
        return true;
    }

    /** Rebuilds the chain on the server, unless resumed and nothing changed;
        again if it changed meanwhile. False when the connection is lost. */
    bool syncChain (bool resumed)
    {
        for (;;)
        {
            std::vector<SlotModel> snapshot;
            {
                const std::lock_guard<std::mutex> l (lock);
                if (resumed && ! dirty)
                {
                    // The server kept the chain; parameter values set while
                    // disconnected are re-sent (idempotent).
                    std::vector<control::ParamChange> changes;
                    for (auto& slot : slots)
                        if (slot.onServer)
                            for (auto& [index, v] : slot.params)
                                changes.push_back ({ slot.slotId, index, v.value });
                    if (! changes.empty())
                        sendFrame (control::Type::paramSet, 0, encode (changes));
                    for (auto& slot : slots)
                        if (slot.onServer && slot.telemetryWanted)
                            sendFrame (control::Type::telemetryActive, 0, encode (control::SlotValue { slot.slotId, 1 }));
                    ready = true;
                    refreshRouting();
                    return true;
                }
                ready = false;
                dirty = false;
                snapshot = slots;
            }
            resumed = false;
            setStatus (Status::building, "Loading the chain on the server");

            control::Frame reply;
            if (call (control::Type::chainClear, 0, {}, control::Type::chainClear, reply, 10000) != CallResult::ok)
                return markDirty();

            int position = 0;
            for (auto& s : snapshot)
            {
                control::SlotAdd add;
                add.slotId = s.slotId;
                add.position = (std::uint32_t) position;
                add.uid = s.uid;
                add.name = s.name.toStdString();
                add.vendor = s.vendor.toStdString();
                add.state = s.state;

                const auto r = call (control::Type::slotAdd, 0, encode (add), control::Type::slotReady, reply, 60000);
                if (r == CallResult::lost)
                    return markDirty();
                s.onServer = s.loaded = r == CallResult::ok;
                s.error = r == CallResult::ok ? juce::String() : lastError;
                if (r != CallResult::ok)
                    continue;

                s.info = control::SlotReady::fromVar (control::fromPayload (reply.payload));
                ++position;
                if (s.bypassed)
                    sendFrame (control::Type::slotBypass, 0, encode (control::SlotValue { s.slotId, 1 }));
                if (s.telemetryWanted)
                    sendFrame (control::Type::telemetryActive, 0, encode (control::SlotValue { s.slotId, 1 }));
                if (! s.params.empty())
                {
                    std::vector<control::ParamChange> changes;
                    for (auto& [index, v] : s.params)
                        changes.push_back ({ s.slotId, index, v.value });
                    sendFrame (control::Type::paramSet, 0, encode (changes));
                }
            }

            {
                const std::lock_guard<std::mutex> l (lock);
                for (auto& done : snapshot)
                    if (auto* s = find (done.slotId))
                    {
                        s->onServer = done.onServer;
                        s->loaded = done.loaded;
                        s->error = done.error;
                        s->info = done.info;
                        if (done.loaded)
                        {
                            s->box = control::toBox (done.info);
                            s->hasBox = true;
                        }
                    }
                if (! dirty)
                {
                    ready = true;
                    refreshRouting();
                    break;
                }
            }
        }
        notify();
        return true;
    }

    /** The server's chain is incomplete: the next connection rebuilds it. */
    bool markDirty()
    {
        const std::lock_guard<std::mutex> l (lock);
        dirty = true;
        return false;
    }

    enum class CallResult { ok, refused, lost };

    /** Connection thread: a request and its answer, dispatching whatever
        else arrives meanwhile. `lastError` says why on failure. */
    CallResult call (control::Type type, std::uint16_t flags, const std::vector<std::uint8_t>& payload,
                     control::Type expected, control::Frame& reply, int timeoutMs)
    {
        auto c = connectionPtr();
        const auto id = nextRequestId++;
        if (c == nullptr || ! c->send (type, flags, id, payload.data(), payload.size()))
        {
            lastError = "connection lost";
            return CallResult::lost;
        }

        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (! threadShouldExit())
        {
            const auto now = juce::Time::getMillisecondCounter();
            if (now >= deadline)
                break;
            const auto r = c->read (reply, juce::jmin (100, (int) (deadline - now)));   // short: notices shutdown
            if (r == control::ControlConnection::ReadResult::closed)
            {
                lastError = "connection lost";
                return CallResult::lost;
            }
            if (r != control::ControlConnection::ReadResult::frame)
                continue;
            if (reply.requestId != id)
            {
                dispatch (reply);
                continue;
            }
            if (reply.type == expected)
                return CallResult::ok;
            lastError = reply.type == control::Type::error
                          ? control::ErrorMessage::fromVar (control::fromPayload (reply.payload)).text
                          : "unexpected answer " + juce::String ((int) reply.type);
            return CallResult::refused;
        }
        lastError = "no answer";
        return CallResult::lost;   // a server that stops answering is treated as gone
    }

    /** Connection thread: a frame that is not the answer being waited for. */
    void dispatch (const control::Frame& f)
    {
        if (f.requestId != 0)
        {
            std::function<void (const control::Frame*)> done;
            {
                const std::lock_guard<std::mutex> l (connectionLock);
                const auto it = pending.find (f.requestId);
                if (it != pending.end())
                {
                    done = std::move (it->second);
                    pending.erase (it);
                }
            }
            if (done)
                done (&f);
            return;
        }

        if (f.type == control::Type::pong)
        {
            std::uint64_t sentMicros = 0;
            if (control::decodePing (f.payload, sentMicros))
            {
                const auto nowMicros = (std::uint64_t) (juce::Time::getMillisecondCounterHiRes() * 1000.0);
                controlRttMicros = (std::uint32_t) juce::jmin<std::uint64_t> (nowMicros - sentMicros, 0xffffffffu);
            }
            return;
        }

        if (f.type == control::Type::paramChanged)
        {
            std::vector<control::ParamChange> changes;
            if (! decode (f.payload, changes))
                return;
            {
                const std::lock_guard<std::mutex> l (lock);
                for (auto& c : changes)
                    if (auto* s = find (c.slotId))
                        s->params[c.index] = { c.value, ++paramSeq };
            }
            if (onParamChanged)
                for (auto& c : changes)
                    onParamChanged (c.slotId, c.index, c.value);
        }
    }

    /** Ends every request still waiting for an answer: the connection is gone. */
    void failPending()
    {
        std::map<std::uint32_t, std::function<void (const control::Frame*)>> waiting;
        {
            const std::lock_guard<std::mutex> l (connectionLock);
            waiting.swap (pending);
        }
        for (auto& [id, done] : waiting)
            done (nullptr);
    }

    //==============================================================================
    // Sending, any thread but the audio thread.

    std::shared_ptr<control::ControlConnection> connectionPtr() const
    {
        const std::lock_guard<std::mutex> l (connectionLock);
        return connection;
    }

    bool sendFrame (control::Type type, std::uint16_t flags, const std::vector<std::uint8_t>& payload)
    {
        auto c = connectionPtr();
        return c != nullptr && c->send (type, flags, 0, payload.data(), payload.size());
    }

    /** Sends a request; `done` gets its answer on the connection thread, or
        nullptr when the connection is lost before. False (and `done` never
        called) when it cannot be sent: callers may hold `lock`, which `done`
        usually takes. */
    bool sendAsync (control::Type type, std::uint16_t flags, const std::vector<std::uint8_t>& payload,
                    std::function<void (const control::Frame*)> done)
    {
        const auto id = nextRequestId++;
        std::shared_ptr<control::ControlConnection> c;
        {
            const std::lock_guard<std::mutex> l (connectionLock);
            c = connection;
            if (c != nullptr)
                pending[id] = done;
        }
        if (c != nullptr && c->send (type, flags, id, payload.data(), payload.size()))
            return true;

        const std::lock_guard<std::mutex> l (connectionLock);
        pending.erase (id);
        return false;
    }

    /** With `lock` held. */
    void sendSlotAdd (SlotModel& s, int index)
    {
        control::SlotAdd add;
        add.slotId = s.slotId;
        add.position = (std::uint32_t) serverPosition (index);
        add.uid = s.uid;
        add.name = s.name.toStdString();
        add.vendor = s.vendor.toStdString();
        add.state = s.state;
        s.onServer = true;

        const auto slotId = s.slotId;
        dirty |= ! sendAsync (control::Type::slotAdd, 0, encode (add), [this, slotId] (const control::Frame* f)
        {
            {
                const std::lock_guard<std::mutex> l (lock);
                auto* slot = find (slotId);
                if (f == nullptr)
                    dirty = true;   // lost: the next connection rebuilds
                else if (slot != nullptr && f->type == control::Type::slotReady)
                {
                    slot->loaded = true;
                    slot->error = {};
                    slot->info = control::SlotReady::fromVar (control::fromPayload (f->payload));
                    slot->box = control::toBox (slot->info);
                    slot->hasBox = true;
                    refreshRouting();
                }
                else if (slot != nullptr)
                {
                    slot->onServer = slot->loaded = false;
                    slot->error = control::ErrorMessage::fromVar (control::fromPayload (f->payload)).text;
                    refreshRouting();
                }
            }
            notify();
        });
    }

    /** With `lock` held: the routing's boxes, top to bottom. The model's:
        every slot whose buses are known, INPUTS and OUTPUTS at 64 channels.
        The server's: its INPUTS and OUTPUTS (CONFIGURE's counts) and the
        slots loaded there. */
    std::vector<chain::Box> routingBoxes (bool server) const
    {
        std::vector<chain::Box> b { chain::Box::makeInputs (server ? juce::jmax (1, host.numInputs) : wire::maxChannels) };
        for (auto& s : slots)
            if (s.hasBox && (! server || (s.onServer && s.loaded)))
                b.push_back (s.box);
        b.push_back (chain::Box::makeOutputs (server ? juce::jmax (1, host.numOutputs) : wire::maxChannels));
        return b;
    }

    /** With `lock` held: the model follows the chain, the server's routing
        is resolved from it, and sent when it changed (header comment). */
    void refreshRouting()
    {
        routing.setBoxes (routingBoxes (false));
        auto resolved = routing;
        resolved.setBoxes (routingBoxes (true));
        serverConnections = resolved.getConnections();
        routedInputs.store (serverRoutes ? resolved.getInputsUsed() : ~std::uint64_t (0));

        if (! ready || ! serverRoutes || (routingSent && serverConnections == lastRoutingSent))
            return;
        lastRoutingSent = serverConnections;
        routingSent = true;
        dirty |= ! sendAsync (control::Type::routing, 0, encode (control::toRoutes (serverConnections)),
                              [this] (const control::Frame* f)
        {
            if (f == nullptr || f->type == control::Type::error)
            {
                const std::lock_guard<std::mutex> l (lock);
                routingSent = false;   // sent again at the next change or connection
            }
        });
    }

    /** A routing edit, then refreshRouting(). */
    template <typename Edit>
    bool editRouting (Edit&& edit)
    {
        bool ok;
        {
            const std::lock_guard<std::mutex> l (lock);
            ok = edit();
            if (ok)
                refreshRouting();
        }
        if (ok)
            notify();
        return ok;
    }

    /** With `lock` held: SLOT_REMOVE, SLOT_MOVE or SLOT_BYPASS. */
    void sendSlotValue (control::Type type, std::uint32_t slotId, std::uint32_t value)
    {
        dirty |= ! sendAsync (type, 0, encode (control::SlotValue { slotId, value }), [this] (const control::Frame* f)
        {
            if (f == nullptr || f->type == control::Type::error)
            {
                const std::lock_guard<std::mutex> l (lock);
                dirty = true;   // out of step with the server: rebuild
            }
        });
    }

    //==============================================================================
    // Helpers, with `lock` held.

    int indexOf (std::uint32_t slotId) const
    {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (slots[i].slotId == slotId)
                return (int) i;
        return -1;
    }

    SlotModel* find (std::uint32_t slotId)
    {
        const int i = indexOf (slotId);
        return i >= 0 ? &slots[(std::size_t) i] : nullptr;
    }

    /** Where the slot at `index` of the model sits in the server's chain,
        which only has the slots it accepted. */
    int serverPosition (int index) const
    {
        int p = 0;
        for (int i = 0; i < index && i < (int) slots.size(); ++i)
            p += slots[(std::size_t) i].onServer ? 1 : 0;
        return p;
    }

    bool isDirty() const                { const std::lock_guard<std::mutex> l (lock); return dirty; }
    int currentTargetVersion() const    { const std::lock_guard<std::mutex> l (lock); return targetVersion; }
    juce::String statusTextCopy() const { return getStatusText(); }

    void setStatus (Status s, const juce::String& text)
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            if (status.load() == s && statusText == text)
                return;
            statusText = text;
        }
        status = s;
        notify();
    }

    void notify()
    {
        if (onStateChange)
            onStateChange();
    }

    //==============================================================================
    const juce::String clientName;

    mutable std::mutex lock;   ///< the chain, the settings, the status text, the target
    struct HostSettings { double sampleRate = 0.0; int maxBlock = 512, numInputs = 2, numOutputs = 2; };
    HostSettings host;
    Settings settings;
    juce::String targetHost;
    int targetPort = 0, targetVersion = 0;
    std::vector<SlotModel> slots;
    std::uint32_t nextSlotId = 1;
    std::uint64_t paramSeq = 0;
    bool dirty = false;
    std::atomic<bool> ready { false };
    std::vector<control::PluginEntry> catalog;

    // Routing, under `lock` (routedInputs and serverRoutes also read without).
    chain::Routing routing;
    std::vector<chain::Connection> serverConnections, lastRoutingSent;
    bool routingSent = false;
    std::atomic<bool> serverRoutes { false };
    std::atomic<std::uint64_t> routedInputs { ~std::uint64_t (0) };

    juce::String statusText { "Not connected" }, serverName, serverOs;
    std::atomic<Status> status { Status::idle };

    mutable std::mutex connectionLock;   ///< `connection` and `pending`
    std::shared_ptr<control::ControlConnection> connection;
    std::map<std::uint32_t, std::function<void (const control::Frame*)>> pending;
    std::atomic<std::uint32_t> nextRequestId { 1 };
    std::atomic<bool> reconfigureRequested { false };
    juce::WaitableEvent wake;

    // Connection thread only.
    std::uint32_t sessionId = 0;
    std::uint64_t token = 0;
    int udpPort = 54321;
    juce::String lastError;
    std::uint32_t linkGeneration = 0;
    std::unique_ptr<AudioLink> ownedLink;
    std::uint32_t configuredGeneration = 0;

    // Shared with the audio thread.
    std::atomic<AudioLink*> published { nullptr };
    std::atomic<AudioLink*> linkInUse { nullptr };
    AudioStatistics stats;
    TelemetryMailbox telemetry;
    std::atomic<std::uint32_t> controlRttMicros { 0 };
    std::atomic<std::uint32_t> resets { 0 };
    std::atomic<double> lastResetMs { 0.0 };
    std::atomic<bool> flushOnReset { true };
    std::atomic<std::uint32_t> hostAheadMicros { 0 };

    // Diagnostics (drainEvents): written by the audio thread only.
    static constexpr std::uint64_t eventCapacity = 256;
    std::array<AudioEvent, (std::size_t) eventCapacity> events;
    std::atomic<std::uint64_t> eventWrite { 0 };
    std::uint64_t eventRead = 0;   // message thread
    std::atomic<bool> prepared { false };
    std::atomic<bool> connectionWanted { false };
    std::atomic<std::uint32_t> prepareGeneration { 0 }, usableGeneration { 0 };

    // Audio thread (set up by prepare()).
    BlockStream stream;
    int maxInFlight = 16;
    std::vector<float> dry;
    std::vector<std::uint64_t> dryBlock, sentBlock;   ///< k + 1 per slot
    double audioSampleRate = 0.0;
    std::uint32_t audioGeneration = 0;
    std::uint32_t audioResets = 0;   ///< the reset count the last callback saw
    std::uint64_t callbackCount = 0;
    bool evPlaying = false;
    std::int64_t evNextTime = 0;

    // How far ahead of real time the host runs (measureAhead): the audio
    // delivered since an anchor, minus the wall-clock time since.
    bool aheadAnchored = false;
    juce::int64 aheadAnchorTicks = 0;
    double aheadDelivered = 0.0;   ///< seconds of audio since the anchor, before this callback
    std::uint32_t aheadResets = 0;
    bool aheadPlaying = false;
    std::int64_t aheadNextTime = 0;

    static constexpr int offlineWaitMs = 5000;
    int offlineWaitLeftMs = offlineWaitMs;

    JUCE_DECLARE_NON_COPYABLE (ClientEngine)
};

} // namespace fxmenet::client
