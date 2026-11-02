# FxmeNet: Review of the Design Idea & Implementation Plan

FxmeNet offloads the DSP of FX-Mechanics JUCE plugins to a headless server on the
LAN, while the editor (with its real look and feel, meters and spectra) stays in
the DAW. This document challenges the idea in `FxmeNet-design.md`, proposes a
refined architecture, and gives a phased plan. It reuses `../JesuNet/plan.md`
for transport, discovery and capacity figures, and replaces its `@gfx` channel
with a telemetry channel.

Findings it relies on (checked against the code on 2026-11-02):

- `~/src/JUCE` is JUCE 9.0.4. It ships `juce_audio_processors_headless`
  (VST3/LV2/AU/LADSPA hosting depending only on `juce_audio_basics` and
  `juce_events`), so the server needs no GUI module. JUCE does not host CLAP.
- `juce::AudioProcessor::getVST3ClientExtensions()` lets a plugin answer
  custom interface IDs in VST3 `queryInterface`, and the hosting side reaches
  `IComponent*` through `ExtensionsVisitor::VST3Client`. That is the side
  channel used below.
- FxmeFX editors read DSP objects directly: `CompressorComponent` takes a
  `Compressor&` and reads `compressor.getGrMeter().getPeak()` in its timer;
  `EqualizerComponent` snapshots two `fxme::SpectrumTap`s and runs
  `SpectrumAnalyzer` on the GUI side. Transient and Limiter expose `VuMeter&`
  the same way. So "control values" are not only scalars: the EQ needs an
  audio-rate tap stream.
- JUCE on Linux loads X11 through `DynamicLibrary` at run time, so a JUCE
  plugin binary should load on a machine with no X server. This has to be
  confirmed by a spike (Phase 0) before anything else is built on it.

---

## 0. Why FxmeNet, given that AudioGridder exists

AudioGridder (MIT, github.com/apohl79/audiogridder) already offloads any
VST2/VST3/AU plugin to a server, with automation, latency compensation,
sandboxing, and editors streamed as images. Its server targets macOS and
Windows; Linux is marked experimental. For third-party plugins it is the right
tool, and FxmeNet should not compete there. Tier 0 below exists so that a chain
can mix in a foreign plugin, not to be a better AudioGridder.

What FxmeNet does that AudioGridder cannot, by construction:

1. The editor is native on the client, with live meters. AudioGridder must run
   the editor where the DSP is and send pictures, because it cannot know what an
   arbitrary editor reads. FxmeNet owns the plugins, so it sends the few values
   the editor draws (a few kB/s) and the real editor runs locally. Knob drags,
   text entry and resizing have no network round trip, HiDPI and fonts are the
   client OS's, and a closed editor costs nothing.
2. A truly headless server. Streaming editor images needs a desktop session on
   the server; FxmeNet's server is a console process with no display, so the
   target is a cheap fanless Linux box in a rack or under the desk, which is
   exactly the case AudioGridder supports least.
3. Server OS independent of client OS. A Linux server fits a macOS or Windows
   DAW, since the client never needs the server's windowing system.
4. It is a feature of the FX-Mechanics plugins, not a separate product. The
   telemetry classes go into FxmeTools, so every FxmeFX plugin (and every later
   one) becomes network-ready for a few lines, and stays an ordinary plugin
   in any DAW.
5. JSFX. The long-term goal is one chain that mixes FX-Mechanics plugins and
   JSFX scripts on the same server (the JesuNet idea). AudioGridder has no
   path to that.
6. Signed plugin distribution between client and server, which a generic host
   cannot do safely for third-party binaries.

The honest cost: AudioGridder carries years of fixes to the hard real-time
parts (jitter, PDC, offline bounce, plugin crashes, DAW quirks). FxmeNet will
have to rediscover them. Two mitigations are built into the plan: AudioGridder's
code is MIT, so the proven parts can be studied and reused (Phase 0), and the
differentiating claim (point 1) is proven first and without any network, in the
Phase 1 loopback harness. If the mirror editor cannot be made convincing there,
FxmeNet is not worth building and AudioGridder plus a generic editor is the
answer.

---

## 1. Can an unmodified VST3/CLAP/AU plugin be used?

Partly. Three separate things travel between a plugin and its user, and they
behave very differently.

| Concern | Unmodified plugin | Why |
| :--- | :--- | :--- |
| Audio + MIDI | Works | The server hosts it like any DAW would. |
| Parameters, automation, state | Works | Standard format API (VST3 parameters, `getStateInformation`). |
| The plugin's own editor, with live meters | Does not work | The editor reads processor memory in-process. There is no standard API for "the data my editor draws". |

That gives a tiered compatibility model, which I recommend adopting explicitly
rather than treating anything below tier 2 as failure:

- **Tier 0 (any VST3 on the server).** Audio, parameters, state. The client shows
  a generic parameter editor built from the parameter list the server sends.
  No client-side binary needed. This is the honest answer for third-party
  plugins.
- **Tier 1 (unmodified, also installed on the client).** The client loads a
  "shadow" instance of the same plugin only to show its real editor and keeps
  its parameters in sync with the server. Knobs and curves work; meters and
  spectra stay flat because the shadow never sees audio.
- **Tier 2 (FxmeNet-aware).** As tier 1, plus the plugin registers its meters
  and taps so the server can stream them and the shadow's editor animates
  exactly as if it ran locally. This is what the FxmeTools classes provide.

The alternative for unmodified plugins is what AudioGridder (an open-source
JUCE network plugin host, worth studying before writing a line) does: render
the editor on the server and stream pixels. I would not do it here. It needs a
display server on the "headless" Linux machine (Xvfb at best), it brings back
exactly the bandwidth and input-forwarding machinery the JesuNet `@gfx` section
was about, and the result is visibly laggy and wrong on HiDPI. Keep it as a
possible late add-on, not the base.

Format notes, also part of the answer:

- A Linux server can only run Linux binaries. "Unmodified AU" is meaningless on
  a Linux server, and a Windows-only VST3 cannot be offloaded there at all.
- JUCE cannot host CLAP. Use VST3 for every hosting role (server and the
  client's shadow), on all three OSes. FxmeNetPlugin itself can still be
  *built* as VST3/CLAP/AU for the DAW (clap-juce-extensions, as planned in the
  build migration). Hosting CLAP is a later, separate job with the CLAP SDK
  directly.

## 2. Challenging the FxmeNet-aware design

The proposal is "a function to get control data on the server, a function to
set it in the editor on the client". The direction is right, but written that
way every plugin needs bespoke get/set code and an editor that knows whether it
is local or remote. The existing code suggests something cheaper.

### 2.1 Same binary, two roles: the shadow instance

Do not split processor and editor into different binaries or classes. The same
plugin binary runs in three roles:

- **Local** (ordinary DAW use): unchanged behaviour.
- **Source** (on the server): processes audio. Its telemetry objects are read
  by the server at ~30 to 60 Hz.
- **Mirror** (on the client, inside FxmeNetPlugin): prepared but never
  processed. Its telemetry objects (`VuMeter`, `SpectrumTap`, ...) are written
  by the network instead of by `processBlock`.

The editor keeps reading `compressor.getGrMeter()` as today, and in mirror mode
that meter simply holds the server's values. **The editor needs no change at
all.** The processor needs a handful of lines to register what the editor
reads. This is the "server between processor and editor" idea, implemented by
interposing on the objects the editor already reads rather than on the
processor/editor boundary itself.

This works because the threading contract does not change. On the server, the
network thread reads meters and taps exactly the way an editor timer does
today. On the client, the network thread writes them where the audio thread
used to: `SpectrumTap` stays single-writer/single-reader, `VuMeter` stays
atomics.

### 2.2 Telemetry is not only control-rate

| Kind | FxmeFX example | Rate | Bandwidth (editor open) |
| :--- | :--- | :--- | :--- |
| Scalar meter | Compressor/Limiter GR, Transient gain | 30 to 60 Hz, latest wins | negligible |
| Stream tap | EQ input/output `SpectrumTap`, `WaveformTap` | audio rate, mono | ~1.5 Mbps per tap as float, ~0.8 as int16 |
| Small array | per-band gains, scope envelope | 30 Hz | a few kB/s |

Streaming the tap samples (rather than computing the FFT on the server and
sending bins) keeps `SpectrumAnalyzer` and the editor unchanged and is cheap on
a gigabit LAN. Server-side analysis can come later if Wi-Fi matters.

Following the JesuNet "closed editor rule", the client tells the server which
slots have an open editor, and the server only reads and sends telemetry for
those (taps also get `setEnabled (false)`, which `SpectrumTap` already has).

### 2.3 Crossing the binary boundary

FxmeNetPlugin and every hosted plugin each link their own static copy of JUCE.
No `juce::` object may cross between them (two `MessageManager`s, two
look-and-feel singletons). Hence:

- Editors are embedded with the format's native view (`IPlugView` through
  JUCE's `AudioPluginInstance::createEditor()`), which JUCE already handles.
- Telemetry and the "role" switch travel through a custom VST3 interface
  answered via `VST3ClientExtensions::queryIEditController`. It exposes a
  small versioned C ABI (plain structs and byte buffers, no JUCE or STL
  types): `getRole/setRole`, `describeTelemetry`, `readTelemetry (buffer)`,
  `writeTelemetry (buffer)`, `onStateChanged` callback. A plugin that does not
  answer it is tier 0/1.

### 2.4 Things the design does not mention yet (and the JesuNet plan did not either)

1. **DAW automation of a dynamic chain.** A VST3/AU cannot reliably add
   parameters after instantiation. FxmeNetPlugin needs a fixed pool of generic
   parameters (say 256) mapped onto chain slots, with the mapping saved in its
   state and display names updated via `updateHostDisplay` (honoured by Reaper,
   less so by others). Users will see "P017" in some hosts. Unavoidable,
   so design it in from Phase 3.
2. **State authority.** Recommend: the client is authoritative. The DAW saves
   FxmeNetPlugin's state, which contains the chain and each shadow's
   `getStateInformation()`; on load or reconnect the client pushes everything
   to the server. The server is a replica that can be restarted at any time.
   Parameter edits stream as `SET_PARAM`; non-parameter state changes (Cab or
   ConvolReverb loading an IR via `EmbeddedAudio`, a preset load) trigger a
   full `SET_STATE` for that slot, signalled by the plugin calling
   `notifyStateChanged()`.
3. **Offline rendering.** When the DAW bounces faster than real time
   (`isNonRealtime()`), the network path must switch to blocking
   request/response with a generous timeout, or the bounce fills with
   concealment silence.
4. **Latency.** The server has no audio device and no clock: it processes a
   block when one arrives. Reported latency = network delay (D blocks, default
   2, at a fixed internal block size) + sum of the chain's own latencies.
   Every chain edit can change it, which makes some DAWs re-sync PDC with a
   glitch. Live monitoring through FxmeNet will feel the extra 3 to 6 ms.
5. **Crash isolation.** One crashing plugin takes down every session on a
   single-process server. Fine for the PoC; later, one worker process per
   client session (also simplifies real-time thread ownership).
6. **Version skew.** Tier 2 requires the shadow and the server instance to
   be the same build (state format, telemetry IDs). Match on VST3 class UID +
   version + binary hash; fall back to tier 1/0 with a visible warning.
7. **Heavy shadows.** A mirror ConvolReverb would load its IR on the client
   for nothing. `isMirror()` lets a processor skip heavy preparation; editors
   that call `getSampleRate()` on the DSP still need `prepareToPlay` with the
   server's rate.

### 2.5 Plugin distribution: useful, but cross-OS and a security hole

- A `.so` from a Linux server is useless to a Windows client. Distribution
  only works if packages carry every platform. The VST3 bundle layout already
  does (`Contents/x86_64-linux`, `Contents/x86_64-win`, `Contents/MacOS`), so
  FxmeFX CI could publish one merged multi-OS `.vst3` per plugin, and that is
  the unit FxmeNet moves around.
- Server to client download installs native code on the user's machine;
  client to server upload is remote code execution on the server. Both must be
  off by default, limited to packages signed with an FX-Mechanics key
  (ed25519), and the server side additionally behind an explicit pairing
  token. Downloaded plugins go into FxmeNet's own folder, never into the
  system VST3 path, so the DAW does not pick them up unasked.
- Licensing: only redistribute plugins that declare themselves redistributable
  (FX-Mechanics ones). Never move third-party binaries.
- JSFX scripts are the easy case (text, interpreted on the server), as the
  design says.

Recommendation: keep distribution out of the PoC. It is Phase 6.

---

## 3. Architecture

```
 DAW machine (any OS)                                Server (headless Linux, or any OS)
+------------------------------------------+        +------------------------------------------+
| FxmeNetPlugin (VST3/CLAP/AU in the DAW)  |        | fxmenet-server (console app, no GUI)     |
|  processBlock -> SPSC -> net thread  ----|- UDP ->|  AudioWorker -> ChainEngine               |
|  <- jitter buffer <- net thread      <---|- UDP --|    slot0: Compressor.vst3 (role=source)   |
|                                          |        |    slot1: Equalizer.vst3  (role=source)   |
|  Chain view / connection header          |        |  TelemetryPump (30-60 Hz, open slots only)|
|  slot editors = shadow instances         |<- UDP -|    reads meters/taps via IFxmeNet ABI     |
|    (Compressor.vst3, role=mirror)        |        |                                          |
|    telemetry written in via IFxmeNet ABI |        |  ControlServer (TCP): chain, params, state|
|  ControlClient (TCP)  <------------------|- TCP ->|  PluginCatalog: scan, UIDs, hashes       |
|  Discovery (UDP broadcast / mDNS)        |<------>|  Discovery responder                      |
+------------------------------------------+        +------------------------------------------+
```

Reused from JesuNet: the UDP audio/MIDI channel and its header (new magic
`'FXNT'`), the TCP control channel and its message set (`GET_SCRIPT_LIST`
becomes `GET_PLUGIN_LIST`), discovery (broadcast + mDNS + manual IP), jitter
buffer, packet-loss concealment, SPSC rings, the bandwidth and CPU figures.

Replaced: channel 3 (`@gfx` frames + input forwarding) becomes a telemetry
channel (UDP, server to client, latest-wins frames per open slot, plus tap
chunks with a sequence number so the client can detect gaps and simply skip).

Added: plugin catalog on both sides (`PLUGIN_LIST` entries carry name, vendor,
VST3 UID, version, sha256, FxmeNet-aware flag and telemetry schema, and which
platforms the server holds binaries for), the slot-editor open/close messages,
and the parameter pool.

Also added, found while sizing the Dede and AmbiRR2 ports (section 4.1):

- Transport per audio block (bpm, ppq position, time signature, playing,
  sample position), so a hosted plugin gets a working `AudioPlayHead`. Dede
  syncs delays to tempo; without this it silently falls back to 120 bpm.
- MIDI in the PoC, not later: Dede reads note pitch from MIDI.
- `LATENCY_CHANGED` from server to client: Dede's latency follows its FFT
  size, so the client's reported latency must follow at run time.
- Any channel count per slot, and a chain-level bus layout: AmbiRR2 is 16 in /
  16 out (about 50 Mbps for one instance at 48 kHz, fine on gigabit).
- Disabling unused inputs and outputs. The client lets the user switch off
  any input or output channel of the chain (stored in the client state). A
  disabled input is not sent: the server feeds silence into that channel of
  the hosted plugin. A disabled output is not sent back: the client writes
  silence into it. The audio header carries the two channel masks, so
  bandwidth scales with what is actually used: AmbiRR2 with 3 mono sources and
  a first-order output (4 channels) streams 7 channels instead of 32 (about
  11 Mbps instead of 50). Masks act at the network boundary only; inside the
  chain on the server, every slot still sees its full channel count.

Server stack: JUCE 9 `juce_core`, `juce_events`,
`juce_audio_processors_headless` (+ FxmeCore). JUCE's sockets
(`DatagramSocket`, `StreamingSocket`) are enough for the PoC; it builds on all
three OSes and the server role does not care which OS the client runs.

---

## 4. The developer API (FxmeTools)

Minimal and split according to the house rule.

`core/FxmeTools/net/` (no JUCE, testable in seconds):

- `TelemetryRegistry` : a list of named channels, each bound to an existing
  object. `addMeter (id, VuMeter&)`, `addTap (id, SpectrumTap&)`,
  `addTap (id, WaveformTap&)`, `addValue (id, std::atomic<float>&)`,
  `addArray (id, ArrayView<std::atomic<float>>)`. `serialise (Writer&)` (source
  side) and `apply (Reader&)` (mirror side). Unknown IDs are skipped, so a
  version skew degrades instead of corrupting.
- Two more channel kinds, because real editors do not only read objects:
  - Computed values: `addScalar (id, getter)` on the source side, and
    `scalar (id)` returning the received value on the mirror side. A getter
    that computes (Dede's `getGateOpenness` picks the filter bank or the
    splitter; AmbiRR2's `getProgress` averages 16 workers) gets one line,
    `if (net.isMirror()) return net.telemetry().scalar (id);`.
  - Read-and-reset peaks: `addPeak (id, takeFn)`. The server calls the `take`
    function at the telemetry rate; the mirror holds the maximum received
    until its own editor takes it, so no peak is lost between frames.
- `TelemetryCodec` : the wire format of one frame, shared with the server and
  client so all three agree by construction.
- Two backward-compatible additions to existing headers: `VuMeter::setLevels
  (rms, peak, max)` (mirror side) and a cursor read on `SpectrumTap` /
  `WaveformTap` (`readSince (cursor, dest, maxN)`, source side). Logged in
  `doc/api-changes.md`.

`FxmeTools/net/` (JUCE module, because it implements a `juce::` interface):

- `NetBridge` : one member of the processor. Implements
  `juce::VST3ClientExtensions` to answer the IFxmeNet interface ID, owns the
  `TelemetryRegistry`, holds the role, and exposes `isMirror()` and
  `notifyStateChanged()`. Header-only, depending on nothing but
  `juce_audio_processors` and FxmeCore, so a plugin that does not use the
  FxmeTools module can still adopt it (section 4.1).

Everything else (sockets, chain engine, catalog, parameter pool) lives in the
FxmeNet repo, because no plugin needs it.

The whole diff for the Compressor:

```cpp
// PluginProcessor.h
#include <FxmeTools/net/NetBridge.h>
...
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &net; }
private:
    fxme::net::NetBridge net { *this };   // after compressor

// PluginProcessor.cpp, constructor
    net.telemetry().addMeter ("gr", compressor.getGrMeter());
```

The Equalizer adds two `addTap` lines; Cab/ConvolReverb add a
`net.notifyStateChanged()` where they load an IR. Editors untouched. The
documentation is a `doc/fxmenet-howto.md` in FxmeTools plus the demo plugin.

### 4.1 Porting cost

The cost does not grow with the size of the plugin. It grows with how its
editor reads the processor, and with what the plugin expects from its host.
Checklist for a port:

1. Everything the editor reads that is not a parameter: list it, then bind
   objects (`addMeter`, `addTap`, `addValue`), wrap computed getters
   (`addScalar` + one mirror line), and declare read-and-reset ones
   (`addPeak`).
2. State the processor derives itself (engine mode, FFT size): if the shadow
   recomputes it from its own parameters, nothing to do; if only
   `processBlock` sets it, register it as a value.
3. Heavy work the shadow must not do (background workers, IR loading, big
   FFT allocations): guard it with `isMirror()`, still storing the atomics the
   editor reads.
4. Non-parameter state that affects the DSP: call `notifyStateChanged()`.
   Cosmetic state (preset names) stays on the client, which is authoritative.
5. Host needs (transport, MIDI, latency changes, channel count): nothing to
   write in the plugin, but FxmeNet must support them before the port works.

Estimates, from reading the code on 2026-11-02:

| Plugin | What the editor reads | Plugin-side work |
| :--- | :--- | :--- |
| FxmeFX Compressor | one `VuMeter` | 4 lines, editor untouched |
| FxmeFX Equalizer | two `SpectrumTap`s | 5 lines, editor untouched |
| Dede (SpectralDede) | 8x2 region `VuMeter`s, 2 output meters with read-and-reset peaks, 8 gate openness and 8 band levels (computed, depend on the engine), 8 band peaks (read-and-reset), input `SpectrumTap`, bpm and last note | about 30 to 40 lines in the processor, editor untouched. Engine mode and FFT size need nothing (the shadow recomputes them in `parameterChanged`); optionally guard the FFT reallocation. Depends on transport, MIDI and latency changes in FxmeNet. Half a day to a day. |
| AmbiRR2 | `engine.isComputing()` and `getProgress()` (progress bar); the room views draw from parameters | about 10 lines, plus an `isMirror()` guard on the processor timer, or the shadow recomputes the image sources on the client for nothing. Depends on 16-channel slots in FxmeNet. Prerequisite: AmbiRR2 is still on the `lib/FxmeTools` submodule layout. |
| JUCE plugin without FxmeTools | its own atomics or FIFOs | CMake: add FxmeCore and the NetBridge include (about 5 lines, no FxmeTools module). Atomics: one line each. A hand-made FIFO spectrum: a small adapter (about 50 lines) implementing the stream interface. Half a day to a day. |

The rule that keeps ports cheap is already the house style: editors read
atomics and lock-free taps owned by the processor, never DSP internals under a
lock. A plugin whose editor reaches into DSP data structures directly
(walking a filter's coefficient arrays, reading a vector the audio thread
resizes) is the expensive case: those reads must first become atomics or
taps, which is a fix worth making anyway.

---

## 5. Implementation plan

Each phase ends on something runnable. The user builds; nothing here assumes
automated builds.

### Phase 0: spikes (de-risk before designing further)

1. Console app on `juce_audio_processors_headless` loads
   `FxmeCompressor.vst3` on a Linux box with `DISPLAY` unset, processes a WAV
   file, saves state. Confirms headless loading of a JUCE plugin binary.
2. Same app queries a dummy custom IID through `ExtensionsVisitor` on a
   patched Compressor. Confirms the side channel on Linux, then Windows/macOS.
3. A minimal JUCE plugin hosting a VST3 inside itself and embedding its editor,
   in Reaper on Linux (nested XEmbed is the most fragile platform).
4. Read AudioGridder's source for its jitter buffer, offline-render handling
   parameter mapping and sandboxing. It is MIT-licensed, so code can be
   reused with its copyright notice kept; prefer lifting a proven piece (jitter
   buffer, PDC handling) over rewriting it.

Exit: three green spikes, or a design change recorded here.

Spike 1 results (2026-11-02, Linux, `DISPLAY` and `WAYLAND_DISPLAY` unset;
Compressor, Equalizer, SpectralDede, AmbiRR2):

- All four load, process and save state with no display. Compressor,
  Equalizer and Dede reload bit-identical. The VST3 `IComponent` is reachable
  on every instance.
- Hard library dependencies of a JUCE plugin binary are only `libfontconfig`
  and `libfreetype`. The X11 libraries are loaded by JUCE at run time
  (`dlopen`) when they are present, even with no display. On a system
  without them (`debian:stable-slim` container with only `libfontconfig1` and
  `libfreetype6` installed: no `libX11`, `libxcb` or `libGL`) all four
  plugins load, process and reload their state bit-identical. So the server
  needs no X11 at all: its package depends on fontconfig and freetype only.
- The server must run the JUCE message loop on its main thread. AmbiRR2
  pushes parameters to its engine from a 30 Hz processor timer, so in a host
  that never dispatches messages a restored state never reaches its DSP. The
  first spike run did not dispatch, and AmbiRR2 differed by 0.029; with the
  loop run for 1 s after `prepareToPlay` it reloads bit-identical (0 ms of
  settling: still 0.014 off; 1000 and 3000 ms: identical).
- DSP that finishes work on a background thread (AmbiRR2's image sources)
  is timing-dependent when rendered faster than real time. This is not
  specific to FxmeNet (a DAW's offline bounce behaves the same), but it means
  offline renders through FxmeNet can differ between runs for such plugins.
  The fix belongs in the plugin (wait for the recompute when
  `isNonRealtime()`).
- Dede exposes 2508 parameters. This confirms the parameter pool with learn
  mapping: only mapped parameters reach the DAW, and the tier 0 generic
  editor needs search and grouping (the VST3 parameter groups), not a flat
  list. Parameters flagged not automatable must not be offered for mapping.
- JUCE's `PluginDescription::createIdentifierString()` contains a hash of
  the file path (`VST3-FxmeCompressor-37711c6c-1668ba35` on the workstation,
  `...-26caff44-1668ba35` in the container). It must never be used to match a
  plugin between client and server; match on the VST3 class ID (plus version
  and binary hash, section 2.4).
- Speed at 44.1 kHz, block 128: Compressor 1644x real time, Equalizer 392x,
  Dede 117x, AmbiRR2 (16 ch) 43x.

### Phase 1: telemetry in FxmeTools + the demo plugin

- `core/FxmeTools/net/` (`TelemetryRegistry`, `TelemetryCodec`) with core
  unit tests (round trip, unknown IDs, tap gaps).
- `VuMeter::setLevels`, `SpectrumTap/WaveformTap::readSince`.
- `FxmeTools/net/NetBridge` + the C ABI header (`FxmeNetAbi.h`, versioned).
- `FxmeNet/Examples/GainPan/` : gain, pan, stereo in/out `VuMeter`s, full house
  shell (TopBar, presets, tooltips). The howto.
- Test harness without a network: one process loads the demo twice (source and
  mirror) and pumps telemetry between them in memory; the mirror's editor must
  animate.

Exit: GainPan's mirror editor moves in the loopback harness.

### Phase 2: protocol + server (audio and control)

- `FxmeNet/Common/` : `Protocol.h` (JesuNet header, `'FXNT'`), control
  messages, discovery, SPSC ring (reuse any FxmeTools fifo before writing one).
- `FxmeNet/Server/` : `AudioWorker` (real-time thread, SCHED_FIFO on Linux),
  `ChainEngine` (VST3 instances in series, latency sum), `ControlServer`,
  `PluginCatalog` (out-of-process scan so a crashing plugin does not kill the
  scan), discovery responder, config file (plugin paths, ports, enable flags).
  Runs as a systemd service.
- A command-line test client that streams a WAV through the server and back.

Exit: a WAV through Compressor + EQ on the server is bit-identical to the
plugins processing it locally.

### Phase 3: FxmeNetPlugin, tier 0

- Processor: SPSC rings, network thread, jitter buffer, concealment, fixed
  internal block size, `setLatencySamples`, offline-render blocking mode,
  reconnect + full state push.
- Parameter pool (256 generic parameters), slot mapping in state, and a
  "learn" mapping UI (touch a pool slot in the DAW, then a control in a slot
  editor, or the reverse) with a mapping list to edit or clear assignments.
- Editor (house shell): connection header (discovery list, manual IP, ping,
  server CPU), chain strip (add/reorder/bypass/remove), plugin browser from the
  server catalog, generic parameter view.

Exit: in Reaper on the client, a chain on the server is automated, saved,
reloaded, bounced offline, and survives a server restart.

### Phase 4: tier 1 and tier 2 (shadow editors + telemetry)

- Client catalog scan; match server entries by UID + version + hash.
- Shadow instances in mirror role, embedded editors, two-way parameter sync
  (with echo suppression), `notifyStateChanged` round trips.
- Telemetry pump on the server (open slots only), telemetry receiver on the
  client writing into the shadows.
- Convert FxmeFX Compressor and Equalizer (PoC), then Limiter and Transient
  (same pattern as Compressor).

Exit: Compressor GR meter and EQ spectra animate in the DAW while the audio is
computed on the server, across two different OSes.

### Phase 5: hardening and measurements

Jitter tuning (D = 1/2/3), stress test (20 to 60 instances), latency and
CPU measurements to replace the estimates inherited from JesuNet with real
numbers, per-session worker processes, Windows/macOS server builds, CI for the
server and FxmeNetPlugin (reuse the FxmeFX workflow).

### Phase 6: plugin distribution (optional features, off by default)

Merged multi-OS `.vst3` bundles from FxmeFX CI, package signing, server to
client download into FxmeNet's folder, client to server upload behind pairing,
permanent install switch.

### Phase 7: JSFX

Bring in the JesuNet server core (EEL2 VM, `ScriptManager`, script upload).
JSFX `@gfx` cannot use the telemetry trick as is, because its drawing code is
EEL running against VM memory: the client would need its own EEL VM running
`@gfx` against mirrored variables and memory, which is a project of its own.
Start JSFX at tier 0 (sliders only) and decide the `@gfx` strategy then.

---

## 6. Decisions (confirmed 2026-11-02)

1. VST3 is the only hosted format (server and shadows) for now.
2. State is client-authoritative: the DAW session is the truth, the server a
   replica.
3. 256 generic parameters in the pool, with a learn mapping UI (Phase 3).
4. FxmeNet is its own repo, using FxmeTools (and JUCE) as sibling clones
   through FetchContent like FxmeFX, with targets `fxmenet-server`,
   `FxmeNetPlugin` and `Examples/GainPan`.
