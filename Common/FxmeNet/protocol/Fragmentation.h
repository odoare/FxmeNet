/*
  ------------------------------------------------------------------------------
    Fragmentation.h

    Splitting a message into datagrams and putting it back together
    (protocol.md section 3). Every fragment but the last carries exactly
    `budget` body bytes, so a fragment's place in the message is
    fragment x budget: both sides use the session's budget (CONFIGURE).

    A message missing a fragment is dropped whole once a later message of
    the same kind completes, or when its slot is needed for a newer one.
    UDP-only: a stream transport never needs this (section 11).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeNet/protocol/Wire.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fxmenet::wire
{

//==============================================================================
/** Splits messages into datagrams. One instance per sending thread. */
class Fragmenter
{
public:
    explicit Fragmenter (std::size_t datagramBudget = defaultDatagramBudget)
        : budget (std::max<std::size_t> (1, datagramBudget))
    {
        scratch.reserve (headerSize + budget);
    }

    std::size_t getBudget() const noexcept  { return budget; }

    /** Calls send (const std::uint8_t* datagram, std::size_t size) -> bool for
        each fragment, in order. `header`'s fragment fields are filled in.
        False when the message needs more than 65535 fragments or a send
        fails. No allocation. */
    template <typename SendFunction>
    bool split (DatagramHeader header, const std::uint8_t* body, std::size_t size, SendFunction&& send)
    {
        const std::size_t count = size == 0 ? 1 : (size + budget - 1) / budget;
        if (count > 65535)
            return false;

        header.fragments = (std::uint16_t) count;
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t offset = i * budget;
            const std::size_t length = std::min<std::size_t> (budget, size - offset);

            scratch.clear();
            ByteWriter w (scratch);
            header.fragment = (std::uint16_t) i;
            writeHeader (w, header);
            if (length > 0)
                w.bytes (body + offset, length);

            if (! send (scratch.data(), scratch.size()))
                return false;
        }
        return true;
    }

private:
    std::size_t budget;
    std::vector<std::uint8_t> scratch;
};

//==============================================================================
/** Reassembles messages from datagrams. One instance per receiving thread
    and session; allocates only in its constructor. */
class Reassembler
{
public:
    /** A complete message. `body` stays valid until the next add(). */
    struct Message
    {
        DatagramHeader header;
        const std::uint8_t* body = nullptr;
        std::size_t size = 0;
    };

    struct Statistics
    {
        std::uint64_t completed  = 0;
        std::uint64_t incomplete = 0;   ///< dropped for a missing fragment
        std::uint64_t duplicates = 0;
        std::uint64_t malformed  = 0;
    };

    Reassembler (std::size_t datagramBudget, std::size_t maxMessageSize, int numSlots = 4)
        : budget (std::max<std::size_t> (1, datagramBudget)),
          maxSize (maxMessageSize),
          maxFragments ((maxMessageSize + budget - 1) / budget + 1),
          slots ((std::size_t) std::max<int> (1, numSlots))
    {
        for (auto& s : slots)
        {
            s.data.resize (maxSize);
            s.got.resize (maxFragments);
        }
    }

    /** Feeds one datagram. True when it completes a message, described in
        `out`. A single-fragment message points into `datagram` itself. */
    bool add (const std::uint8_t* datagram, std::size_t size, Message& out)
    {
        ByteReader r (datagram, size);
        DatagramHeader h;
        if (size < headerSize || ! readHeader (r, h))
        {
            ++stats.malformed;
            return false;
        }

        const std::uint8_t* body = datagram + headerSize;
        const std::size_t length = size - headerSize;

        if (h.fragments == 1)
        {
            dropOlderThan (h.kind, h.messageId);
            out = { h, body, length };
            ++stats.completed;
            return true;
        }

        const bool last = h.fragment == h.fragments - 1;
        const std::size_t offset = (std::size_t) h.fragment * budget;
        if (h.fragments > maxFragments || (! last && length != budget) || length > budget
            || offset + length > maxSize)
        {
            ++stats.malformed;
            return false;
        }

        Slot& s = slotFor (h);
        if (s.got[h.fragment])
        {
            ++stats.duplicates;
            return false;
        }

        std::copy (body, body + length, s.data.begin() + (std::ptrdiff_t) offset);
        s.got[h.fragment] = 1;
        if (last)
            s.lastLength = length;

        if (++s.received < s.fragments)
            return false;

        s.used = false;
        dropOlderThan (h.kind, h.messageId);
        out = { s.header, s.data.data(), (std::size_t) (s.fragments - 1) * budget + s.lastLength };
        ++stats.completed;
        return true;
    }

    const Statistics& getStatistics() const noexcept  { return stats; }

private:
    struct Slot
    {
        bool used = false;
        DatagramHeader header;
        std::uint16_t fragments = 0;
        std::uint16_t received = 0;
        std::size_t lastLength = 0;
        std::uint64_t started = 0;
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> got;
    };

    /** a is older than b, allowing for message id wrap-around. */
    static bool isOlder (std::uint32_t a, std::uint32_t b) noexcept
    {
        return (std::int32_t) (a - b) < 0;
    }

    Slot& slotFor (const DatagramHeader& h)
    {
        for (auto& s : slots)
            if (s.used && s.header.kind == h.kind && s.header.messageId == h.messageId)
                return s;

        Slot* chosen = nullptr;
        for (auto& s : slots)
            if (! s.used)
            {
                chosen = &s;
                break;
            }

        if (chosen == nullptr)   // all busy: give up the oldest incomplete message
        {
            chosen = &slots.front();
            for (auto& s : slots)
                if (s.started < chosen->started)
                    chosen = &s;
            ++stats.incomplete;
        }

        chosen->used = true;
        chosen->header = h;
        chosen->fragments = h.fragments;
        chosen->received = 0;
        chosen->lastLength = 0;
        chosen->started = ++clock;
        std::fill (chosen->got.begin(), chosen->got.begin() + h.fragments, std::uint8_t (0));
        return *chosen;
    }

    void dropOlderThan (Kind kind, std::uint32_t messageId)
    {
        for (auto& s : slots)
            if (s.used && s.header.kind == kind && isOlder (s.header.messageId, messageId))
            {
                s.used = false;
                ++stats.incomplete;
            }
    }

    std::size_t budget, maxSize, maxFragments;
    std::vector<Slot> slots;
    std::uint64_t clock = 0;
    Statistics stats;
};

} // namespace fxmenet::wire
