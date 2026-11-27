/*
  ------------------------------------------------------------------------------
    LocalProcessor.h

    Processing on the DAW's computer in place of the server (local play,
    doc/fallback-plan.md). ClientEngine hands each block to it as it would
    send it to the server, at the same point of the stream, so the latency
    reported to the DAW does not change: the result is played when the
    server's reply would have been.

    JUCE-free interface; the plugin implements it (LocalChain).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace fxmenet::client
{

class LocalProcessor
{
public:
    virtual ~LocalProcessor() = default;

    /** Audio thread: one block of `numSamples` (the stream's N). `in` has
        `numInputs` channels, `out` `numOutputs`; `midi` is packed as on the
        wire (wire::MidiReader). `reset`: the plugin was reset by its host
        since the previous block (as the server's chain is). No allocation. */
    virtual void processLocalBlock (const float* const* in, int numInputs, float* const* out, int numOutputs,
                                    int numSamples, const std::uint8_t* midi, std::size_t midiBytes,
                                    bool offline, bool reset) = 0;

    /** Any thread: the local chain's latency, in samples (as the server's
        chainLatency). */
    virtual int getLocalLatency() const = 0;
};

} // namespace fxmenet::client
