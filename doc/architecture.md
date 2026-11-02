# FxmeNet architecture

FxmeNet runs the DSP of FX-Mechanics plugins on a headless server on the LAN
while their real editors stay in the DAW. The design, its rationale and the
phased plan are in [`../plan.md`](../plan.md); this file describes what exists
in the repository, and is kept in sync with it.

## Repository layout

```
CMakeLists.txt          root project; JUCE and FxmeTools via cmake/Dependencies.cmake
cmake/FxmeDeps.cmake    shared resolver (sibling clone or pinned commit), identical
                        in every FX-Mechanics project: copy, never edit here only
cmake/Dependencies.cmake  pins: JUCE 9.0.4, FxmeTools 1a767df
Spikes/                 Phase 0 throwaway programs (FXMENET_BUILD_SPIKES, ON for now)
  HeadlessHost/         spike 1: GUI-free VST3 hosting, WAV through a plugin,
                        state round trip
doc/                    this file, todo.md, done.md
```

Planned targets (not yet present): `fxmenet-server` (Server/),
`FxmeNetPlugin` (Plugin/), the shared protocol code (Common/), and the howto
plugin `Examples/GainPan`. The plugin-side classes (`TelemetryRegistry`,
`NetBridge`) go into FxmeTools, not here.

## Dependencies

JUCE and FxmeTools are sibling clones (`~/src/JUCE`, `~/src/FxmeTools`) when
present, otherwise the pinned commits fetched by FetchContent. Release builds
configure with `-DFXME_USE_SIBLINGS=OFF`. The configure step prints where each
one came from and whether it matches its pin.

## Spike 1: HeadlessHost

`FxmeNetSpikeHeadless <plugin.vst3> <in.wav> <out.wav> [blockSize] [settleMs]`
(in `build/Spikes/HeadlessHost/FxmeNetSpikeHeadless_artefacts/Release/`) links
only `juce_audio_processors_headless`, `juce_audio_formats`, `juce_core` and
`juce_events` (no GUI module), the same stack the server will use. It reports
the GUI libraries the plugin binary pulls into the process (Linux), what a
tier 0 client would be told (parameters, latency, channels, whether the VST3
`IComponent` is reachable for the side channel), processing speed, and whether
a fresh instance loaded with the saved state gives the same output. Before
processing it runs the JUCE message loop for `settleMs` (default 1000), so
timers and background work in the plugin get to run, as on the server.
