/*
  ------------------------------------------------------------------------------
    AudioTransport.h

    How the audio path sends and receives whole messages (audio requests and
    replies, telemetry), whatever carries them. The session, the jitter
    buffer and the audio code only ever see this interface and whole
    messages, never datagrams: fragmentation, retries and the reply cache are
    the UDP implementation's business. That is what keeps a TCP audio
    transport a small addition (protocol.md section 11).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeNet/protocol/Wire.h>

#include <cstddef>
#include <cstdint>

namespace fxmenet
{

/** A complete message received. `body` stays valid until the next receive()
    on the same transport. */
struct ReceivedMessage
{
    wire::Kind          kind  = wire::Kind::keepAlive;
    std::uint16_t       flags = 0;
    const std::uint8_t* body  = nullptr;
    std::size_t         size  = 0;
};

class AudioTransport
{
public:
    virtual ~AudioTransport() = default;

    /** Sends one message. One sending thread at a time. False on failure. */
    virtual bool send (wire::Kind kind, std::uint16_t flags,
                       const std::uint8_t* body, std::size_t size) = 0;

    /** Waits up to `timeoutMs` for the next complete message of this
        session. One receiving thread at a time. False on timeout or error. */
    virtual bool receive (ReceivedMessage& out, int timeoutMs) = 0;
};

} // namespace fxmenet
