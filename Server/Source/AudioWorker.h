/*
  ------------------------------------------------------------------------------
    AudioWorker.h

    A session's audio thread (protocol.md section 5). The UDP thread hands it
    complete audio requests through a lock-free queue; it puts them in order,
    processes each block once through the chain, and sends the reply to the
    client, keeping the last 32 replies so a duplicate or a retry is answered
    from the cache instead of being processed twice.

      - Real time (no offline flag): a gap is not waited for, the late block
        is lost (the client conceals it); an old block is answered from the
        cache or dropped.
      - Offline: a block ahead of the expected one is held until the missing
        one arrives (the client retries it), for at most 5 s.

    No allocation, lock or log on this thread once started: every buffer is
    sized in start() from the session's CONFIGURE.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include "Chain.h"
#include "UdpPeer.h"

#include <FxmeNet/control/ControlMessages.h>
#include <FxmeNet/protocol/Fragmentation.h>

#include <array>
#include <atomic>
#include <vector>

namespace fxmenet::server
{

class AudioWorker : private juce::Thread
{
public:
    struct Statistics
    {
        std::atomic<std::uint64_t> processed { 0 };
        std::atomic<std::uint64_t> lostBlocks { 0 };     ///< gaps skipped in real time
        std::atomic<std::uint64_t> fromCache { 0 };      ///< duplicates and retries answered from the cache
        std::atomic<std::uint64_t> stale { 0 };          ///< too old for the cache, dropped
        std::atomic<std::uint64_t> queueFull { 0 };
        std::atomic<std::uint64_t> malformed { 0 };
        std::atomic<std::uint64_t> sendErrors { 0 };
        std::atomic<std::uint32_t> lastProcessMicros { 0 };
        std::atomic<std::uint32_t> maxProcessMicros { 0 };
    };

    AudioWorker (Chain& chainToProcess, const juce::DatagramSocket& sharedSocket);
    ~AudioWorker() override;

    /** Sizes every buffer for `config` and starts the thread. Message
        thread, with the worker stopped. */
    void start (const control::Configure& config, std::uint32_t sessionId, std::uint64_t token);

    /** Stops the thread. Message thread. */
    void stop();

    bool isRunning() const      { return isThreadRunning(); }

    /** UDP thread, on every datagram of the session: follows the client's
        address. A change (a client reconnecting with a new socket after a
        resume) is taken at most once a second; the address is double
        buffered, and a sender holds it only for one sendto, so switching
        never touches the one in use. */
    void updatePeer (const juce::String& host, int port);

    /** The client's current address, or nullptr before its first datagram. */
    const UdpPeer* getPeer() const noexcept
    {
        const int i = activePeer.load();
        return i < 0 ? nullptr : &peers[(std::size_t) i];
    }

    /** UDP thread: queues a complete request (copied). False when the queue
        is full or the request is too big for the session's format. */
    bool enqueue (std::uint16_t flags, const std::uint8_t* body, std::size_t size);

    const Statistics& getStatistics() const noexcept   { return stats; }

    /** The largest request the session's format allows (for the reassembler). */
    static std::size_t maxRequestSize (const control::Configure&);

private:
    struct Entry
    {
        std::uint16_t flags = 0;
        std::vector<std::uint8_t> body;
        std::size_t size = 0;
    };

    struct Held
    {
        bool used = false;
        std::uint64_t block = 0;
        std::uint32_t since = 0;
        Entry entry;
    };

    struct Cached
    {
        bool valid = false;
        std::uint64_t block = 0;
        std::vector<std::uint8_t> body;
    };

    /** Queue length: half a second of blocks, so a host rendering ahead in
        bursts (Reaper's anticipative FX processing) does not overflow it;
        within 32 MB of reserved requests, and never under 16. */
    static int queueLengthFor (const control::Configure&);
    static constexpr int holdSize = 16;
    static constexpr int cacheSize = 32;
    static constexpr std::uint32_t holdTimeoutMs = 5000;

    void run() override;
    void handle (const Entry&);
    void processBlock (const wire::BlockView&, std::uint16_t flags);
    void send (const std::vector<std::uint8_t>& body);
    void drainHeld();
    void releaseStaleHeld();
    void hold (const Entry&, std::uint64_t block);

    Chain& chain;
    const juce::DatagramSocket& socket;
    control::Configure config;
    std::uint32_t sessionId = 0;
    std::uint64_t token = 0;

    std::array<UdpPeer, 2> peers;
    std::atomic<int> activePeer { -1 };
    juce::uint32 lastPeerChange = 0;   // UDP thread

    juce::AbstractFifo fifo { 16 };   // resized by start()
    std::vector<Entry> queue;
    juce::WaitableEvent wake;

    std::array<Held, holdSize> held;
    std::array<Cached, cacheSize> cache;
    bool started = false;
    std::uint64_t expected = 0;

    juce::AudioBuffer<float> work;
    juce::MidiBuffer midi;
    std::vector<std::uint8_t> midiOut;
    std::vector<const float*> outputs;
    wire::Fragmenter fragmenter;
    std::uint32_t nextMessageId = 1;

    Statistics stats;

    JUCE_DECLARE_NON_COPYABLE (AudioWorker)
};

} // namespace fxmenet::server
