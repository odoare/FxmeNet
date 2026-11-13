/*
  ------------------------------------------------------------------------------
    ControlFrame.h

    The control channel (protocol.md section 4): TCP frames, the message
    types, and the binary payloads. JSON payloads (HELLO, WELCOME, CONFIGURE,
    ERROR, PLUGIN_LIST, SLOT_READY) are in ControlMessages.h, on the JUCE
    side, because they use juce::JSON.

    Frame:  u32 length (of what follows)  u16 type  u16 flags  u32 requestId
            payload (length - 8 bytes)

    FrameAssembler cuts a byte stream into frames, whatever the reads'
    boundaries, and refuses frames over a size limit (a corrupt length would
    otherwise make it wait forever, or allocate without bound).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/net/TelemetryCodec.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fxmenet::control
{

using fxme::net::ByteReader;
using fxme::net::ByteWriter;

//==============================================================================
enum class Type : std::uint16_t
{
    hello           = 1,
    welcome         = 2,
    configure       = 3,
    ping            = 4,
    pong            = 5,
    error           = 6,
    bye             = 7,
    listPlugins     = 10,
    pluginList      = 11,
    slotAdd         = 20,
    slotReady       = 21,
    slotRemove      = 22,
    slotMove        = 23,
    slotBypass      = 24,
    chainClear      = 25,
    routing         = 26,
    paramSet        = 30,
    paramChanged    = 31,
    stateSet        = 32,
    stateGet        = 33,
    state           = 34,
    telemetryActive = 40,
    // JSFX scripts' @gfx display (Common/FxmeNet/jsfx/JsfxSync.h).
    jsfxSubscribe   = 41,
    jsfxSync        = 42,
    jsfxWrite       = 43,
    // A monitor connection (HELLO role "monitor", fxmenet-top): no session.
    monitorStatus   = 50,
    monitorPaths    = 51,
    monitorRescan   = 52
};

constexpr std::uint16_t flagJson = 1 << 0;

constexpr std::size_t frameHeaderSize = 12;   ///< length field + 8
constexpr std::size_t defaultMaxFrameSize = 64u << 20;   ///< plugin states with embedded audio can be large

struct Frame
{
    Type type = Type::bye;
    std::uint16_t flags = 0;
    std::uint32_t requestId = 0;
    std::vector<std::uint8_t> payload;

    bool isJson() const noexcept  { return (flags & flagJson) != 0; }
};

/** Appends one frame to `out`. */
inline void writeFrame (std::vector<std::uint8_t>& out, Type type, std::uint16_t flags,
                        std::uint32_t requestId, const std::uint8_t* payload, std::size_t size)
{
    ByteWriter w (out);
    w.u32 ((std::uint32_t) (size + 8));
    w.u16 ((std::uint16_t) type);
    w.u16 (flags);
    w.u32 (requestId);
    if (size > 0)
        w.bytes (payload, size);
}

//==============================================================================
/** Turns a byte stream into frames. */
class FrameAssembler
{
public:
    explicit FrameAssembler (std::size_t maxFrameSize = defaultMaxFrameSize) : maxSize (maxFrameSize) {}

    /** Appends bytes as they were read from the stream. */
    void append (const std::uint8_t* data, std::size_t size)
    {
        pending.insert (pending.end(), data, data + size);
    }

    enum class Result { frame, needMore, corrupt };

    /** Takes the next complete frame out of what was appended. After
        `corrupt` (a length below 8 or over the limit) the stream cannot be
        resynchronised: close the connection. */
    Result next (Frame& out)
    {
        if (pending.size() - start < 4)
        {
            compact();
            return Result::needMore;
        }

        ByteReader r (pending.data() + start, pending.size() - start);
        const std::size_t length = r.u32();
        if (length < 8 || length > maxSize)
            return Result::corrupt;
        if (pending.size() - start < 4 + length)
        {
            compact();
            return Result::needMore;
        }

        out.type = (Type) r.u16();
        out.flags = r.u16();
        out.requestId = r.u32();
        const auto* payload = r.take (length - 8);
        out.payload.assign (payload, payload + (length - 8));
        start += 4 + length;
        return Result::frame;
    }

private:
    /** Drops the bytes already consumed, once in a while. */
    void compact()
    {
        if (start > 0 && start * 2 >= pending.size())
        {
            pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) start);
            start = 0;
        }
    }

    std::size_t maxSize;
    std::vector<std::uint8_t> pending;
    std::size_t start = 0;
};

//==============================================================================
// Strings: u16 byte length + UTF-8.

inline void writeString (ByteWriter& w, const std::string& s)
{
    const auto n = std::min<std::size_t> (s.size(), 65535);
    w.u16 ((std::uint16_t) n);
    w.bytes (s.data(), n);
}

inline std::string readString (ByteReader& r)
{
    const auto n = r.u16();
    const auto* p = r.take (n);
    return p != nullptr ? std::string ((const char*) p, n) : std::string();
}

//==============================================================================
// Binary payloads (protocol.md section 4.2).

/** SLOT_ADD. */
struct SlotAdd
{
    std::uint32_t slotId = 0;
    std::uint32_t position = 0;
    std::uint32_t uid = 0;
    std::string name, vendor;
    std::vector<std::uint8_t> state;
};

inline std::vector<std::uint8_t> encode (const SlotAdd& m)
{
    std::vector<std::uint8_t> out;
    ByteWriter w (out);
    w.u32 (m.slotId);
    w.u32 (m.position);
    w.u32 (m.uid);
    writeString (w, m.name);
    writeString (w, m.vendor);
    w.u32 ((std::uint32_t) m.state.size());
    w.bytes (m.state.data(), m.state.size());
    return out;
}

inline bool decode (const std::vector<std::uint8_t>& in, SlotAdd& m)
{
    ByteReader r (in.data(), in.size());
    m.slotId = r.u32();
    m.position = r.u32();
    m.uid = r.u32();
    m.name = readString (r);
    m.vendor = readString (r);
    const auto n = r.u32();
    const auto* p = r.take (n);
    if (p != nullptr)
        m.state.assign (p, p + n);
    return r.ok();
}

/** SLOT_REMOVE, SLOT_MOVE (with position), SLOT_BYPASS and TELEMETRY_ACTIVE
    (with on): a slot and one number. */
struct SlotValue
{
    std::uint32_t slotId = 0;
    std::uint32_t value = 0;
};

inline std::vector<std::uint8_t> encode (const SlotValue& m)
{
    std::vector<std::uint8_t> out;
    ByteWriter w (out);
    w.u32 (m.slotId);
    w.u32 (m.value);
    return out;
}

inline bool decode (const std::vector<std::uint8_t>& in, SlotValue& m)
{
    ByteReader r (in.data(), in.size());
    m.slotId = r.u32();
    m.value = r.u32();
    return r.ok();
}

/** PARAM_SET and PARAM_CHANGED: a batch of normalised values. */
struct ParamChange
{
    std::uint32_t slotId = 0;
    std::uint32_t index = 0;
    float value = 0.0f;
};

inline std::vector<std::uint8_t> encode (const std::vector<ParamChange>& changes)
{
    std::vector<std::uint8_t> out;
    ByteWriter w (out);
    w.u32 ((std::uint32_t) changes.size());
    for (const auto& c : changes)
    {
        w.u32 (c.slotId);
        w.u32 (c.index);
        w.f32 (c.value);
    }
    return out;
}

inline bool decode (const std::vector<std::uint8_t>& in, std::vector<ParamChange>& changes)
{
    ByteReader r (in.data(), in.size());
    const auto n = r.u32();
    if ((std::size_t) n * 12 > r.remaining())
        return false;
    changes.resize (n);
    for (auto& c : changes)
    {
        c.slotId = r.u32();
        c.index = r.u32();
        c.value = r.f32();
    }
    return r.ok();
}

/** STATE_SET and STATE: a slot's state blob. STATE_GET: the slot alone. */
struct SlotState
{
    std::uint32_t slotId = 0;
    std::vector<std::uint8_t> state;
};

inline std::vector<std::uint8_t> encode (const SlotState& m)
{
    std::vector<std::uint8_t> out;
    ByteWriter w (out);
    w.u32 (m.slotId);
    w.u32 ((std::uint32_t) m.state.size());
    w.bytes (m.state.data(), m.state.size());
    return out;
}

inline bool decode (const std::vector<std::uint8_t>& in, SlotState& m)
{
    ByteReader r (in.data(), in.size());
    m.slotId = r.u32();
    const auto n = r.u32();
    const auto* p = r.take (n);
    if (p != nullptr)
        m.state.assign (p, p + n);
    return r.ok();
}

/** ROUTING (doc/routing.md): every connection of the graph, from an
    output pin to an input pin. Boxes are slot ids, plus routingInputs
    (FxmeNet's inputs, as outputs) and routingOutputs (its outputs, as
    inputs); channels are numbered across a box's buses, each bus counted at
    its full width (SLOT_READY's buses). */
constexpr std::uint32_t routingInputs = 0;
constexpr std::uint32_t routingOutputs = 0xffffffffu;

struct Route
{
    std::uint32_t fromBox = 0;
    std::uint16_t fromChannel = 0;
    std::uint32_t toBox = 0;
    std::uint16_t toChannel = 0;

    bool operator== (const Route& o) const noexcept
    {
        return fromBox == o.fromBox && fromChannel == o.fromChannel && toBox == o.toBox && toChannel == o.toChannel;
    }
};

inline std::vector<std::uint8_t> encode (const std::vector<Route>& routes)
{
    std::vector<std::uint8_t> out;
    ByteWriter w (out);
    w.u32 ((std::uint32_t) routes.size());
    for (const auto& c : routes)
    {
        w.u32 (c.fromBox);
        w.u16 (c.fromChannel);
        w.u32 (c.toBox);
        w.u16 (c.toChannel);
    }
    return out;
}

inline bool decode (const std::vector<std::uint8_t>& in, std::vector<Route>& routes)
{
    ByteReader r (in.data(), in.size());
    const auto n = r.u32();
    if ((std::size_t) n * 12 > r.remaining())
        return false;
    routes.resize (n);
    for (auto& c : routes)
    {
        c.fromBox = r.u32();
        c.fromChannel = r.u16();
        c.toBox = r.u32();
        c.toChannel = r.u16();
    }
    return r.ok();
}

/** PING and PONG: the sender's clock in microseconds, echoed. */
inline std::vector<std::uint8_t> encodePing (std::uint64_t micros)
{
    std::vector<std::uint8_t> out;
    ByteWriter w (out);
    w.u64 (micros);
    return out;
}

inline bool decodePing (const std::vector<std::uint8_t>& in, std::uint64_t& micros)
{
    ByteReader r (in.data(), in.size());
    micros = r.u64();
    return r.ok();
}

} // namespace fxmenet::control
