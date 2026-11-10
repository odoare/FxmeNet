/*
  ------------------------------------------------------------------------------
    AudioWorker.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "AudioWorker.h"

namespace fxmenet::server
{

namespace
{
    constexpr std::size_t maxMidiBytes = wire::maxMidiBytes;

    void raiseTo (std::atomic<std::uint32_t>& a, std::uint32_t v) noexcept
    {
        auto current = a.load();
        while (v > current && ! a.compare_exchange_weak (current, v)) {}
    }
}

AudioWorker::AudioWorker (Chain& chainToProcess, const juce::DatagramSocket& sharedSocket)
    : juce::Thread ("FxmeNet audio"), chain (chainToProcess), socket (sharedSocket)
{
}

AudioWorker::~AudioWorker()
{
    stop();
}

std::size_t AudioWorker::maxRequestSize (const control::Configure& c)
{
    return 64 + wire::transportSize + maxMidiBytes
         + (std::size_t) juce::jmax (c.numInputs, c.numOutputs) * (std::size_t) c.blockSize * 4;
}

int AudioWorker::queueLengthFor (const control::Configure& c)
{
    const auto halfSecond = (int) std::ceil (0.5 * c.sampleRate / juce::jmax (1, c.blockSize));
    const auto byMemory = (int) ((32u << 20) / juce::jmax<std::size_t> (1, maxRequestSize (c)));
    return juce::jmax (16, juce::jmin (halfSecond, byMemory, 2048));
}

void AudioWorker::start (const control::Configure& c, std::uint32_t session, std::uint64_t sessionToken)
{
    jassert (! isThreadRunning());
    config = c;
    sessionId = session;
    token = sessionToken;

    const auto requestBytes = maxRequestSize (config);
    const auto replyBytes = requestBytes;   // same bound: outputs are as wide as inputs at most here

    // The UDP thread does not enqueue meanwhile: the session has no
    // reassembler while it reconfigures.
    const int length = queueLengthFor (config);
    fifo.setTotalSize (length + 1);   // AbstractFifo holds one less than its size
    fifo.reset();
    queue.resize ((std::size_t) (length + 1));
    for (auto& e : queue)
        e.body.reserve (requestBytes);
    for (auto& h : held)
    {
        h.used = false;
        h.entry.body.reserve (requestBytes);
    }
    for (auto& k : cache)
    {
        k.valid = false;
        k.body.reserve (replyBytes);
    }

    work.setSize (wire::maxChannels, config.blockSize);
    midi.ensureSize (maxMidiBytes);
    midiOut.reserve (maxMidiBytes);
    outputs.resize (wire::maxChannels);
    fragmenter = wire::Fragmenter ((std::size_t) config.datagramBudget);

    started = false;
    expected = 0;
    haveParity = false;   // the first block sets it: a stream starts with a fresh chain

    blockSeconds = config.blockSize / juce::jmax (1.0, config.sampleRate);

    realtime = startRealtimeThread (juce::Thread::RealtimeOptions{}.withPriority (8));
    if (! realtime)
        startThread (juce::Thread::Priority::highest);
}

void AudioWorker::stop()
{
    signalThreadShouldExit();
    wake.signal();
    stopThread (2000);
}

void AudioWorker::updatePeer (const juce::String& host, int port)
{
    const int current = activePeer.load();
    if (current >= 0 && peers[(std::size_t) current].getPort() == port && peers[(std::size_t) current].getHost() == host)
        return;

    const auto now = juce::Time::getMillisecondCounter();
    if (current >= 0 && now - lastPeerChange < 1000)
        return;

    const int next = current < 0 ? 0 : 1 - current;
    if (peers[(std::size_t) next].resolve (host, port))
    {
        activePeer.store (next);
        lastPeerChange = now;
    }
}

bool AudioWorker::enqueue (std::uint16_t flags, const std::uint8_t* body, std::size_t size)
{
    if (size > maxRequestSize (config))
    {
        stats.malformed.fetch_add (1);
        return false;
    }

    {
        const auto scope = fifo.write (1);   // committed when it goes out of scope
        if (scope.blockSize1 + scope.blockSize2 == 0)
        {
            stats.queueFull.fetch_add (1);
            return false;
        }

        auto& e = queue[(std::size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)];
        e.flags = flags;
        e.body.assign (body, body + size);   // within the reserved capacity: no allocation
        e.size = size;
        e.arrived = juce::Time::getHighResolutionTicks();
    }
    wake.signal();   // after the commit, or the worker could wake to an empty queue
    return true;
}

//==============================================================================
void AudioWorker::run()
{
    while (! threadShouldExit())
    {
        bool any = false;
        while (fifo.getNumReady() > 0 && ! threadShouldExit())
        {
            const auto scope = fifo.read (1);
            handle (queue[(std::size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)]);
            any = true;
        }

        releaseStaleHeld();
        if (! any)
            wake.wait (20);
    }
}

void AudioWorker::handle (const Entry& e)
{
    wire::BlockView v;
    if (! wire::readBlock (e.body.data(), e.size, false, e.flags, v) || v.info.samples != config.blockSize)
    {
        stats.malformed.fetch_add (1);
        return;
    }

    const auto block = v.info.block;
    const bool offline = (e.flags & wire::requestFlags::offline) != 0;

    if (! started)
    {
        started = true;
        expected = block;
    }

    if (block < expected)   // a duplicate, a retry, or late
    {
        auto& k = cache[(std::size_t) (block % cacheSize)];
        if (k.valid && k.block == block)
        {
            send (k.body);
            stats.fromCache.fetch_add (1);
        }
        else
        {
            stats.stale.fetch_add (1);
        }
        return;
    }

    if (block > expected && offline)
    {
        hold (e, block);
        return;
    }

    if (block > expected)
        stats.lostBlocks.fetch_add (block - expected);

    processBlock (v, e.flags, e.arrived);
    expected = block + 1;
    drainHeld();
}

void AudioWorker::hold (const Entry& e, std::uint64_t block)
{
    Held* freeSlot = nullptr;
    for (auto& h : held)
    {
        if (h.used && h.block == block)
            return;   // a retry of a block already held
        if (! h.used && freeSlot == nullptr)
            freeSlot = &h;
    }

    if (freeSlot == nullptr)
    {
        // Too many blocks waiting for one missing block: give up on it.
        std::uint64_t smallest = ~std::uint64_t (0);
        for (auto& h : held)
            smallest = juce::jmin (smallest, h.block);
        stats.lostBlocks.fetch_add (smallest - expected);
        expected = smallest;
        drainHeld();
        hold (e, block);
        return;
    }

    freeSlot->used = true;
    freeSlot->block = block;
    freeSlot->since = juce::Time::getMillisecondCounter();
    freeSlot->entry.flags = e.flags;
    freeSlot->entry.body.assign (e.body.begin(), e.body.begin() + (std::ptrdiff_t) e.size);
    freeSlot->entry.size = e.size;
    freeSlot->entry.arrived = e.arrived;
}

void AudioWorker::drainHeld()
{
    for (bool progressed = true; progressed;)
    {
        progressed = false;
        for (auto& h : held)
        {
            if (! h.used || h.block != expected)
                continue;

            h.used = false;
            wire::BlockView v;
            if (wire::readBlock (h.entry.body.data(), h.entry.size, false, h.entry.flags, v))
                processBlock (v, h.entry.flags, h.entry.arrived);
            expected = h.block + 1;
            progressed = true;
        }
    }
}

void AudioWorker::releaseStaleHeld()
{
    const auto now = juce::Time::getMillisecondCounter();
    std::uint64_t smallest = ~std::uint64_t (0);
    bool stale = false;
    for (auto& h : held)
        if (h.used)
        {
            smallest = juce::jmin (smallest, h.block);
            stale = stale || now - h.since > holdTimeoutMs;
        }

    if (stale)
    {
        stats.lostBlocks.fetch_add (smallest - expected);
        expected = smallest;
        drainHeld();
    }
}

//==============================================================================
void AudioWorker::processBlock (const wire::BlockView& v, std::uint16_t flags, std::int64_t arrived)
{
    const auto t0 = juce::Time::getHighResolutionTicks();
    const int n = config.blockSize;

    // The work buffer has all 64 channels: every slot takes the ones it
    // needs (Chain::process), and no AudioBuffer view over more than 32
    // channels (which would allocate) is made here.
    work.clear();

    // Inputs: each channel present in the mask, in order; absent ones stay silent.
    int ordinal = 0;
    for (int c = 0; c < wire::maxChannels; ++c)
        if ((v.info.mask >> c) & 1u)
            v.copyChannel (ordinal++, work.getWritePointer (c));

    midi.clear();
    wire::MidiReader reader (v.midi, v.midiBytes);
    wire::MidiEvent event;
    while (reader.next (event))
        midi.addEvent (event.data, event.size, juce::jlimit (0, n - 1, (int) event.sampleOffset));

    if (v.info.hasTransport)
        chain.getPlayHead().set (v.info.transport, config.sampleRate);

    // The client's plugin was reset (transport moved, host flush): blocks
    // come in order here, so a parity change marks the first block after it.
    const bool parity = (flags & wire::requestFlags::resetParity) != 0;
    const bool reset = haveParity && parity != lastParity;
    haveParity = true;
    lastParity = parity;
    if (reset)
        stats.resets.fetch_add (1);

    chain.process (work, midi, (flags & wire::requestFlags::offline) != 0, reset);

    // Reply, written straight into its cache slot.
    auto& k = cache[(std::size_t) (v.info.block % cacheSize)];
    k.valid = true;
    k.block = v.info.block;
    k.body.clear();

    midiOut.clear();
    wire::MidiWriter midiWriter (midiOut);
    for (const auto m : midi)
        if (midiOut.size() + 3 + (std::size_t) m.numBytes <= maxMidiBytes)
            midiWriter.add (m.samplePosition, m.data, m.numBytes);

    wire::BlockInfo info;
    info.block = v.info.block;
    info.samples = (std::uint16_t) n;
    info.mask = config.outputMask & chain.getOutputsFed();   // routed: unfed outputs are not sent
    info.chainLatency = (std::uint32_t) juce::jmax (0, chain.getLatency());

    int out = 0;
    for (int c = 0; c < wire::maxChannels; ++c)
        if ((info.mask >> c) & 1u)
            outputs[(std::size_t) out++] = work.getReadPointer (c);

    const auto micros = (std::uint32_t) (juce::Time::highResolutionTicksToSeconds (
                                             juce::Time::getHighResolutionTicks() - t0) * 1.0e6);
    info.processMicros = micros;

    wire::ByteWriter w (k.body);
    wire::writeBlock (w, true, info, midiOut.data(), midiOut.size(), outputs.data());
    send (k.body);

    stats.processed.fetch_add (1);
    stats.lastProcessMicros.store (micros);
    raiseTo (stats.maxProcessMicros, micros);
    raiseTo (stats.windowMaxProcessMicros, micros);

    // From arrival to reply sent: includes the wait behind earlier blocks,
    // which processing time alone does not show. Offline blocks may wait
    // on purpose (held for a missing one): not counted.
    if ((flags & wire::requestFlags::offline) == 0)
    {
        const auto turnaround = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - arrived);
        raiseTo (stats.windowMaxTurnaroundMicros, (std::uint32_t) (turnaround * 1.0e6));
        if (turnaround > blockSeconds)
            stats.overOneBlock.fetch_add (1);
        if (turnaround > 2.0 * blockSeconds)
            stats.overTwoBlocks.fetch_add (1);
    }
}

void AudioWorker::send (const std::vector<std::uint8_t>& body)
{
    const auto* peer = getPeer();
    if (peer == nullptr)
        return;

    wire::DatagramHeader h;
    h.kind = wire::Kind::audioReply;
    h.sessionId = sessionId;
    h.token = token;
    h.messageId = nextMessageId++;

    const bool ok = fragmenter.split (h, body.data(), body.size(), [this, peer] (const std::uint8_t* d, std::size_t size)
    {
        if (! peer->send (socket, d, size))
            return false;
        stats.bytesSent.fetch_add (size, std::memory_order_relaxed);
        return true;
    });
    if (! ok)
        stats.sendErrors.fetch_add (1);
}

} // namespace fxmenet::server
