/*
  ------------------------------------------------------------------------------
    SystemLoad.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "SystemLoad.h"

#include <algorithm>
#include <thread>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <psapi.h>
#else
 #include <sys/resource.h>
 #include <unistd.h>
 #if defined (__APPLE__)
  #include <mach/mach.h>
 #else
  #include <fstream>
  #include <sstream>
  #include <string>
 #endif
#endif

namespace fxmenet::server
{

namespace
{
#if defined (_WIN32)
    std::uint64_t ticksOf (const FILETIME& t)
    {
        return (static_cast<std::uint64_t> (t.dwHighDateTime) << 32) | t.dwLowDateTime;
    }
#endif
}

//==============================================================================
bool SystemLoad::readTicks (Ticks& out)
{
    out.busy.clear();
    out.total.clear();

#if defined (_WIN32)
    FILETIME idle {}, kernel {}, user {};
    if (! GetSystemTimes (&idle, &kernel, &user))
        return false;
    const auto total = ticksOf (kernel) + ticksOf (user);   // kernel time includes idle
    out.total.push_back (total);
    out.busy.push_back (total - ticksOf (idle));
    return true;

#elif defined (__APPLE__)
    natural_t numCpus = 0;
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t infoCount = 0;
    if (host_processor_info (mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &numCpus, &info, &infoCount) != KERN_SUCCESS)
        return false;

    out.busy.assign (1, 0);
    out.total.assign (1, 0);
    for (natural_t i = 0; i < numCpus; ++i)
    {
        const auto* t = reinterpret_cast<const unsigned int*> (info) + CPU_STATE_MAX * i;
        const std::uint64_t busy = (std::uint64_t) t[CPU_STATE_USER] + t[CPU_STATE_SYSTEM] + t[CPU_STATE_NICE];
        const std::uint64_t total = busy + t[CPU_STATE_IDLE];
        out.busy.push_back (busy);
        out.total.push_back (total);
        out.busy[0] += busy;
        out.total[0] += total;
    }
    vm_deallocate (mach_task_self(), reinterpret_cast<vm_address_t> (info), infoCount * sizeof (integer_t));
    return true;

#else
    // "cpu" then "cpu0", "cpu1"...: user nice system idle iowait irq softirq steal.
    std::ifstream stat ("/proc/stat");
    std::string line;
    while (std::getline (stat, line))
    {
        if (line.compare (0, 3, "cpu") != 0)
            break;
        std::istringstream fields (line);
        std::string name;
        fields >> name;
        std::uint64_t v[8] {};
        for (auto& x : v)
            fields >> x;
        const auto idle = v[3] + v[4];
        std::uint64_t total = 0;
        for (auto x : v)
            total += x;
        out.busy.push_back (total - idle);
        out.total.push_back (total);
    }
    return ! out.total.empty();
#endif
}

double SystemLoad::processCpuSeconds()
{
#if defined (_WIN32)
    FILETIME created {}, exited {}, kernel {}, user {};
    if (! GetProcessTimes (GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0.0;
    return (double) (ticksOf (kernel) + ticksOf (user)) * 1.0e-7;   // 100 ns units
#else
    rusage u {};
    if (getrusage (RUSAGE_SELF, &u) != 0)
        return 0.0;
    return (double) u.ru_utime.tv_sec + (double) u.ru_stime.tv_sec
         + ((double) u.ru_utime.tv_usec + (double) u.ru_stime.tv_usec) * 1.0e-6;
#endif
}

std::uint64_t SystemLoad::residentBytes()
{
#if defined (_WIN32)
    PROCESS_MEMORY_COUNTERS counters {};
    if (GetProcessMemoryInfo (GetCurrentProcess(), &counters, sizeof (counters)))
        return counters.WorkingSetSize;
    return 0;
#elif defined (__APPLE__)
    mach_task_basic_info_data_t info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info (mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t> (&info), &count) == KERN_SUCCESS)
        return info.resident_size;
    return 0;
#else
    std::ifstream statm ("/proc/self/statm");
    std::uint64_t size = 0, resident = 0;
    if (statm >> size >> resident)
        return resident * (std::uint64_t) sysconf (_SC_PAGESIZE);
    return 0;
#endif
}

//==============================================================================
SystemLoad::Sample SystemLoad::sample()
{
    Sample s;
    s.numCores = (int) std::thread::hardware_concurrency();
    s.residentBytes = residentBytes();

    const auto now = std::chrono::steady_clock::now();
    const auto cpu = processCpuSeconds();
    Ticks ticks;
    const bool haveTicks = readTicks (ticks);

    if (primed)
    {
        const double wall = std::chrono::duration<double> (now - lastWall).count();
        if (wall > 0.0)
            s.processPercent = 100.0 * (cpu - lastCpuSeconds) / wall;

        if (haveTicks && ticks.total.size() == last.total.size())
        {
            for (std::size_t i = 0; i < ticks.total.size(); ++i)
            {
                // Linux's iowait may step back: differences are clamped.
                const auto total = (double) ((std::int64_t) (ticks.total[i] - last.total[i]));
                const auto busy = (double) ((std::int64_t) (ticks.busy[i] - last.busy[i]));
                const double percent = total > 0.0 ? 100.0 * std::clamp (busy / total, 0.0, 1.0) : 0.0;
                if (i == 0)
                    s.totalPercent = percent;
                else
                    s.corePercent.push_back (percent);
            }
        }
    }

    lastWall = now;
    lastCpuSeconds = cpu;
    if (haveTicks)
        last = std::move (ticks);
    primed = true;
    return s;
}

} // namespace fxmenet::server
