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
  FxmeNet/client/       the client's audio-thread core, JUCE-free: BlockStream
                        (re-blocking, latency, jitter buffer, concealment),
                        ReplyRing (replies from the receive thread)
  tests/                FxmeNetProtocolTests and FxmeNetClientTests (no JUCE),
                        FxmeNetTransportTests (loopback UDP), FxmeNetControlTests
                        (JSON, loopback TCP)
Server/Source/          fxmenet-server: Main (options, signals, message loop),
                        Server (listener, handshakes, UDP routing, orphans),
                        Session (control handling), Chain (slots, snapshots),
                        AudioWorker (ordering, reply cache, processing),
                        PluginCatalog, UdpPeer, Log
Examples/GainPan/       FxmeNetGainPan: gain, balance, stereo in/out meters, the
                        full FX-Mechanics shell; the FxmeNet-aware howto plugin
Tools/Loopback/         FxmeNetLoopback: Phase 1 harness, source and mirror of one
                        plugin in one process, telemetry pumped in memory
Tools/WavClient/        fxmenet-wavclient: Phase 2 end-to-end test, a WAV through a
                        chain on the server, compared with local processing
Spikes/                 Phase 0 throwaway programs (FXMENET_BUILD_SPIKES, ON for now)
  HeadlessHost/         spike 1: GUI-free VST3 hosting, WAV through a plugin,
                        state round trip
doc/                    this file, todo.md, done.md; protocol.md (the wire
                        protocol, draft); audiogridder-notes.md
```

Planned target (not yet present): `FxmeNetPlugin` (Plugin/). The
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


## Client core (Common/FxmeNet/client)

The part of FxmeNetPlugin that runs on the DAW's audio thread, kept JUCE-free
so it is tested exhaustively without a host or a network
(`FxmeNetClientTests`).

`BlockStream` cuts the host's buffers into the session's N-sample blocks
and hands each full block to the network (`blockReady`, with the offset of
its end in the callback, for transport and MIDI). Output is a FIFO
prefilled with `latency` samples of silence and topped up one block at a
time (`fetchBlock`) only when the host needs the samples, so each reply has
as long as possible to arrive. The latency (protocol.md section 5.1) is
`D*N + B - N` while the host's blocks are multiples of N, `D*N + B + N - 1`
otherwise (B its largest block), which gives every reply at least D*N
samples of time; the test checks that bound, and that the output is the
input delayed by exactly the latency, for fixed, odd and random host block
sizes. Losing the alignment midway adds the difference once. A missing reply
is concealed: the last sample played is held and ramped to zero over 64
samples, then silence, and the next block fades in over 64 samples. Buffers
larger than the announced maximum are processed in pieces.

`ReplyRing` holds the replies between the receive thread and the audio
thread: block k in slot k mod 64, a tag per slot. The audio thread publishes
the next block it will read, and the receive thread refuses replies older
than that, too far ahead, or duplicated, so a slot being read is never
written; a seqlock check on the tag would still turn a broken assumption
into a missing block, never a torn one. Channels arrive by the reply's
output mask (bit c into channel c, missing ones silent).
## fxmenet-server (Server/)

`fxmenet-server [--name NAME] [--control-port 54322] [--udp-port 54321]
[--discovery-port 54320] [--no-discovery] [--plugin-path DIR]...
[--no-default-paths] [--rescan]`.

**Plugin catalog.** Each VST3 file is examined in a child process,
`fxmenet-server --scan-plugin FILE` (30 s timeout), which prints its
descriptions between markers, each with the plugin's FxmeNet ABI (it
instantiates the plugin once) and the sha256 of the binary for this
platform inside the bundle (`Contents/x86_64-linux/...`, `MacOS`,
`x86_64-win`). A plugin crashing or hanging while loaded is logged and
skipped. Results go to a cache (`FX-Mechanics/fxmenet-server-plugins.xml`
in the user's configuration folder) keyed by path, binary size and date, so
only new or changed plugins are examined at the next start; a failed file
stays skipped until it changes; `--rescan` ignores the cache. The catalog's
`platforms` uses the bundle folder names. Links only the GUI-free JUCE
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

**Telemetry and parameter changes.** Each session has a timer on the
message thread at its telemetry rate (CONFIGURE, default 30 Hz). For each
slot whose editor is open (`TELEMETRY_ACTIVE`, which also calls the
plugin's `setTelemetryActive`), it reads a frame through `INetPlugin` and
sends it as a telemetry message to the client's UDP address, fragmented
like any message. Running on the message thread keeps it from racing with
chain edits (slots are only touched there); a plugin blocking that thread
pauses telemetry, not audio. The same timer sends `PARAM_CHANGED`: each
slot's `ParamWatcher` listens to every parameter and, since it may be
called on any thread (the audio thread included), only sets per-parameter
atomics, which the timer collects into one batch. The watcher is created
after the slot's initial state is loaded, so that state is not echoed back.

**Discovery.** A thread on UDP 54320 answers `FXMENET_DISCOVER` broadcasts
with `FXMENET_ANNOUNCE` (name, OS, version, ports, session count), using
`Common/FxmeNet/control/Discovery.h`, shared with clients. A second server
on the same machine cannot bind the port and runs without it. mDNS is not
implemented yet.

**Offline blocks.** The request's offline flag reaches the plugins:
`Chain::process` calls `setNonRealtime` when it changes, which JUCE's VST3
host turns into the process mode of that very call.

**The client's address** follows the session: every datagram passes its
sender to `AudioWorker::updatePeer`, which switches to a new address (a
client back with a new socket after a resume) at most once a second. The
address is double buffered and a sender holds it for one `sendto` only.

## fxmenet-wavclient (Tools/WavClient)

`fxmenet-wavclient --in in.wav --out out.wav --plugin a.vst3 [--plugin
b.vst3]... [--server host[:port]] [--block N] [--realtime] [--budget D]
[--settle ms] [--no-compare]`. Reads each plugin's identity (uid, name,
vendor) from its local copy, opens a session (HELLO, CONFIGURE with the
file's channels and the chain's widest bus, one SLOT_ADD per plugin), waits
`--settle` ms (1000) for the server's plugins to settle, then streams the
file in blocks over `UdpTransport`:

- offline (default): each block waits for its reply, re-sent every 50 ms
  with the retry flag, 5 s at most;
- `--realtime`: blocks leave at the audio rate; replies later than `--budget`
  blocks (2) count as late, missing ones as lost.

It reports round-trip times, retries or late and lost blocks, the chain
latency and the server's processing time, writes the result, then processes
the same blocks locally through the same plugins (same empty play head,
same settling) and compares. A difference is written next to the output as
`<out>.local.wav`. `--telemetry` switches telemetry on for every
FxmeNet-aware slot, counts the frames received while streaming and decodes
the last one with the slot's schema; the `PARAM_CHANGED` batches that
arrived are counted too. `--discover` alone lists the servers answering a
broadcast.

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
