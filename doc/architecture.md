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
cmake/Dependencies.cmake  pins: JUCE 9.0.4, FxmeTools 94e4e4d, jsfx (the JSFX engine,
                        github.com/odoare/jsfx branch fxmenet-branch; sibling
                        ~/src/JesuNet/jsfx; FXMENET_JSFX=OFF leaves it out)
Common/                 shared by server and client, header-only:
  FxmeNet/protocol/     Wire.h (datagram header, audio request/reply, telemetry
                        bodies), Fragmentation.h (Fragmenter, Reassembler); JUCE-free
  FxmeNet/transport/    AudioTransport.h (the interface, JUCE-free),
                        UdpTransport.h (juce::DatagramSocket, one session)
  FxmeNet/control/      ControlFrame.h (frames, types, binary payloads, FrameAssembler;
                        JUCE-free), ControlMessages.h (JSON messages, juce::JSON),
                        ControlConnection.h (one TCP connection, juce::StreamingSocket)
  FxmeNet/catalog/      PluginFiles.h: a bundle's platform folder, its binary, the
                        binary's sha256 (server catalog and client matching);
                        Buses.h: the buses a hosted plugin gets everywhere (its
                        default layout, main buses on; never enableAllBuses)
  FxmeNet/jsfx/         JSFX scripts (doc/jsfx-plan.md): JsfxBundle.h (a script
                        with its files and a JSFX slot's state on the wire;
                        JUCE-free), JsfxScripts.h (finding scripts on disk,
                        packing one with what it imports; juce_core),
                        JsfxSync.h (the @gfx display's mirror: messages and
                        the server's side; JUCE-free)
  FxmeNet/client/       the client: BlockStream (re-blocking, latency, jitter
                        buffer, concealment) and ReplyRing, JUCE-free; AudioLink
                        (one session's UDP audio) and ClientEngine (chain,
                        connection thread, audio entry point), JUCE
  tests/                FxmeNetProtocolTests and FxmeNetClientTests (no JUCE),
                        FxmeNetTransportTests (loopback UDP), FxmeNetControlTests
                        (JSON, loopback TCP)
Server/Source/          fxmenet-server: Main (options, signals, message loop),
                        Server (listener, handshakes, UDP routing, orphans),
                        Session (control handling), Chain (slots, snapshots),
                        AudioWorker (ordering, reply cache, processing),
                        PluginCatalog (scan, folders, rescans), JsfxInstance (a
                        JSFX script as a plugin of the chain, on the fxjsfx
                        library), Monitor (fxmenet-top's connections),
                        SystemLoad, UdpPeer, Log
Plugin/                 FxmeNetPlugin ("FxmeNet", VST3 and AU): the client in the DAW,
                        processor, 256-parameter pool, editor and its panels
Examples/GainPan/       FxmeNetGainPan: gain, balance, stereo in/out meters, the
                        full FX-Mechanics shell; the FxmeNet-aware howto plugin
Tools/Loopback/         FxmeNetLoopback: Phase 1 harness, source and mirror of one
                        plugin in one process, telemetry pumped in memory
Tools/WavClient/        fxmenet-wavclient: Phase 2 end-to-end test, a WAV through a
                        chain on the server, compared with local processing
Tools/Top/              fxmenet-top: the server in a terminal (sessions, load,
                        traffic, log) and its plugin folders
Spikes/                 Phase 0 throwaway programs (FXMENET_BUILD_SPIKES, ON for now)
  HeadlessHost/         spike 1: GUI-free VST3 hosting, WAV through a plugin,
                        state round trip
  NestedEditor/         spike 3: a VST3 hosting a VST3 and embedding its editor
                        (XEmbed on Linux), for Phase 4's shadow editors
doc/                    this file, todo.md, done.md; protocol.md (the wire
                        protocol, draft); jsfx-plan.md (JSFX scripts in the
                        chain: design, phases, state); audiogridder-notes.md
doc/manual/             the user manual: LaTeX source for a PDF and an HTML site
                        (make pdf, make html; README.md there)
```

The plugin-side classes (for plugins hosted on the server) live in FxmeTools, not here: `net/TelemetryCodec.h`,
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
is concealed (after `AudioLink`'s bounded wait, below): the last sample played is held and ramped to zero over 64
samples, then silence, and the next block fades in over 64 samples. Buffers
larger than the announced maximum are processed in pieces.

`ReplyRing` holds the replies between the receive thread and the audio
thread: block k in slot k mod 64, a tag per slot. The audio thread publishes
the next block it will read, and the receive thread refuses replies older
than that, too far ahead, or duplicated, so a slot being read is never
written; a seqlock check on the tag would still turn a broken assumption
into a missing block, never a torn one. Channels arrive by the reply's
output mask (bit c into channel c, missing ones silent).

`AudioLink` is one session's audio path: its own UDP socket, sends from the
audio thread (the enabled input channels encoded into a buffer reserved
once), and a receive thread putting replies into a `ReplyRing`. Its first
datagram, a keep-alive, is sent by the connection thread: JUCE resolves and
caches the server's address on a socket's first write, so the audio thread
never waits on a lookup, and the server learns where to send telemetry. The
last request of each block in flight is kept for offline retries: offline,
`fetch` waits for the reply, re-sending every 50 ms, up to 5 s. In real
time it waits only until a deadline the engine sets for each host callback,
a quarter of the callback's duration: a host rendering ahead in bursts
(Reaper's anticipative FX processing) needs blocks long before their
latency has elapsed in wall-clock time; the replies caught that way are
counted (`rescued`). Statistics
(sent, replies, refused, retries, round trip, chain latency) live in the
engine and survive links. Telemetry messages go to the engine's
`TelemetryMailbox` (up to 16 frames queued per slot, in order: tap channels
carry the samples since the previous frame), never to a mirror directly,
since the message thread may delete one at any time.

`ClientEngine` is FxmeNetPlugin minus its GUI and DAW glue. The chain is the
client's (`SlotModel`: identity, last known state, bypass, parameter values
set since that state, and what the server answered). A connection thread
connects, says HELLO (resuming the previous session if there is one), sends
CONFIGURE (always: it restarts the server's block numbering), opens a new
`AudioLink`, then rebuilds the chain on the server (CHAIN_CLEAR, SLOT_ADD
with each state, bypass, the parameter values set since) unless the session
was resumed and nothing changed; it then reads the control channel and
pings every second, and reconnects every 2 s after a failure. Chain edits
made while connected are sent at once and their answers update the model;
made while not connected (or when a send fails) they mark the chain dirty,
and the connection thread rebuilds it. `refreshStates` fetches every slot's
state (STATE_GET) so the model can be saved, dropping the parameter values
the new states already include.

On the audio thread, `process` runs the `BlockStream` with the current link,
taken through a hazard pointer: the connection thread swaps links
atomically and deletes the old one only once the audio thread has let go of
it. A new link restarts the stream at block 0. With no link (connecting,
reconfiguring) or when bypassed, blocks are played dry through the same
FIFO, so the output keeps the same latency.

`prepare` (the host's `prepareToPlay`, which Reaper calls at the start and
end of every render) always reconfigures: CONFIGURE re-prepares the server's
plugins as the host re-prepares local ones, so a render starts from the same
plugin state as a local render would. From the moment `prepare` returns, the
current link is unusable (a prepare generation counter, compared with the
generation the last CONFIGURE saw), so the audio thread never restarts its
stream on a session the server is still running; it plays dry meanwhile,
or, offline, waits for the new session (up to 5 s per episode, so a server
that is gone costs one wait and the render then goes on dry). `AudioLink`
also keeps its wire numbering increasing across stream restarts
(`rebase`), as a second guard.
## fxmenet-server (Server/)

`fxmenet-server [--name NAME] [--control-port 54322] [--udp-port 54321]
[--discovery-port 54320] [--no-discovery] [--plugin-path DIR]...
[--no-default-paths] [--rescan] [--remote-admin]`. `--plugin-path` and
`--no-default-paths` apply to that run only; the folders kept from one run
to the next are those set with fxmenet-top.

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

**Plugin folders.** The scan looks in the VST3 default folders (unless
turned off), the saved folders, and this run's `--plugin-path` folders.
The saved folders and the default-folders switch are set from fxmenet-top
and kept in `FX-Mechanics/fxmenet-server-settings.json`, next to the
cache. The startup scan runs on the message thread; a rescan (fxmenet-top)
runs on a thread of its own with its own VST3 format object, and the new
entries replace the old ones on the message thread when it ends, so
sessions go on meanwhile (`find`'s pointer is only used within one
message). A rescan asked during one follows it. Stopping the server
during a scan kills the plugin being examined and writes no cache.

**Monitors and status.** A HELLO with the role "monitor" (fxmenet-top,
protocol.md 4.7) gets a `Monitor` instead of a session: its reader thread
posts frames to the message thread, which answers `MONITOR_STATUS` from
`Server::describeStatus` and applies the folder changes when the monitor
is allowed to (from 127.0.0.1 or ::1, or any address with
`--remote-admin`). Every second the server's timer samples the machine's
and the process's CPU and memory (`SystemLoad`: /proc on Linux, Mach
calls on macOS, GetSystemTimes and GetProcessTimes on Windows, where
there is no per-core figure), the bytes received on the UDP socket, and
each session (`Session::sampleSecond`: blocks and bytes per second, the
worst processing time and reply delay of the second, folded into the 10 s
maxima of the activity line). The log keeps its last 1000 lines in memory
(`LogRing`) for monitors.

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
  the OS allows it): takes requests from a lock-free queue (half a second of
  blocks, 16 to 2048 entries within 32 MB, sized and preallocated at
  CONFIGURE while the session takes no datagram), orders them (protocol.md 5.4: a real-time gap
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

## fxmenet-top (Tools/Top)

`fxmenet-top [HOST[:PORT]] [--once]` (this machine's server by default),
`fxmenet-top --find` (lists the servers answering discovery). A terminal
program for Linux, macOS and Windows 10 and later (virtual terminal
sequences), on `juce_core` only; `Terminal` is oto-top's (otomatotamoto),
with the keys needed to type a path. A thread (`Link`) connects as a
monitor, asks for the status every second, reconnects when the server
goes, and sends the folder commands; the main thread reads keys and draws
four times a second.

Three views: 1 the sessions (one line each: client, address, format,
plugins, the worst processing time of the last second against the
block's duration, traffic, and the late, lost and stale counts, red for
10 s after they moved), with the selected session's details, chain and
the latest log lines below; 2 the plugin folders (default, saved, this
run, missing ones in red), the scan's progress, and the plugins found
(filter with `/`); 3 the log (scroll, filter). On the plugins view, `a`
adds a folder, `d` removes the selected saved one, `x` turns the default
folders on or off, `r` and `R` rescan (`R` ignoring the cache); read only
when the server does not grant "admin". `--once` prints one frame without
colours and exits.

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

`--engine` runs the same test through `ClientEngine`, as FxmeNetPlugin will:
the file is fed in host buffers of `--host-block` samples (512), or of random
sizes up to that with `--random-blocks`, offline or paced in real time
(`--realtime`); the output is realigned by the stream latency the engine
reports, then compared with local processing in N-sample blocks as above.

## FxmeNetPlugin (Plugin)

The DAW glue around `ClientEngine`; product name "FxmeNet", VST3 and AU
(CLAP once clap-juce-extensions is pinned).

- Buses: one input and one output, 1 to 64 channels each, stereo by
  default. The stream settings (block size N, budget D, input and output
  masks) are part of the state; changing them re-prepares the engine with
  the audio suspended (`suspendProcessing`), or not at all when unchanged.
- `processBlock` gives the engine the buffers, the play head (bpm, ppq, time,
  loop start, time signature, play / record / loop) and `isNonRealtime()`.
  `processBlockBypassed` asks for dry audio, delayed by the same latency. A
  layout the engine was not prepared for gives silence until the next
  `prepareToPlay`.
- A 50 Hz timer (message thread) reports the latency again when it changes,
  asks for the server's catalog once per connection, and runs the pool.
- The pool (`ParameterPool.h`): 256 `PoolParameter`s (`p001`..`p256`), each
  idle or mapped to (slot, parameter index). A mapped one takes its name
  ("FxmeCompressor: Ratio"), unit and step texts, and the host is told
  (`updateHostDisplay` with parameter info changed). A value set by the DAW,
  on any thread, only raises an atomic flag; the timer sends the changed
  mapped values in one `PARAM_SET`. `PARAM_CHANGED` values from the server
  are set on the pool parameters with the value remembered as sent, so they
  are not echoed back. When a slot first loads, its automatable parameters
  (bypass excepted) are mapped to the first free pool entries; the pool
  then shows the slot's current values (`SLOT_READY` carries them). A
  removed slot frees its entries. Automatic allocation (and Map) only takes
  entries never used in the project (`everUsed`, saved as `usedPool`): the
  DAW's automation lanes belong to pool entries, so a freed entry still
  carries the removed plugin's automation, and must not silently drive the
  next plugin's parameter. A freed entry is reused only by Learn, or once
  all 256 have been used. Mapping by hand: `mapPool` (one pool
  entry per slot parameter: mapping one drops the others), `mapToFreePool`,
  `unmapPool`; learn: `startLearn (target)`, then the first pool parameter
  whose value the DAW changes (any value other than the last one FxmeNet
  set itself) is mapped to the target. A slot parameter moved in the editor
  goes through its pool entry when it has one (with the drag as a change
  gesture), so the DAW records it; otherwise straight to the server.
- State, XML: server, stream settings, each slot (id, uid, name, vendor,
  bypass, state blob in base64, parameter values set since that state) and
  the mapping. `getStateInformation` first fetches the slots' states from the
  server (2 s at most; otherwise the last known ones are saved).
  `setStateInformation` hands the chain to the engine, which rebuilds it on
  the server, restores the mapping as saved (no automatic mapping for those
  slots), and connects.
- Local plugins (`LocalCatalog`, Phase 4 step 8a): one scan per process,
  shared by every instance (`juce::SharedResourcePointer`), on a background
  thread with JUCE's `PluginDirectoryScanner` and a dead man's pedal file
  (a plugin that crashes the DAW while scanned is skipped from then on).
  Cached with each binary's sha256 in `FX-Mechanics/fxmenet-client-plugins.xml`
  (the pedal file beside it), so only new or changed plugins are scanned
  and hashed again (changed: the binary's size or date, not the bundle
  folder's, which a copy over an installed bundle leaves alone). Searched:
  the VST3 default folders and the user's extra folders (FxmeNet.settings,
  "Folders..." in the Settings tab), with their subfolders; a
  plugin outside them leaves the list. Rescan and Full rescan in the same
  tab; the scan thread waits for requests, and one made during a scan
  follows it.
  `match` compares a server entry with the local plugins: same uid, name and
  vendor is the same plugin; the same version makes it compatible, tier 1,
  or tier 2 when the server says it is FxmeNet-aware; another version or no
  copy, tier 0 (generic controls). The sha256 is compared only when both
  sides run the same platform, and a difference is shown (another build of
  the same version), not refused. The editor's lists show T0 / T1 / T2,
  with the reason on hover. The server's plugins are listed T2 first, then
  T1, T0, and by name within each, filtered by the field above the list
  (every word typed must be in the name); while typing, the first match is
  selected and Return adds it; the selection follows its plugin when the
  list changes. FxmeNet and its spikes are never listed;
  instruments are left out (FxmeNet hosts effects). The plugin is built
  with `JUCE_PLUGINHOST_VST3`.
- Shadows (`Shadow.h`, Phase 4 step 8b): a slot's local copy, created the
  first time its editor is asked for (a slot that is loaded and tier 1 or
  2), kept until the slot goes or reloads. Told it is a mirror
  (`INetPlugin::setRole`) before `prepareToPlay` at the session's rate and
  N; never processed. It must have the same parameter count as on the
  server, or it is refused. It gets the slot's state from the server
  (`ClientEngine::fetchState`), then, from the processor's 50 Hz timer,
  every parameter's current value (`setQuietly`: shown in its editor,
  nothing reported; a parameter its editor moved in the last 300 ms is left
  alone). Its editor's own moves and gestures go through
  `setSlotParameter` and the gesture calls, like the generic view's (via
  the pool entry when mapped, so the DAW records them). For an
  FxmeNet-aware plugin, a change of `getStateChangeCount` (a preset or a
  file loaded in its editor) sends its whole state with STATE_SET
  (`ClientEngine::setSlotState`). Before deleting a shadow the processor
  calls `beforeShadowRemoved`, so the editor drops the view first.
- Telemetry (Phase 4 step 8c): showing a slot's own editor sends
  TELEMETRY_ACTIVE on (`setEditorShown`, kept in the model as
  `telemetryWanted` and sent again after a rebuild or a resume), closing it
  sends it off. The processor's timer takes the frames from the engine's
  mailbox and writes them into the shadow (`INetPlugin::writeTelemetry`),
  so an FxmeNet-aware plugin's meters and spectra show the server's audio.
- Editor: server field (typed, or picked from a discovery broadcast run on
  a background thread), Connect, status with the server's OS; on the left
  the server's plugins (double-click or Add) and the chain with Up, Down,
  Bypass, Remove; on the right three tabs (`Panels.h`):
  - Parameters: the slot's own editor (its shadow's), in a viewport, with a
    button to switch to generic controls, and a "last touched" button: the
    control last moved or grabbed in the plugin's editor (the processor
    records it from the shadow's callbacks), with a menu to map it to a new
    pool entry, unmap it, or learn it; the window grows to fit the plugin's
    editor and returns to its size afterwards. Generic controls
    otherwise (and the reason when there is no own editor), one row each: name and
    unit, a slider (normalised, stepped ones snap; double-click for the
    default), the value text (the step's name, else the normalised value:
    the plugin's own texts are Phase 4, with its own editor), the pool
    entry, Learn, Map / Unmap;
  - Mapping: every mapped pool entry with its name and value, Unmap (multiple
    selection), Unmap all, and the learn in progress with Cancel;
  - Routing: the routing between the chain's plugins (`RoutingView`, all
    the room it can get), Reset routing, and the two options of the
    default routing;
  - Settings: N and D and the latency they give (network and re-blocking,
    then the chain's), the transport options, and the local plugin scan
    (folders, rescans).
  One line of statistics at the bottom. Displays read the slots through
  `ClientEngine::getSlotInfos` (no state blobs) and
  `ClientEngine::currentValue`, which stays right across a state refresh.
  The last server is remembered machine-wide (`FX-Mechanics/FxmeNet.settings`).

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
