cmake_minimum_required(VERSION 3.22)

# Both of these must be set before project(): the compiler probes run there,
# so a CMAKE_OSX_ARCHITECTURES set afterwards gives a single-slice build that
# still calls itself universal. 10.13 rather than 11.0 (11.0 silently
# excludes Catalina).
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS deployment version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target architectures")
endif()

project(FxmeNet VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE and FxmeTools: sibling clones ../JUCE and ../FxmeTools when present,
# otherwise the commits pinned in cmake/Dependencies.cmake. Release builds use
# -DFXME_USE_SIBLINGS=OFF.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

enable_testing()

# ── Shared code: wire protocol, audio transports, their tests ─────────────────
add_subdirectory(Common)

# ── fxmenet-server ───────────────────────────────────────────────────────────
add_subdirectory(Server)

# ── The FxmeNet howto plugin (gain, balance, meters; FxmeNet-aware) ───────────
add_subdirectory(Examples/GainPan)

# ── Phase 1 harness: source and mirror of one plugin, telemetry in memory ─────
add_subdirectory(Tools/Loopback)

# ── Phase 2 end-to-end test: a WAV through a chain on the server ──────────────
add_subdirectory(Tools/WavClient)

# ── Phase 0 spikes (throwaway test programs, see doc/todo.md) ─────────────────
option(FXMENET_BUILD_SPIKES "Build the Phase 0 spike programs" ON)
if(FXMENET_BUILD_SPIKES)
    add_subdirectory(Spikes/HeadlessHost)
endif()
