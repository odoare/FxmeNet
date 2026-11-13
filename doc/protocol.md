# FxmeNet protocol, version 1 (draft)

Status: draft for review, 2026-11-02. Nothing is implemented yet. Decisions
behind it: `../plan.md` (sections 2 to 4), `audiogridder-notes.md`.

## 1. Overview

A **client** (FxmeNetPlugin, in a DAW) runs a **session** on a **server**
(fxmenet-server). A session is one chain of plugin **slots**, processing one
audio stream. A DAW track with FxmeNetPlugin on it is one session.

| Channel | Transport | Direction | Carries |
| :--- | :--- | :--- | :--- |
| Discovery | UDP broadcast, mDNS | both | finding servers |
| Control | TCP | both | session, chain, parameters, state, catalog |
| Audio | UDP | request client to server, reply back | audio blocks, MIDI, transport, latency |
| Telemetry | UDP (same socket as audio) | server to client | what open editors draw |

Default ports: discovery 54320/udp, audio and telemetry 54321/udp, control
54322/tcp. All configurable on the server; the client learns them from
discovery or the user.

## 2. Conventions

- **Encoding:** little-endian, written field by field (no struct copies), as
  FxmeTools' `net/TelemetryCodec.h`. `f32`/`f64` are IEEE 754. Strings are
  UTF-8 with a `u16` byte length before them.
- **Self-delimiting messages:** every message carries its own length (or
  fills its datagram exactly), so the same messages can travel on a stream.
  No code may rely on "one datagram is one block": that is what keeps a TCP
  audio transport cheap to add (section 11).
- **Versioning:** every UDP message starts with a magic and a version byte;
  the control channel negotiates the protocol version once, in `HELLO`. A
  receiver ignores fields it does not know at the end of a JSON object or of
  a message whose length says there is more.
- **Ids:** `sessionId` (u32, server-chosen), `token` (u64, random,
  server-chosen, required in every UDP message of the session, so a stale or
  foreign packet is dropped), `slotId` (u32, client-chosen, unique in the
  session, stable across reconnects).

## 3. UDP framing

Every datagram:

```
u32 magic    'FXNA' (0x414E5846)
u8  version  1
u8  kind     1 audio request, 2 audio reply, 3 telemetry, 4 keep-alive
u16 flags    per kind, below
u32 sessionId
u64 token
u32 messageId    increases per message within (session, direction)
u16 fragment     index of this datagram in the message, from 0
u16 fragments    datagrams in the message (1 when not split)
... body bytes [fragment's share]
```

Header: 28 bytes. A message larger than the datagram budget (default 1400
bytes of body, keeping clear of IP fragmentation on a 1500-byte MTU; raised
on jumbo-frame networks by the session config) is split into fragments in
order. The receiver reassembles by `messageId`; a message missing a fragment
after the next message of the same kind is complete is dropped whole.

## 4. Session setup (control channel)

### 4.1 Control framing

TCP stream of frames:

```
u32 length      of what follows
u16 type
u16 flags       bit 0: payload is JSON (else binary)
u32 requestId   0 for unsolicited messages; a reply carries its request's id
... payload
```

Structural messages are JSON (low rate, easy to evolve and to log); plugin
state and parameter batches are binary.

### 4.2 Messages

| Type | Name | From | Payload |
| :--- | :--- | :--- | :--- |
| 1 | `HELLO` | client | JSON: protocol versions supported, client name, OS, FxmeNet version, `resume` (sessionId + token of a session to take back after a reconnect, optional), `role` (`"monitor"` for fxmenet-top, section 4.7; absent for a session) |
| 2 | `WELCOME` | server | JSON: protocol version chosen, sessionId, token, UDP port, server name, OS, capabilities (`["udp-audio"]`, `"routing"` when it applies `ROUTING`, later `"tcp-audio"`), or an error |
| 3 | `CONFIGURE` | client | JSON: sample rate, block size N, input and output channel counts, channel masks, telemetry rate (Hz, default 30), datagram budget |
| 4 | `PING` / 5 `PONG` | either | binary: u64 sender clock (microseconds), echoed |
| 6 | `ERROR` | either | JSON: code, text, requestId it answers |
| 7 | `BYE` | either | none |
| 10 | `LIST_PLUGINS` | client | none |
| 11 | `PLUGIN_LIST` | server | JSON: entries (section 4.3) |
| 20 | `SLOT_ADD` | client | binary: slotId, position, plugin uid (u32), name, vendor, state blob (u32 length + bytes, may be empty) |
| 21 | `SLOT_READY` | server | JSON: slotId, latency, channel layout, buses (section 4.5), parameters (section 4.4), telemetry schema (base64 of the `TelemetryCodec` schema), FxmeNet ABI version or null |
| 22 | `SLOT_REMOVE` | client | binary: slotId |
| 23 | `SLOT_MOVE` | client | binary: slotId, new position |
| 24 | `SLOT_BYPASS` | client | binary: slotId, u8 on |
| 25 | `CHAIN_CLEAR` | client | none |
| 26 | `ROUTING` | client | binary: count (u32), then (fromBox u32, fromChannel u16, toBox u32, toChannel u16); section 4.6 |
| 30 | `PARAM_SET` | client | binary: count, then (slotId u32, index u32, normalised value f32) |
| 31 | `PARAM_CHANGED` | server | same layout: changes the plugin made itself |
| 32 | `STATE_SET` | client | binary: slotId, state blob |
| 33 | `STATE_GET` / 34 `STATE` | client / server | as `STATE_SET`, with an empty blob / slotId + blob |
| 40 | `TELEMETRY_ACTIVE` | client | binary: slotId, u8 on (the slot's editor opened or closed) |
| 50 | `MONITOR_STATUS` | monitor / server | JSON: `logSince`, `catalogRevision` / the status (section 4.7) |
| 51 | `MONITOR_PATHS` | monitor | JSON: `paths` (the saved plugin folders), `useDefaultPaths` |
| 52 | `MONITOR_RESCAN` | monitor | JSON: `full` (ignore the cache) |

`SLOT_READY` answers `SLOT_ADD`, `PLUGIN_LIST` answers `LIST_PLUGINS`,
`STATE` answers `STATE_GET`, `PONG` answers `PING`, `MONITOR_STATUS`
answers `MONITOR_STATUS`, and `ERROR` answers
anything that failed. Every other request (`CONFIGURE`, `SLOT_REMOVE`,
`SLOT_MOVE`, `SLOT_BYPASS`, `CHAIN_CLEAR`, `ROUTING`, `STATE_SET`, `TELEMETRY_ACTIVE`,
`MONITOR_PATHS`, `MONITOR_RESCAN`)
is acknowledged on success by a frame of its own type with an empty payload
and the same `requestId`. `PARAM_SET` is never acknowledged (a stream of
changes); a failed one gets an `ERROR`. Chain
edits are applied by the server off the audio thread and swapped in between
two blocks; a slot's processing never stops for another slot's edit.

### 4.3 Plugin catalog entry

```json
{ "uid": "1668ba35", "name": "FxmeCompressor", "vendor": "FX-Mechanics",
  "version": "0.5.0", "category": "Fx|Dynamics", "sha256": "...",
  "inputs": 2, "outputs": 2, "fxmenetAbi": 1, "platforms": ["x86_64-linux"] }
```

`platforms` uses the folder names of a VST3 bundle (`x86_64-linux`,
`aarch64-linux`, `MacOS`, `x86_64-win`, `arm64-win`); `sha256` is the hash of
the binary in the server's own folder.

A plugin's identity between machines is (`uid`, `name`, `vendor`). `uid` is
JUCE's `PluginDescription::uniqueId` for VST3: a 32-bit hash of the class id,
normalised so it is the same on every OS (amended 2026-11-02: JUCE's hosting
API does not expose the 16-byte class id itself). Never JUCE's identifier
string, which also hashes the file path. `version` and `sha256` decide tier 2
compatibility (section 2.4 of the plan). `fxmenetAbi` is null for a plugin
that does not answer `INetPlugin`.

### 4.4 Parameters (in `SLOT_READY`)

Per parameter: index, VST3 parameter id, name, label, default and current
value (normalised; `value` added 2026-11-02, a reader missing it takes the
default), number of steps (0 = continuous), automatable, bypass, group path, and the
text of up to 32 sample values for stepped ones. Enough for the tier 0 generic
editor and the automation pool; values always travel normalised.

A parameter may also carry `rangeMin`, `rangeMax` and `rangeStep` (added
2026-11-12): its values in the plugin's own units, linear over the
normalised range. JSFX sliders have them; the generic editor then shows
real values.

### 4.4b JSFX scripts

A JSFX script (doc/jsfx-plan.md) is added with `SLOT_ADD` like a plugin:
uid `0x4A534658` ("JSFX"), vendor `"JSFX"`, the script's path under the
client's Effects folder as the name. It is not in the server's catalog: the
script comes in the state blob, which is (little-endian, `u32` lengths):

```
u32 magic 0x53464A46, u32 version 1
blob main            the script's path, one of the files below
u32 count            at most 256 files, 32 MiB together
count x { blob path, u8 data (1: under Data/, 0: under Effects/), blob bytes }
blob sliders         the engine's slider state (text), may be empty
blob serialized      what @serialize wrote, may be empty
```

Paths are relative, `/` separated, without empty, `.` or `..` parts,
backslash, colon or control characters; a blob that breaks a rule is
refused. The server writes the files to a temporary folder and loads the
script from there. `STATE` returns the same blob (with the current sliders),
so a saved project carries its scripts. `SLOT_READY` then has
`"kind": "jsfx"` and `"hasGfx"`. A script that does not compile is answered
with `ERROR` (plugin load failed) and the compiler's message with its line.

### 4.4c A JSFX script's @gfx display

While a JSFX slot's telemetry is on, the server keeps the client's copy of
the script's state, for the client to run `@gfx` there
(`Common/FxmeNet/jsfx/JsfxSync.h`, doc/jsfx-plan.md):

| Type | Name | From | Payload |
|---:|---|---|---|
| 41 | `JSFX_SUBSCRIBE` | client | binary: u32 slotId, u32 count, count x u32 memory block (65536 items each). No answer |
| 42 | `JSFX_SYNC` | server | binary: u32 slotId, u32 frame, u8 flags (1: reset), [reset: u32 count, count x (u16 length, name)], u32 count, count x (u32 variable index, f64), u32 count, count x (u32 first item, u32 n, n x f64) |
| 43 | `JSFX_WRITE` | client | binary: as `JSFX_SYNC`, never a reset: what the display's `@gfx` changed while the user acted on it. No answer |

`TELEMETRY_ACTIVE` on (also when already on) makes the next `JSFX_SYNC` a
reset: every variable, by name, then by index; nothing subscribed, the
client's memory taken as all zero. After it, a frame carries what changed
since the one before: variables compared by their bits, memory in chunks
of 256 items, ranges never across a block. At most 64 kB of memory per
slot and per frame, 256 kB per session: the rest follows in later frames.
Unsolicited, over the control connection (the frames are deltas: they must
all arrive, in order).

`JSFX_WRITE` goes the other way: the server writes its variables and memory
into the script and counts them as already sent. Sliders do not travel this
way but as parameters (`PARAM_SET`), so that the script's `@slider` runs.

### 4.5 Buses (in `SLOT_READY`)

Added 2026-11-06, for routing. `buses`: the plugin's buses, inputs then
outputs, each in the plugin's order:

```json
{ "name": "Sidechain", "input": true, "channels": 2, "main": false }
```

`channels` is the bus's full width, enabled or not (a disabled bus counts
its default layout), so a routing's channel numbers do not move when a bus
is enabled. `main` is the first bus each way. `numInputs` and `numOutputs`
still give the channels enabled now. A reader without `buses` (an older
server) takes one main bus each way, of `numInputs` and `numOutputs`
channels.

### 4.6 Routing

Added 2026-11-06; design in [`routing.md`](routing.md). `ROUTING` carries
the whole graph between the chain's plugins, every connection from an
output pin to an input pin: the client resolves its rules (default routing,
manual pins) and the server only applies the result. Boxes are slot ids,
plus 0 for the session's inputs (FxmeNet's input channels, seen as
outputs) and 0xffffffff for its outputs (seen as inputs). A box's channels
are numbered across its buses in the order of section 4.5, each bus at its
full width.

- Several connections into one input pin are summed; one output pin may
  feed several inputs.
- The server keeps the last `ROUTING` whole and applies it to the chain as
  it is: a connection to a slot not loaded (yet), to a pin that does not
  exist, or not going downward in the chain is ignored until it makes
  sense. So a `ROUTING` sent right after `SLOT_ADD` or `SLOT_MOVE` is never
  refused for arriving first.
- Latency compensation: each plugin's inputs are aligned on the latest of
  them, each connection delayed as needed, and the session's latency is
  that of the path reaching the outputs last.
- A bus other than the main ones is enabled only while one of its channels
  is connected.
- Channel masks: the client leaves the inputs the routing does not use out
  of its audio requests (each request carries its mask, 5.2), and a routed
  server leaves out of its replies the outputs nothing feeds (5.3); the
  missing channels are silence. Neither needs a new `CONFIGURE`.
- Until a session receives `ROUTING` its chain is serial, as before
  (each plugin in place on the first channels of one shared buffer).
- The client sends `ROUTING` only to a server whose `WELCOME` lists the
  `"routing"` capability, after each change of the graph and again when a
  session is rebuilt; a resumed session keeps it.

### 4.7 Monitor connections

fxmenet-top (`Tools/Top`) connects to the control port like a client, with
`"role": "monitor"` in its `HELLO`. The server opens no session for it:
its `WELCOME` has sessionId 0, the capability `"monitor"`, and `"admin"`
when the monitor may change the plugin folders (it connected from the
server's machine, or the server runs with `--remote-admin`). A monitor
sends only `MONITOR_STATUS`, `MONITOR_PATHS`, `MONITOR_RESCAN`, `PING`
and `BYE`; anything else gets an `ERROR` (`unsupported`, 3), and the two
changes without `"admin"` get `notAllowed` (8).

`MONITOR_STATUS` asks for the log lines numbered `logSince` and after (0:
the latest 500), and the plugin list unless the catalog's revision is
still `catalogRevision`. The answer, refreshed by the server once a second:

- `server`: name, version, os, host, ports, discovery on, seconds up,
  monitors connected;
- `load`: `process` (the server's CPU, 100 per core used), `total` (the
  machine, 0 to 100, -1 if unknown), `cores` (each core; empty on
  Windows), `numCores`, `resident` (bytes);
- `network`: `inBytesPerSecond` (every datagram received),
  `outBytesPerSecond` (the sessions' replies and telemetry);
- `sessions`: for each, id, peer, client, os, orphan, seconds, configured,
  then sampleRate, blockSize, inputs, outputs, realtime (its audio
  thread), chainLatency, `slots` (id, name, vendor, version, latency,
  bypassed, fxmenet), `counters` since it started (processed, lost, stale,
  fromCache, queueFull, malformed, sendErrors, resets, overOneBlock,
  overTwoBlocks), processMicros (the last block), processMaxMicros and
  turnaroundMaxMicros (the last second), processPeakMicros (since it
  started), blocksPerSecond, inBytesPerSecond, outBytesPerSecond;
- `catalog`: revision, count, scanning, done and total (files), file
  (being examined), summary (the last scan), useDefaultPaths, and
  defaultPaths, savedPaths, commandLinePaths (each a list of path and
  exists), then `plugins` (name, vendor, version, category, inputs,
  outputs, fxmenet, file) when the revision changed;
- `log`: `lines`, and `next`, the number to ask from next time.

`MONITOR_PATHS` replaces the saved folders (absolute paths; the server
keeps them in `fxmenet-server-settings.json` next to its plugin cache) and
turns the VST3 default folders on or off; `MONITOR_RESCAN` scans again.
Both are acknowledged once the scan has started; it runs on a thread of
its own and the status shows its progress. Sessions keep the plugin list
they asked for: a client sees a rescan's result the next time it
connects.

## 5. Audio

### 5.1 Blocks and latency

The session runs at a fixed block size N (`CONFIGURE`, default 128). The
client re-blocks the DAW's buffers into N-sample blocks, sends block k as
soon as it is full, and plays its reply back after a fixed latency, which
gives every reply at least D blocks of time (D = latency budget in blocks,
default 2, user-selectable). With B the DAW's largest block, it reports:

```
latency = D * N + B - N        when the DAW's blocks are multiples of N
          D * N + B + N - 1    otherwise
        + chain latency (from the latest audio reply)
```

(amended 2026-11-02: the first draft's `D * N (+ N)` left a reply no time at
all when B > N, and less than D blocks when B is not a multiple of N.) A DAW
whose blocks stop being multiples of N midway switches the client to the
second formula once, with a latency change.

The server has no clock: it processes a block when its request arrives.

### 5.2 Audio request (kind 1, client to server)

Flags: bit 0 `offline` (no deadline: the client waits), bit 1 `retry` (a
re-send of the same block), bit 2 `transport` present, bit 3 `resetParity`
(added 2026-11-04, see 5.4).

```
u64 block        block index k since the session started (0, 1, 2, ...)
u16 samples      N
u64 inputMask    which chain inputs are sent (bit c = input channel c)
u16 midiBytes
[transport, 48 bytes, when flag 2]
    f64 bpm  f64 ppqPosition  f64 ppqLastBarStart  i64 timeInSamples
    f64 loopStartPpq  u16 timeSigNumerator  u16 timeSigDenominator
    u8  bits (playing 0, recording 1, looping 2)  u8 pad[3]
[MIDI, midiBytes]: events of (u16 sampleOffset, u8 size, bytes)
[audio]: f32 x N for each channel set in inputMask, lowest channel first
```

A disabled input (mask bit clear) is fed to the chain as silence.

### 5.3 Audio reply (kind 2, server to client)

```
u64 block        echoed
u16 samples      N
u64 outputMask   which chain outputs are sent
u32 chainLatency samples, now (the client applies a change on its message thread)
u32 processMicros   server time spent in the chain, for the statistics
u16 midiBytes
[MIDI]  [audio]  as in the request
```

### 5.4 Ordering, loss, offline

- The server processes each block at most once, in increasing order, and
  keeps its last 32 replies.
- **Real time (offline flag clear):** a request older than the last block
  processed is answered from the cache, or dropped if not cached. A gap (block
  k+1 arrives, k never did) is not waited for: k+1 is processed, k is lost.
  A reply missing when its block is due is waited for, but only until a
  deadline set per host callback (a quarter of the callback's duration):
  hosts that render ahead (Reaper's anticipative FX processing) call the
  plugin in bursts faster than real time, so a block can be needed before
  its latency has elapsed in wall-clock time (amended 2026-11-03; the first
  draft never waited). A host calling faster than real time is ahead of its
  audio output by the buffer it is filling: the client measures that
  margin (audio delivered since an anchor minus wall-clock time, the anchor
  taken again at each reset, play / stop change, play-head jump, or when
  the host falls behind) and the deadline grows by half of it, at most
  250 ms. That covers Reaper refilling its whole render-ahead buffer after
  a start, stop or seek (amended 2026-11-04: 10 to 13 blocks of 1024 were
  concealed at each). After the deadline the block is replaced by silence
  with short ramps (64 samples), counted and shown; a reply arriving after
  that is discarded.
- **Offline (flag set, the DAW's `isNonRealtime()`):** the client's audio
  thread waits for the reply, re-sending the request with the retry flag every
  50 ms, giving up after 5 s (then silence and an error shown). The server
  holds block k+1 until block k has arrived (bounded by the same 5 s), and
  answers a retry of a block already processed from its cache: nothing is
  processed twice, nothing is lost.
- **Reset:** when the host resets the client's plugin (JUCE's `reset()`:
  transport start, stop or seek, a flush), the client's reset count goes up
  and every later request carries its parity in bit 3. The server resets
  every plugin of the chain before processing the first block whose parity
  differs from the previous block's, so the chain is reset at the same point
  of the stream as the client (to the block: with host blocks that are not
  multiples of N, that block may start a few samples before the reset). A
  parity rather than a one-off flag: a lost request moves the reset one
  block later instead of losing it. The first block of a stream only sets
  the parity (the chain is fresh). Two resets within one lost block cancel
  out; the price is accepted. The client also drops what is in flight:
  its stream restarts at block 0 with the FIFO full of silence and the
  latency unchanged (the wire numbering goes on, older replies are
  refused), so it outputs silence for its latency after a reset, as a
  local plugin clearing its delay line does (amended 2026-11-04: without
  it, the replies in flight played the audio from before a seek).
- **Bypass on the client** (the DAW's bypass, or no server): the dry signal is
  delayed by the reported latency, so the timing never jumps.
- **Disconnected:** as bypass, plus a visible state; the session is rebuilt
  on reconnection (section 8).

## 6. Telemetry (kind 3, server to client)

```
u32 slotId
u32 frame        increases per slot
[TelemetryCodec frame, as INetPlugin::readTelemetry produced it]
```

Sent at the session's telemetry rate for each slot whose editor is open
(`TELEMETRY_ACTIVE`), nothing for the others. Loss is tolerated: meters and
values are latest-wins, a lost tap chunk shows as a seam. Typical sizes: 104
bytes for GainPan, about 6.4 kB per spectrum tap at 30 Hz and 48 kHz (five
fragments).

## 7. Discovery

- **Broadcast:** the client sends `{"cmd":"FXMENET_DISCOVER","version":1}` to
  255.255.255.255:54320; each server answers by unicast with
  `{"cmd":"FXMENET_ANNOUNCE","name","os","version","controlPort","udpPort","sessions","cpuLoad"}`.
- **mDNS / DNS-SD:** not in version 1 (decided 2026-11-02). mDNS is
  link-local like the broadcast, so it finds no server the broadcast misses,
  and it needs Avahi or Bonjour on every platform. Possible later as
  `_fxmenet._tcp` with the same fields in TXT records, for system browsers.
- **Manual:** host or IP and port typed in the client, remembered.

## 8. Reconnection and state

The client's state is authoritative: it holds the chain and each slot's
state blob, saved with the DAW session. When the control connection drops:

1. The client goes to bypass (dry, delayed) and retries every 2 s.
2. The server keeps the session for a grace period (10 s) after the drop.
3. On reconnection the client sends `HELLO` with `resume`. If the server
   still holds the session, it answers with the same ids and the client just
   resynchronises states it changed meanwhile; otherwise the client rebuilds:
   `CONFIGURE`, `CHAIN_CLEAR`, one `SLOT_ADD` per slot with its state blob.
4. Audio resumes at block 0 of the new session.

State changes made on the client side (an editor loading an IR in a mirror,
a preset) reach the server with `STATE_SET`, triggered by the mirror's
`INetPlugin::getStateChangeCount()` changing. Parameter changes go as
`PARAM_SET`.

## 9. Security (version 1)

LAN only, no encryption. The token in every UDP message keeps stray packets
out of a session. Anything that installs code (plugin distribution, Phase 6)
needs pairing and signed packages, and is off by default.

## 10. Limits in version 1

- 64 channels per direction (the masks are u64).
- Parameter changes travel on the control channel and apply at the next block
  the server processes: not sample-accurate. Sample-accurate automation
  (changes inside the audio request) is a possible version 2.
- One stream per session; sidechain inputs are ordinary extra input channels.

## 11. A TCP audio transport, if UDP proves wrong

The audio and telemetry messages are defined independently of the datagram
header: a TCP variant sends the same bodies as frames of
`u32 length, u8 kind, u8 version, u16 flags` on a second TCP connection, with
`TCP_NODELAY`, no fragmentation, no `token` (the connection is the session),
and no retries (TCP delivers). Everything else (blocks, latency, masks,
transport, the real-time and offline rules on the client) is unchanged.

Code structure required from Phase 2 on, so that this stays a small change:

- one `AudioTransport` interface (`send (message)`, `receive (buffer, timeout)`)
  with a UDP implementation; fragmentation, the retry timer and the reply
  cache live in the UDP implementation only;
- the session, jitter buffer and audio code see whole messages, never
  datagrams;
- a server capability (`"tcp-audio"`) and a `CONFIGURE` field select it.

Estimated cost of adding it then: about a day plus testing.
