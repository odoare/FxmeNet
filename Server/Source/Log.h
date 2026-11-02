/*
  ------------------------------------------------------------------------------
    Log.h

    Timestamped lines on stdout (systemd's journal picks them up). Any thread
    except an audio thread: it locks and allocates. Audio threads count
    events in atomics instead, and the message thread reports them.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>

#include <iostream>
#include <mutex>

namespace fxmenet::server
{

inline void log (const juce::String& message)
{
    static std::mutex lock;
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S");
    const std::lock_guard<std::mutex> guard (lock);
    std::cout << stamp << "  " << message << std::endl;
}

} // namespace fxmenet::server
