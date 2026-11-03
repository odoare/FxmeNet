/*
  ------------------------------------------------------------------------------
    AudioLink.h

    One session's audio path on the client (protocol.md section 5): a UDP
    socket, the audio thread's sends, and a receive thread that puts replies
    into a ReplyRing. Created by the engine's connection thread once a
    session is configured, published to the audio thread, and replaced as a
    whole when the session changes (reconnection, new configuration): block
    numbers restart at 0 with each link.

    Audio thread: send() encodes the enabled input channels into a buffer
    reserved once and sends it (UdpTransport, sendto). The first datagram
    goes out from start(), on the connection thread: JUCE resolves and caches
    the server's address on the first write, so the audio thread never waits
    on a name lookup; that keep-alive also tells the server where to send
    telemetry before any audio flows. fetch() reads block k's reply; offline
    it waits for it, re-sending the request with the retry flag every 50 ms,
    for up to 5 s. In real time it waits too, but only until a deadline the
    engine sets per host callback (a fraction of the callback's duration):
    hosts that render ahead, like Reaper's anticipative FX processing, call
    the plugin in bursts much faster than real time, so a block can be
    needed long before its latency has elapsed in wall-clock time.

    The last requests are kept (one per block in flight) for those retries.

    Block numbers on the wire only ever increase within a link (the server
    drops anything older than what it expects): when the engine restarts its
    stream at block 0 on the same link (the host prepared again), rebase()
    makes stream block k go out as a number past everything sent so far.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

#include <FxmeNet/client/ReplyRing.h>
#include <FxmeNet/control/ControlMessages.h>
#include <FxmeNet/transport/UdpTransport.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace fxmenet::client
{

/** Audio-path statistics, kept by the engine across links. Any thread. */
struct AudioStatistics
{
    std::atomic<std::uint64_t> sent { 0 }, retries { 0 }, received { 0 }, refused { 0 }, telemetry { 0 };
    std::atomic<std::uint64_t> offlineTimeouts { 0 };
    std::atomic<std::uint64_t> rescued { 0 };   ///< replies late for their block but caught by the real-time wait
    std::atomic<std::uint32_t> lastRttMicros { 0 }, maxRttMicros { 0 };
    std::atomic<std::uint32_t> processMicros { 0 };
    std::atomic<int> chainLatency { 0 };   ///< from the latest reply

    void resetMaxima() noexcept   { maxRttMicros.store (0); }
};

/** Telemetry frames (TelemetryCodec frames, protocol.md section 6) from the
    receive thread to whoever feeds the mirrors (the message thread). A few
    frames are queued per slot, in order: tap channels (spectra, waveforms)
    carry the samples since the previous frame, so dropping one leaves a
    gap. Owned by the engine, shared by its links. */
class TelemetryMailbox
{
public:
    static constexpr std::size_t maxQueued = 16;   ///< per slot; the oldest go first

    /** Receive thread. */
    void push (std::uint32_t slotId, const std::uint8_t* frame, std::size_t size)
    {
        const std::lock_guard<std::mutex> l (lock);
        auto& queue = frames[slotId];
        if (queue.size() >= maxQueued)
            queue.erase (queue.begin());
        queue.emplace_back (frame, frame + size);
    }

    /** Message thread: every frame queued, by slot, oldest first. */
    std::map<std::uint32_t, std::vector<std::vector<std::uint8_t>>> take()
    {
        std::map<std::uint32_t, std::vector<std::vector<std::uint8_t>>> out;
        const std::lock_guard<std::mutex> l (lock);
        out.swap (frames);
        return out;
    }

private:
    std::mutex lock;
    std::map<std::uint32_t, std::vector<std::vector<std::uint8_t>>> frames;
};

class AudioLink : private juce::Thread
{
public:
    struct Settings
    {
        juce::String host;
        int udpPort = 54321;
        std::uint32_t sessionId = 0;
        std::uint64_t token = 0;
        control::Configure config;   ///< as sent: block size, masks, budget
        int numInputs = 2;           ///< host channels given to send()
        int numOutputs = 2;          ///< host channels fetch() fills
        int maxInFlight = 64;        ///< blocks sent and not yet played, at most
        std::uint32_t generation = 1;
    };

    static constexpr int retryIntervalMs = 50;
    static constexpr int offlineTimeoutMs = 5000;

    AudioLink (const Settings& s, AudioStatistics& statistics, TelemetryMailbox& telemetryMailbox)
        : juce::Thread ("FxmeNet audio receive"), settings (s), stats (statistics), mailbox (telemetryMailbox)
    {
        const auto& c = settings.config;
        n = c.blockSize;
        inputMask = c.inputMask & wire::maskOfFirst (settings.numInputs);
        slots = juce::jmax (4, settings.maxInFlight);

        const int channelsSent = wire::countChannels (inputMask);
        const std::size_t bodySize = 64 + wire::transportSize + (std::size_t) (channelsSent * n * 4);
        requests = std::make_unique<Request[]> ((std::size_t) slots);
        for (int i = 0; i < slots; ++i)
            requests[(std::size_t) i].body.reserve (bodySize);
        sendPointers.resize (wire::maxChannels, nullptr);

        ring.prepare (settings.numOutputs, n, juce::jmax (64, 2 * slots));
    }

    ~AudioLink() override
    {
        stopThread (2000);
    }

    /** Opens the socket, sends the first keep-alive, starts receiving.
        Connection thread. False when the socket cannot be opened. */
    bool start()
    {
        if (! socket.bindToPort (0))
            return false;

        UdpTransport::Settings t;
        t.sessionId = settings.sessionId;
        t.token = settings.token;
        t.datagramBudget = (std::size_t) settings.config.datagramBudget;
        t.maxMessageSize = 64 + (std::size_t) (wire::maxChannels * n * 4);
        transport = std::make_unique<UdpTransport> (socket, settings.host, settings.udpPort, t);

        if (! transport->send (wire::Kind::keepAlive, 0, nullptr, 0))
            return false;
        startThread (juce::Thread::Priority::high);
        return true;
    }

    std::uint32_t getGeneration() const noexcept    { return settings.generation; }
    const Settings& getSettings() const noexcept    { return settings; }

    //==============================================================================
    // Audio thread.

    /** Sends block k (`block` has numInputs channels of N samples).
        `transportInfo` may be null. */
    void send (std::uint64_t streamBlock, const float* const* block, const wire::Transport* transportInfo, bool offline)
    {
        const auto k = base + streamBlock;
        nextWire = juce::jmax (nextWire, k + 1);

        int count = 0;
        for (int c = 0; c < settings.numInputs && c < wire::maxChannels; ++c)
            if ((inputMask & (std::uint64_t (1) << c)) != 0)
                sendPointers[(std::size_t) count++] = block[c];

        auto& r = requests[(std::size_t) (k % (std::uint64_t) slots)];
        wire::BlockInfo info;
        info.block = k;
        info.samples = (std::uint16_t) n;
        info.mask = inputMask;
        info.hasTransport = transportInfo != nullptr;
        if (transportInfo != nullptr)
            info.transport = *transportInfo;

        r.body.clear();
        wire::ByteWriter w (r.body);
        wire::writeBlock (w, false, info, nullptr, 0, sendPointers.data());
        r.flags = (std::uint16_t) ((offline ? wire::requestFlags::offline : 0)
                                   | (transportInfo != nullptr ? wire::requestFlags::transport : 0));
        r.block.store (k + 1, std::memory_order_relaxed);
        r.sentTicks.store (juce::Time::getHighResolutionTicks(), std::memory_order_release);

        transport->send (wire::Kind::audioRequest, r.flags, r.body.data(), r.body.size());
        stats.sent.fetch_add (1, std::memory_order_relaxed);
    }

    /** Block k's reply into `dest` (numOutputs x N); false when missing.
        Offline, waits for it (retries every 50 ms, up to 5 s). In real time,
        waits until `deadlineTicks` (juce::Time high-resolution ticks; 0: no
        wait). */
    bool fetch (std::uint64_t streamBlock, float* const* dest, bool offline, juce::int64 deadlineTicks)
    {
        const auto k = base + streamBlock;
        if (! offline && deadlineTicks > 0 && ! ring.has (k))
        {
            const auto ticksPerMs = juce::Time::getHighResolutionTicksPerSecond() / 1000;
            for (;;)
            {
                const auto left = deadlineTicks - juce::Time::getHighResolutionTicks();
                if (ring.has (k) || left <= 0)
                    break;
                if (left >= ticksPerMs)
                    replyArrived.wait ((int) (left / ticksPerMs));
                else
                    juce::Thread::yield();   // under a millisecond: no timed wait that short
            }
            if (ring.has (k))
                stats.rescued.fetch_add (1, std::memory_order_relaxed);
        }

        if (offline && ! ring.has (k))
        {
            const auto start = juce::Time::getMillisecondCounter();
            auto lastSend = start;
            while (! ring.has (k))
            {
                const auto now = juce::Time::getMillisecondCounter();
                if (now - start > (juce::uint32) offlineTimeoutMs)
                {
                    stats.offlineTimeouts.fetch_add (1, std::memory_order_relaxed);
                    break;
                }
                if (now - lastSend >= (juce::uint32) retryIntervalMs)
                {
                    resend (k);
                    lastSend = now;
                }
                replyArrived.wait (10);
            }
        }
        return ring.read (k, dest);
    }

    /** Block k will not be read from this link (played dry): lets later
        replies in. */
    void skip (std::uint64_t streamBlock) noexcept     { ring.setNextRead (base + streamBlock + 1); }

    /** The engine's stream restarted at block 0: its blocks continue the
        numbering on the wire, and older replies are refused. */
    void rebase() noexcept
    {
        base = nextWire;
        ring.setNextRead (base);
    }

private:
    struct Request
    {
        std::vector<std::uint8_t> body;
        std::uint16_t flags = 0;
        std::atomic<std::uint64_t> block { 0 };       ///< k + 1, 0 when empty
        std::atomic<juce::int64> sentTicks { 0 };
    };

    void resend (std::uint64_t k)
    {
        auto& r = requests[(std::size_t) (k % (std::uint64_t) slots)];
        if (r.block.load (std::memory_order_relaxed) != k + 1)
            return;
        transport->send (wire::Kind::audioRequest, (std::uint16_t) (r.flags | wire::requestFlags::retry),
                         r.body.data(), r.body.size());
        stats.retries.fetch_add (1, std::memory_order_relaxed);
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            ReceivedMessage m;
            if (! transport->receive (m, 100))
                continue;

            if (m.kind == wire::Kind::telemetry)
            {
                wire::TelemetryView t;
                if (wire::readTelemetry (m.body, m.size, t))
                {
                    mailbox.push (t.slotId, t.codecFrame, t.size);
                    stats.telemetry.fetch_add (1, std::memory_order_relaxed);
                }
                continue;
            }

            wire::BlockView v;
            if (m.kind != wire::Kind::audioReply || ! wire::readBlock (m.body, m.size, true, m.flags, v))
                continue;

            if (! ring.write (v))
            {
                stats.refused.fetch_add (1, std::memory_order_relaxed);
                continue;
            }

            stats.received.fetch_add (1, std::memory_order_relaxed);
            stats.chainLatency.store ((int) v.info.chainLatency, std::memory_order_relaxed);
            stats.processMicros.store (v.info.processMicros, std::memory_order_relaxed);

            const auto& r = requests[(std::size_t) (v.info.block % (std::uint64_t) slots)];
            if (r.block.load (std::memory_order_relaxed) == v.info.block + 1)
            {
                const auto ticks = juce::Time::getHighResolutionTicks() - r.sentTicks.load (std::memory_order_acquire);
                const auto micros = (std::uint32_t) juce::jlimit (0.0, 4.0e9,
                                        juce::Time::highResolutionTicksToSeconds (ticks) * 1.0e6);
                stats.lastRttMicros.store (micros, std::memory_order_relaxed);
                if (micros > stats.maxRttMicros.load (std::memory_order_relaxed))
                    stats.maxRttMicros.store (micros, std::memory_order_relaxed);
            }
            replyArrived.signal();
        }
    }

    const Settings settings;
    AudioStatistics& stats;
    TelemetryMailbox& mailbox;
    std::uint64_t base = 0, nextWire = 0;   // audio thread
    int n = 128, slots = 64;
    std::uint64_t inputMask = 0;

    juce::DatagramSocket socket;
    std::unique_ptr<UdpTransport> transport;
    std::unique_ptr<Request[]> requests;   // atomics: not in a vector
    std::vector<const float*> sendPointers;
    ReplyRing ring;
    juce::WaitableEvent replyArrived;

    JUCE_DECLARE_NON_COPYABLE (AudioLink)
};

} // namespace fxmenet::client
