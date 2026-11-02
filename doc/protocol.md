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
| 1 | `HELLO` | client | JSON: protocol versions supported, client name, OS, FxmeNet version, `resume` (sessionId + token of a session to take back after a reconnect, optional) |
| 2 | `WELCOME` | server | JSON: protocol version chosen, sessionId, token, UDP port, server name, OS, capabilities (`["udp-audio"]`, later `"tcp-audio"`), or an error |
| 3 | `CONFIGURE` | client | JSON: sample rate, block size N, input and output channel counts, channel masks, telemetry rate (Hz, default 30), datagram budget |
| 4 | `PING` / 5 `PONG` | either | binary: u64 sender clock (microseconds), echoed |
| 6 | `ERROR` | either | JSON: code, text, requestId it answers |
| 7 | `BYE` | either | none |
| 10 | `LIST_PLUGINS` | client | none |
| 11 | `PLUGIN_LIST` | server | JSON: entries (section 4.3) |
| 20 | `SLOT_ADD` | client | binary: slotId, position, plugin uid (u32), name, vendor, state blob (u32 length + bytes, may be empty) |
| 21 | `SLOT_READY` | server | JSON: slotId, latency, channel layout, parameters (section 4.4), telemetry schema (base64 of the `TelemetryCodec` schema), FxmeNet ABI version or null |
| 22 | `SLOT_REMOVE` | client | binary: slotId |
| 23 | `SLOT_MOVE` | client | binary: slotId, new position |
| 24 | `SLOT_BYPASS` | client | binary: slotId, u8 on |
| 25 | `CHAIN_CLEAR` | client | none |
| 30 | `PARAM_SET` | client | binary: count, then (slotId u32, index u32, normalised value f32) |
| 31 | `PARAM_CHANGED` | server | same layout: changes the plugin made itself |
| 32 | `STATE_SET` | client | binary: slotId, state blob |
| 33 | `STATE_GET` / 34 `STATE` | client / server | as `STATE_SET`, with an empty blob / slotId + blob |
| 40 | `TELEMETRY_ACTIVE` | client | binary: slotId, u8 on (the slot's editor opened or closed) |

`SLOT_READY` answers `SLOT_ADD`, `PLUGIN_LIST` answers `LIST_PLUGINS`,
`STATE` answers `STATE_GET`, `PONG` answers `PING`, and `ERROR` answers
anything that failed. Every other request (`CONFIGURE`, `SLOT_REMOVE`,
`SLOT_MOVE`, `SLOT_BYPASS`, `CHAIN_CLEAR`, `STATE_SET`, `TELEMETRY_ACTIVE`)
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

A plugin's identity between machines is (`uid`, `name`, `vendor`). `uid` is
JUCE's `PluginDescription::uniqueId` for VST3: a 32-bit hash of the class id,
normalised so it is the same on every OS (amended 2026-11-02: JUCE's hosting
API does not expose the 16-byte class id itself). Never JUCE's identifier
string, which also hashes the file path. `version` and `sha256` decide tier 2
compatibility (section 2.4 of the plan). `fxmenetAbi` is null for a plugin
that does not answer `INetPlugin`.

### 4.4 Parameters (in `SLOT_READY`)

Per parameter: index, VST3 parameter id, name, label, default (normalised),
number of steps (0 = continuous), automatable, bypass, group path, and the
text of up to 32 sample values for stepped ones. Enough for the tier 0 generic
editor and the automation pool; values always travel normalised.

## 5. Audio

### 5.1 Blocks and latency

The session runs at a fixed block size N (`CONFIGURE`, default 128). The
client re-blocks the DAW's buffers into N-sample blocks through FIFOs, sends
block k as soon as it is full, and plays block k's reply D blocks later
(D = latency budget in blocks, default 2, user-selectable). It reports to
the DAW:

```
latency = D * N  +  (N if the DAW's block size is not a multiple of N, else 0)
        + chain latency (from the latest audio reply)
```

The server has no clock: it processes a block when its request arrives.

### 5.2 Audio request (kind 1, client to server)

Flags: bit 0 `offline` (no deadline: the client waits), bit 1 `retry` (a
re-send of the same block), bit 2 `transport` present.

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
  The client never waits on the audio thread: a reply missing when its block
  is due is replaced by silence with a short fade (64 samples), counted and
  shown; a reply arriving after that is discarded.
- **Offline (flag set, the DAW's `isNonRealtime()`):** the client's audio
  thread waits for the reply, re-sending the request with the retry flag every
  50 ms, giving up after 5 s (then silence and an error shown). The server
  holds block k+1 until block k has arrived (bounded by the same 5 s), and
  answers a retry of a block already processed from its cache: nothing is
  processed twice, nothing is lost.
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
- **mDNS / DNS-SD:** `_fxmenet._tcp`, TXT records with the same fields.
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
