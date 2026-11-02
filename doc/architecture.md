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
Common/                 shared by server and client, header-only:
  FxmeNet/protocol/     Wire.h (datagram header, audio request/reply, telemetry
                        bodies), Fragmentation.h (Fragmenter, Reassembler); JUCE-free
  FxmeNet/transport/    AudioTransport.h (the interface, JUCE-free),
                        UdpTransport.h (juce::DatagramSocket, one session)
  FxmeNet/control/      ControlFrame.h (frames, types, binary payloads, FrameAssembler;
                        JUCE-free), ControlMessages.h (JSON messages, juce::JSON),
                        ControlConnection.h (one TCP connection, juce::StreamingSocket)
  tests/                FxmeNetProtocolTests (no JUCE), FxmeNetTransportTests (loopback
                        UDP), FxmeNetControlTests (JSON, loopback TCP)
Server/Source/          fxmenet-server: Main (options, signals, message loop),
                        Server (listener, handshakes, UDP routing, orphans),
                        Session (control handling), Chain (slots, snapshots),
                        AudioWorker (ordering, reply cache, processing),
                        PluginCatalog, UdpPeer, Log
Examples/GainPan/       FxmeNetGainPan: gain, balance, stereo in/out meters, the
                        full FX-Mechanics shell; the FxmeNet-aware howto plugin
Tools/Loopback/         FxmeNetLoopback: Phase 1 harness, source and mirror of one
                        plugin in one process, telemetry pumped in memory
Spikes/                 Phase 0 throwaway programs (FXMENET_BUILD_SPIKES, ON for now)
  HeadlessHost/         spike 1: GUI-free VST3 hosting, WAV through a plugin,
                        state round trip
doc/                    this file, todo.md, done.md; protocol.md (the wire
                        protocol, draft); audiogridder-notes.md
```

Planned targets (not yet present): `FxmeNetPlugin` (Plugin/), the
command-line test client. The
plugin-side classes live in FxmeTools, not here: `net/TelemetryCodec.h`,
`net/TelemetryRegistry.h`, `net/NetAbi.h` in core, `net/NetBridge.h` in the
module, documented in FxmeTools `doc/fxmenet-howto.md`.

## Common (protocol and transport)

Implements `doc/protocol.md` sections 3, 5 and 6. `FxmeNetProtocol` is an
INTERFACE target (include root `Common/`, links FxmeCore for `ByteWriter` /
`ByteReader`). Encoding appends to a vector the caller reserved once, so it
does not allocate after that; decoding reads in place (`BlockView`,
`MidiReader`, `TelemetryView`) and `copyChannel` writes samples straight
into the caller's buffer. The `Reassembler` preallocates its slots (4
messages in progress, each up to the session's maximum message size) and
returns a single-datagram message without copying it.

`AudioTransport` is the seam a TCP audio transport would plug into: the
rest of the code sends and receives whole messages only. `UdpTransport` is
the client's side (one peer, one session); the server will share one socket
among its sessions and route datagrams by session id, reusing `Fragmenter`
and `Reassembler`.

The control channel (protocol.md section 4) follows the same split. Frames
and binary payloads are JUCE-free; the JSON messages are structs with
`toVar` / `fromVar` over `juce::JSON`, carrying 64-bit values and plugin
uids as hex strings, keeping defaults for missing fields and ignoring
unknown ones. `ControlConnection` sends from any thread (a lock keeps frames
whole) and reads from one, with a timeout; a corrupt length closes it, since
a stream cannot be resynchronised.

## fxmenet-server (Server/)

`fxmenet-server [--name NAME] [--control-port 54322] [--udp-port 54321]
[--plugin-path DIR]... [--no-default-paths]`. Links only the GUI-free JUCE
modules (`juce_audio_processors_headless`, `juce_core`, `juce_events`).

Threads:

- **Main thread = JUCE message thread**, running the dispatch loop until
  SIGINT/SIGTERM (a 10 Hz timer watches a flag the signal handler sets).
  Every plugin instance is created, configured and destroyed here, and
  plugins get their timers and async updates.
- **Accept thread** on the control port; each new connection's HELLO is read
  on a 2-thread pool (5 s timeout), then `Server::onHello` on the message
  thread creates a session or resumes an orphan (same id and token).
- **One reader thread per control connection**, posting each frame to the
  message thread (`Session::handleFrame`), tagged with a generation number
  so frames of a replaced connection are ignored.
- **One UDP thread** for the shared socket: parses the datagram header,
  finds the session by id under `sessionsLock`, checks the token, and calls
  `Session::onDatagram`, which reassembles and queues complete requests.
- **One audio thread per session** (`AudioWorker`, real-time priority when
  the OS allows it): takes requests from a lock-free queue (16 entries,
  preallocated from CONFIGURE), orders them (protocol.md 5.4: a real-time gap
  is skipped, an offline one is held up to 5 s; anything older is answered
  from the 32-reply cache or dropped), processes the chain and replies.
  Nothing allocates, locks or logs there; counters in atomics are logged
  every 10 s by the message thread.

Chain edits never stop the audio: the message thread publishes an immutable
snapshot of the slot list through an atomic pointer; the audio thread
announces the snapshot it uses in a second atomic (a hazard pointer) and
re-checks it is still current; a replaced snapshot is freed once the audio
thread has let go of it, so a removed plugin is destroyed on the message
thread. Each slot processes a view of the 64-channel work buffer with as
many channels as it has buses. Roles: an FxmeNet-aware plugin is set to
`source` before `prepareToPlay`.

Replies leave from the shared socket with `::sendto` on its raw handle and
the client's address resolved once (`UdpPeer`): JUCE's
`DatagramSocket::write` caches one destination without a lock, which would
race between sessions and resolve the address on almost every datagram.

Limits of this first version (doc/todo.md): in-process plugin scan; no
telemetry pump or discovery yet; `PARAM_CHANGED` not sent; a session's UDP
peer is fixed at its first datagram; `setNonRealtime` is not forwarded to
the plugins for offline blocks; catalog `sha256` empty and `fxmenetAbi`
unknown until a slot is loaded.

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
