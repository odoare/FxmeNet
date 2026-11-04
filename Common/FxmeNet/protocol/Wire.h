/*
  ------------------------------------------------------------------------------
    Wire.h

    The FxmeNet audio-path messages (doc/protocol.md sections 3, 5 and 6):
    the UDP datagram header, and the bodies of the audio request, the audio
    reply and the telemetry message. Encoding is little-endian field by
    field (FxmeTools' ByteWriter / ByteReader), so client and server may run
    different OSes and builds.

    Bodies are defined independently of the datagram header: a TCP audio
    transport would carry the same bodies (protocol.md section 11). Nothing
    here assumes that one datagram holds one message.

    Real-time use: encoding appends to a std::vector the caller reserved once
    (no allocation after that); decoding reads in place and copies samples
    straight into the caller's buffers.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/net/TelemetryCodec.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fxmenet::wire
{

using fxme::net::ByteReader;
using fxme::net::ByteWriter;

//==============================================================================
// Datagram header (protocol.md section 3)

constexpr std::uint32_t udpMagic        = 0x414E5846u;   // bytes 'F' 'X' 'N' 'A'
constexpr std::uint8_t  protocolVersion = 1;
constexpr std::size_t   headerSize      = 28;

/** Default body bytes per datagram: 28 + 1400 stays under a 1500-byte MTU
    once IP and UDP headers are added. */
constexpr std::size_t   defaultDatagramBudget = 1400;

enum class Kind : std::uint8_t
{
    audioRequest = 1,
    audioReply   = 2,
    telemetry    = 3,
    keepAlive    = 4
};

/** Audio request flags. */
namespace requestFlags
{
    constexpr std::uint16_t offline   = 1 << 0;
    constexpr std::uint16_t retry     = 1 << 1;
    constexpr std::uint16_t transport = 1 << 2;

    /** The parity of the client's reset count (protocol.md section 5.1):
        the server resets every plugin of the chain before the first block
        whose parity differs from the previous block's. A parity rather than
        a one-off flag, so a lost request delays the reset by a block
        instead of losing it. */
    constexpr std::uint16_t resetParity = 1 << 3;
}

struct DatagramHeader
{
    Kind          kind       = Kind::keepAlive;
    std::uint16_t flags      = 0;
    std::uint32_t sessionId  = 0;
    std::uint64_t token      = 0;
    std::uint32_t messageId  = 0;
    std::uint16_t fragment   = 0;
    std::uint16_t fragments  = 1;
};

inline void writeHeader (ByteWriter& w, const DatagramHeader& h)
{
    w.u32 (udpMagic);
    w.u8 (protocolVersion);
    w.u8 ((std::uint8_t) h.kind);
    w.u16 (h.flags);
    w.u32 (h.sessionId);
    w.u64 (h.token);
    w.u32 (h.messageId);
    w.u16 (h.fragment);
    w.u16 (h.fragments);
}

/** False when the bytes are not an FxmeNet version 1 datagram. */
inline bool readHeader (ByteReader& r, DatagramHeader& h)
{
    if (r.u32() != udpMagic || r.u8() != protocolVersion)
        return false;
    h.kind      = (Kind) r.u8();
    h.flags     = r.u16();
    h.sessionId = r.u32();
    h.token     = r.u64();
    h.messageId = r.u32();
    h.fragment  = r.u16();
    h.fragments = r.u16();
    return r.ok() && h.fragments > 0 && h.fragment < h.fragments;
}

//==============================================================================
// Transport (the DAW's play head, protocol.md section 5.2)

struct Transport
{
    double        bpm               = 120.0;
    double        ppqPosition       = 0.0;
    double        ppqLastBarStart   = 0.0;
    std::int64_t  timeInSamples     = 0;
    double        loopStartPpq      = 0.0;
    std::uint16_t timeSigNumerator   = 4;
    std::uint16_t timeSigDenominator = 4;
    bool          playing   = false;
    bool          recording = false;
    bool          looping   = false;
};

constexpr std::size_t transportSize = 48;

inline void f64 (ByteWriter& w, double v)
{
    std::uint64_t bits;
    std::memcpy (&bits, &v, 8);
    w.u64 (bits);
}

inline double f64 (ByteReader& r)
{
    const std::uint64_t bits = r.u64();
    double v;
    std::memcpy (&v, &bits, 8);
    return v;
}

inline void writeTransport (ByteWriter& w, const Transport& t)
{
    f64 (w, t.bpm);
    f64 (w, t.ppqPosition);
    f64 (w, t.ppqLastBarStart);
    w.u64 ((std::uint64_t) t.timeInSamples);
    f64 (w, t.loopStartPpq);
    w.u16 (t.timeSigNumerator);
    w.u16 (t.timeSigDenominator);
    w.u8 ((std::uint8_t) ((t.playing ? 1 : 0) | (t.recording ? 2 : 0) | (t.looping ? 4 : 0)));
    w.u8 (0);
    w.u8 (0);
    w.u8 (0);
}

inline Transport readTransport (ByteReader& r)
{
    Transport t;
    t.bpm                = f64 (r);
    t.ppqPosition        = f64 (r);
    t.ppqLastBarStart    = f64 (r);
    t.timeInSamples      = (std::int64_t) r.u64();
    t.loopStartPpq       = f64 (r);
    t.timeSigNumerator   = r.u16();
    t.timeSigDenominator = r.u16();
    const auto bits      = r.u8();
    r.skip (3);
    t.playing   = (bits & 1) != 0;
    t.recording = (bits & 2) != 0;
    t.looping   = (bits & 4) != 0;
    return t;
}

//==============================================================================
// Channel masks

constexpr int maxChannels = 64;

inline int countChannels (std::uint64_t mask) noexcept
{
    int n = 0;
    for (; mask != 0; mask &= mask - 1)
        ++n;
    return n;
}

inline std::uint64_t maskOfFirst (int numChannels) noexcept
{
    return numChannels >= maxChannels ? ~std::uint64_t (0)
                                      : (std::uint64_t (1) << numChannels) - 1;
}

//==============================================================================
// Audio blocks (requests and replies share the layout of MIDI and audio)

/** The fields of an audio request or reply other than MIDI and samples. */
struct BlockInfo
{
    std::uint64_t block   = 0;
    std::uint16_t samples = 0;
    std::uint64_t mask    = 0;   ///< input mask (request) or output mask (reply)

    // Request only.
    bool      hasTransport = false;
    Transport transport;

    // Reply only.
    std::uint32_t chainLatency  = 0;
    std::uint32_t processMicros = 0;
};

/** One MIDI event as carried: up to 255 bytes. */
struct MidiEvent
{
    std::uint16_t       sampleOffset = 0;
    std::uint8_t        size = 0;
    const std::uint8_t* data = nullptr;
};

/** Packed MIDI events (u16 offset, u8 size, bytes), as in the message. */
class MidiWriter
{
public:
    explicit MidiWriter (std::vector<std::uint8_t>& destination) : out (destination) {}

    void clear()  { out.clear(); }

    /** False (nothing added) for an empty or longer-than-255-byte message. */
    bool add (int sampleOffset, const std::uint8_t* data, int size)
    {
        if (size <= 0 || size > 255)
            return false;
        ByteWriter w (out);
        w.u16 ((std::uint16_t) sampleOffset);
        w.u8 ((std::uint8_t) size);
        w.bytes (data, (std::size_t) size);
        return true;
    }

private:
    std::vector<std::uint8_t>& out;
};

/** Iterates over packed MIDI events in place. */
class MidiReader
{
public:
    MidiReader (const std::uint8_t* data, std::size_t size) : r (data, size) {}

    bool next (MidiEvent& e)
    {
        if (r.remaining() == 0)
            return false;
        e.sampleOffset = r.u16();
        e.size = r.u8();
        e.data = r.take (e.size);
        return r.ok() && e.data != nullptr;
    }

private:
    ByteReader r;
};

/** Writes an audio request (isReply false) or reply body. `channels` holds
    one pointer per bit set in info.mask, lowest channel first, each with
    info.samples samples. `midi` is packed (MidiWriter), may be empty. */
inline void writeBlock (ByteWriter& w, bool isReply, const BlockInfo& info,
                        const std::uint8_t* midi, std::size_t midiBytes,
                        const float* const* channels)
{
    w.u64 (info.block);
    w.u16 (info.samples);
    w.u64 (info.mask);
    if (isReply)
    {
        w.u32 (info.chainLatency);
        w.u32 (info.processMicros);
    }
    w.u16 ((std::uint16_t) midiBytes);
    if (! isReply && info.hasTransport)
        writeTransport (w, info.transport);
    if (midiBytes > 0)
        w.bytes (midi, midiBytes);

    const int n = countChannels (info.mask);
    for (int c = 0; c < n; ++c)
        for (int i = 0; i < info.samples; ++i)
            w.f32 (channels[c][i]);
}

/** A decoded audio block: the fields, and where MIDI and samples are in the
    message (valid while the message buffer is). */
struct BlockView
{
    BlockInfo info;
    const std::uint8_t* midi = nullptr;
    std::size_t midiBytes = 0;
    const std::uint8_t* audio = nullptr;   ///< f32 x samples per channel in the mask

    int numChannels() const noexcept  { return countChannels (info.mask); }

    /** Copies the `ordinal`-th channel present (0 = lowest set bit). */
    void copyChannel (int ordinal, float* dest) const noexcept
    {
        ByteReader r (audio + (std::size_t) ordinal * info.samples * 4, (std::size_t) info.samples * 4);
        for (int i = 0; i < info.samples; ++i)
            dest[i] = r.f32();
    }
};

/** Parses a request (isReply false, `flags` from its header) or a reply.
    False when the body is malformed or shorter than its fields say. */
inline bool readBlock (const std::uint8_t* body, std::size_t size, bool isReply,
                       std::uint16_t flags, BlockView& v)
{
    ByteReader r (body, size);
    v.info.block   = r.u64();
    v.info.samples = r.u16();
    v.info.mask    = r.u64();
    if (isReply)
    {
        v.info.chainLatency  = r.u32();
        v.info.processMicros = r.u32();
    }
    v.midiBytes = r.u16();
    v.info.hasTransport = ! isReply && (flags & requestFlags::transport) != 0;
    if (v.info.hasTransport)
        v.info.transport = readTransport (r);
    v.midi = v.midiBytes > 0 ? r.take (v.midiBytes) : nullptr;

    const std::size_t audioBytes = (std::size_t) v.numChannels() * v.info.samples * 4;
    v.audio = r.take (audioBytes);
    return r.ok() && (audioBytes == 0 || v.audio != nullptr);
}

//==============================================================================
// Telemetry (protocol.md section 6)

inline void writeTelemetry (ByteWriter& w, std::uint32_t slotId, std::uint32_t frame,
                            const std::uint8_t* codecFrame, std::size_t size)
{
    w.u32 (slotId);
    w.u32 (frame);
    w.bytes (codecFrame, size);
}

struct TelemetryView
{
    std::uint32_t slotId = 0;
    std::uint32_t frame  = 0;
    const std::uint8_t* codecFrame = nullptr;
    std::size_t size = 0;
};

inline bool readTelemetry (const std::uint8_t* body, std::size_t size, TelemetryView& v)
{
    ByteReader r (body, size);
    v.slotId = r.u32();
    v.frame  = r.u32();
    v.size   = r.remaining();
    v.codecFrame = r.take (v.size);
    return r.ok();
}

} // namespace fxmenet::wire
