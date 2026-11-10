/*
  ------------------------------------------------------------------------------
    Log.h

    Timestamped lines on stdout (systemd's journal picks them up), and the
    last ones in memory for fxmenet-top (LogRing). Any thread except an
    audio thread: it locks and allocates. Audio threads count events in
    atomics instead, and the message thread reports them.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

#include <deque>
#include <iostream>
#include <mutex>
#include <vector>

namespace fxmenet::server
{

/** The last lines logged, numbered from 1, for monitors. */
class LogRing
{
public:
    static constexpr std::size_t capacity = 1000;

    static LogRing& instance()
    {
        static LogRing ring;
        return ring;
    }

    void add (const juce::String& line)
    {
        const std::lock_guard<std::mutex> guard (lock);
        lines.push_back (line);
        ++last;
        if (lines.size() > capacity)
            lines.pop_front();
    }

    /** The lines numbered `from` and after (at most `max`, the latest
        ones), and the number of the next line to ask for. */
    std::vector<juce::String> since (std::uint64_t from, std::size_t max, std::uint64_t& next) const
    {
        const std::lock_guard<std::mutex> guard (lock);
        next = last + 1;
        const auto first = last + 1 - lines.size();   // number of lines.front()
        from = std::max (from, first);
        if (from > last)
            return {};
        from = std::max (from, last + 1 - std::min<std::uint64_t> (max, last + 1 - from));
        return { lines.begin() + (std::ptrdiff_t) (from - first), lines.end() };
    }

private:
    mutable std::mutex lock;
    std::deque<juce::String> lines;
    std::uint64_t last = 0;   ///< number of the latest line, 0: none yet
};

inline void log (const juce::String& message)
{
    static std::mutex lock;
    const auto line = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S") + "  " + message;
    {
        const std::lock_guard<std::mutex> guard (lock);
        std::cout << line << std::endl;
    }
    LogRing::instance().add (line);
}

} // namespace fxmenet::server
