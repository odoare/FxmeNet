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
cmake/Dependencies.cmake  pins: JUCE 9.0.4, FxmeTools 94e4e4d
Examples/GainPan/       FxmeNetGainPan: gain, balance, stereo in/out meters, the
                        full FX-Mechanics shell; the FxmeNet-aware howto plugin
Tools/Loopback/         FxmeNetLoopback: Phase 1 harness, source and mirror of one
                        plugin in one process, telemetry pumped in memory
Spikes/                 Phase 0 throwaway programs (FXMENET_BUILD_SPIKES, ON for now)
  HeadlessHost/         spike 1: GUI-free VST3 hosting, WAV through a plugin,
                        state round trip
doc/                    this file, todo.md, done.md
```

Planned targets (not yet present): `fxmenet-server` (Server/),
`FxmeNetPlugin` (Plugin/), the shared protocol code (Common/). The
plugin-side classes live in FxmeTools, not here: `net/TelemetryCodec.h`,
`net/TelemetryRegistry.h`, `net/NetAbi.h` in core, `net/NetBridge.h` in the
module, documented in FxmeTools `doc/fxmenet-howto.md`.

## GainPan (Examples/GainPan)

Stereo in, stereo out. Parameters: `gain` (-60 to +12 dB, default 0) and
`pan` (-1 to +1, default 0), a balance that lowers the far side linearly and
never boosts. Four `fxme::VuMeter`s (input and output, left and right), fed
in `processBlock` and drawn by two `fxme::StereoLevelMeter`s from a 30 Hz
editor timer, which reads and resets each held max. State: the APVTS XML
with a `stateVersion` attribute (1). Presets: `fxme::PresetManager` in
`FX-Mechanics/FxmeNetGainPan/Presets`, factory `Init` from `Source/Assets`.
Shell: `fxme::TopBar` with the preset bar and browser, a "?" switch
(`AppSettings.h`, machine-wide), info button, icon, splash (placeholder art).

FxmeNet: the processor owns an `fxme::net::NetBridge` (returned from
`getVST3ClientExtensions()`) and registers the four meters as `inL`, `inR`,
`outL`, `outR`. The editor has no FxmeNet code.

## Loopback harness (Tools/Loopback)

`FxmeNetLoopback [plugin.vst3]` (default `~/.vst3/FxmeNetGainPan.vst3`) loads
the plugin twice with JUCE's VST3 hosting, finds `INetPlugin` on each with
`fxme::net::queryNetPlugin (getVST3Client()->getIComponentPtr())`, sets the
roles before `prepareToPlay`, plays a test signal (two sines under slow
swells) through the source on a real-time-paced thread, and every 33 ms
reads one telemetry frame from the source and writes it into the mirror.
Parameters are copied both ways with an echo guard. Both editors are shown
side by side; the "Telemetry" switch stops the pump.

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
