/*
  ------------------------------------------------------------------------------
    SystemLoad.h

    The machine's CPU use (total and per core) and the server process's own
    (CPU and resident memory), sampled about once a second for fxmenet-top.
    JUCE-free; the platform code is in SystemLoad.cpp:

      Linux    /proc/stat, /proc/self/statm, getrusage
      macOS    host_processor_info, task_info, getrusage
      Windows  GetSystemTimes (no per-core figures), GetProcessTimes,
               GetProcessMemoryInfo

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace fxmenet::server
{

class SystemLoad
{
public:
    struct Sample
    {
        double processPercent = 0.0;        ///< like top: 100 per core fully used
        double totalPercent = -1.0;         ///< the machine, 0 to 100; -1: unknown
        std::vector<double> corePercent;    ///< each core, 0 to 100; empty: unknown
        std::uint64_t residentBytes = 0;    ///< 0: unknown
        int numCores = 0;
    };

    /** Use over the time since the previous call (zeros the first time).
        One thread. */
    Sample sample();

private:
    /** Busy and total ticks: [0] the whole machine, then each core. */
    struct Ticks
    {
        std::vector<std::uint64_t> busy, total;
    };

    static bool readTicks (Ticks&);
    static double processCpuSeconds();
    static std::uint64_t residentBytes();

    Ticks last;
    double lastCpuSeconds = 0.0;
    std::chrono::steady_clock::time_point lastWall;
    bool primed = false;
};

} // namespace fxmenet::server
