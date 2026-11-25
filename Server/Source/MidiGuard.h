/*
  ------------------------------------------------------------------------------
    MidiGuard.h

    The notes a chain is holding, so that none keeps sounding after a break
    (doc/instrument-plan.md, phase 2). It follows the MIDI given to the chain
    (note on and off, the sustain pedal, all notes off) and, when asked,
    writes what releases it: a note-off for each note held, the pedal up,
    then all notes off (CC 123), on the channels that hold something.

    A break is any moment after which the note-offs the DAW sent may never
    reach the plugins: a reset of the client's plugin (transport jump, stop),
    the stream resuming after a gap (the client bypassed, or the network
    lost a stretch), a slot bypassed while it holds notes.

    Audio thread only; no allocation once the buffers given to it are sized
    (releaseSize() bytes at most).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <bitset>

namespace fxmenet::server
{

class MidiGuard
{
public:
    /** Follows the events about to be given to the chain. */
    void track (const juce::MidiBuffer& midi) noexcept
    {
        for (const auto m : midi)
        {
            if (m.numBytes < 2)
                continue;
            const auto status = m.data[0] & 0xf0;
            const auto channel = (std::size_t) (m.data[0] & 0x0f);
            const auto key = (std::size_t) (m.data[1] & 0x7f);
            const int value = m.numBytes > 2 ? m.data[2] : 0;
            if (status == 0x90 && value > 0)
                notes[channel].set (key);
            else if (status == 0x80 || status == 0x90)
                notes[channel].reset (key);
            else if (status == 0xb0 && key == 64)
                sustain.set (channel, value >= 64);
            else if (status == 0xb0 && (key == 123 || key == 120))
                notes[channel].reset();
        }
    }

    bool isHolding() const noexcept
    {
        if (sustain.any())
            return true;
        for (auto& n : notes)
            if (n.any())
                return true;
        return false;
    }

    /** Adds the events that release what is held, at `position`. The state
        is kept (the caller decides whether the release reaches every
        plugin, then calls clear()). */
    void addReleases (juce::MidiBuffer& out, int position) const
    {
        for (std::size_t c = 0; c < 16; ++c)
        {
            if (notes[c].none() && ! sustain[c])
                continue;
            const auto ch = (int) c + 1;
            for (int k = 0; k < 128; ++k)
                if (notes[c][(std::size_t) k])
                    out.addEvent (juce::MidiMessage::noteOff (ch, k), position);
            out.addEvent (juce::MidiMessage::controllerEvent (ch, 64, 0), position);
            out.addEvent (juce::MidiMessage::allNotesOff (ch), position);
        }
    }

    void clear() noexcept
    {
        for (auto& n : notes)
            n.reset();
        sustain.reset();
    }

    /** The most bytes addReleases may add to a MidiBuffer. */
    static constexpr int releaseSize() noexcept   { return 16 * (128 + 2) * 16; }

private:
    std::array<std::bitset<128>, 16> notes;
    std::bitset<16> sustain;
};

} // namespace fxmenet::server
