# FxmeNet done

Newest first. Numbers are those of `todo.md`.

## 2026-11-03

- 8b complete ("Last touched" passes).
- 8c (written): `TelemetryMailbox` (receive thread to message thread,
  per-slot queue), TELEMETRY_ACTIVE with the slot's editor (kept in the
  model, re-sent after rebuild or resume), frames written into the shadows
  by the processor's timer. Checked: syntax.

- Pool reuse fix passes; Dynette and TAL Reverb 4 null against local
  plugins, so `Buses.h` (default bus layout) stays.
- 8b, user request (written): a "Last touched" button in the plugin-editor
  view of the Parameters tab (a plugin's own editor has no way to reach
  FxmeNet's mapping): shows the control last moved or grabbed there and its
  pool entry; its menu maps it to a new entry, unmaps it, or starts a learn.
  Checked: syntax.

- Dynette's loudness found by the user: a new plugin's parameters were
  auto-mapped to pool entries just freed by a removed plugin, whose
  automation lanes in the DAW then drove them (Dynette's input gain at
  +24 dB). Fixed: automatic allocation and Map take only pool entries never
  used in the project (`everUsed`, saved as `usedPool`; mapped entries count
  as used in older states); freed ones are reused by Learn, or once all 256
  have been used. Checked: syntax.
- 8b passes in Reaper, except two plugins with aux inputs processed on the
  server: Dynette very loud, TAL Reverb 4 sounding different from local.
  First suspected cause (not the actual one for Dynette, see above): the server called `enableAllBuses()`, activating
  their aux / sidechain inputs, which are silent there, while a DAW keeps a
  plugin's default layout (aux inactive); such plugins switch their
  detector or mix to that input. Fixed: `Common/FxmeNet/catalog/Buses.h`,
  `useDefaultBuses` (default layout, main buses enabled), used by the
  server's chain, the shadows, the wavclient's local reference and spike 3.
  Checked: syntax.

- 8a passes in Reaper (GainPan T2 once the "FxmeNet*" filter, which hid it,
  was narrowed to FxmeNet itself and its spike).
- 8b (written): `Shadow.h` (mirror role, quiet values, editor changes and
  gestures out), shadows in the processor (created on demand, state from
  the server, values synced at 50 Hz, state changes sent, removed with
  their slot), the slot's own editor in the Parameters tab with a switch to
  generic controls and the window fitted to it. Engine: `fetchState`,
  `setSlotState`. Checked: syntax with JUCE's warning flags and `-Wshadow`.

- 8a (written): `Common/FxmeNet/catalog/PluginFiles.h` (bundle folder,
  binary, sha256; the server's catalog uses it now), `LocalCatalog` in the
  plugin (shared background scan with dead man's pedal, cache with hashes,
  tier matching), tiers in the editor's lists with reasons on hover, local
  scan status and Rescan / Full rescan in the Stream & channels tab. The
  plugin now builds with `JUCE_PLUGINHOST_VST3` and links
  `juce_cryptography`. Checked: syntax of the plugin sources and the
  server's catalog.

- 3: spike 3 passes in Reaper on Linux: the editors of every plugin tried,
  from several vendors, embed and work (display, mouse, keyboard, resizing,
  reopening). Phase 4's shadow editors can be shown this way. The file
  dialog was replaced by a menu of the plugins found (JUCE's browser opens
  a .vst3 bundle instead of selecting it). Open: TAL Reverb 4 shows its
  editor but passes the sound dry in the spike; suspected cause, a bus
  wider than the spike's stereo buffer (the spike then skipped processing);
  the spike now processes such plugins in a scratch buffer and traces each
  plugin's buses. Confirmed: TAL Reverb 4 has two aux inputs, and works
  with the scratch buffer. Spike 3 closed. The server is not affected (its
  64-channel work buffer is cleared every block, the aux inputs get
  silence); explicit routing of extra buses is optional item 15.

- 3 (written): spike 3, `Spikes/NestedEditor` (FxmeNetSpikeNested): loads a
  VST3 chosen in a file dialog, processes the audio through it, shows its
  editor under a small bar, follows its size both ways, recreates it on
  demand, remembers the plugin and its state. Checked: syntax.

- 7 (Phase 3) complete: 7d passes in Reaper (parameter view, learn and
  manual mapping, Mapping tab, N / D with the latency, channel toggles).
  Phase 3's exit criterion met: in Reaper, a chain on the server is
  automated, saved, reloaded, bounced offline, and survives a server
  restart.
- 7d (written): editor with three tabs (`Panels.h/.cpp`): Parameters (one
  row per slot parameter with slider, value text, pool entry, Learn,
  Map / Unmap), Mapping (list, Unmap, Unmap all, learn status), Stream &
  channels (N, D, latency, channel toggles). Processor: mapping API
  (`mapPool`, `mapToFreePool`, `unmapPool`, `findPoolFor`), learn, slot
  parameter edits through the pool with gestures, stream settings
  get / set. Engine: `getSlotInfos` (no state blobs for displays),
  `currentValue`, values kept across a state refresh, server OS. Checked:
  syntax of the plugin sources and the wavclient.

- 7c complete: DAW bypass (dry, no timing jump) and server restart while
  playing (dry meanwhile, then processed again) pass in Reaper; this also
  closes the 7b restart check.
- Offline render passes in Reaper after the fix below: identical to the
  local render from the first sample, no stale blocks.
- Offline render in Reaper: the beginning differed from a local render and
  the server counted "stale" blocks. Cause: Reaper calls `prepareToPlay` at
  the start and end of each render; the engine restarted its stream at
  block 0 on the session still running (the server dropped those blocks as
  older than expected), then reconfigured, playing dry meanwhile, which an
  offline render races through. Fixed: a prepare makes the current link
  unusable at once (generation counter) until a CONFIGURE that saw it is
  done; offline, the audio thread waits for the new session (5 s at most
  per episode); `AudioLink::rebase` keeps wire numbers increasing across
  stream restarts. Every prepare still re-sends CONFIGURE, so the server's
  plugins are re-prepared like local ones. Checked: syntax.

- 7c passes in Reaper after the fixes below: no lost or concealed blocks,
  automation of pool parameters, session save and restore, and a null test
  (network chain against the same chain local, one polarity inverted)
  gives silence, so the reported latency is exact.
- 7c first run in Reaper: chopped sound. Server log: every lost block was
  "queue full" (the request queue held 15 blocks); Reaper's anticipative FX
  processing calls the plugin in bursts up to 200 ms ahead, which also left
  the client's replies no wall-clock time. Fixed: the server queue holds
  half a second of blocks (sized at CONFIGURE, the UDP thread kept out
  meanwhile); in real time the client waits for a missing reply up to a
  quarter of the callback's duration (`rescued` in the statistics);
  protocol.md 5.4 amended. Checked: syntax.

## 2026-11-02

- 7c (written): FxmeNetPlugin (`Plugin/`): buses up to 64 channels,
  transport and offline flag to the engine, bypass as delayed dry, latency
  reporting, 256-parameter pool with automatic mapping of newly loaded
  slots and echo-free two-way values, XML state (chain with states fetched
  from the server, mapping, server, stream settings), minimal editor.
  Engine: `setParameters` (one `PARAM_SET` per batch), values set while
  disconnected re-sent on resume. Protocol: parameter `value` in
  `SLOT_READY` (section 4.4), server and control test updated. Checked:
  syntax of the plugin, server `Chain.cpp`, control test and wavclient
  with JUCE's warning flags.

- 7a and 7b pass: `FxmeNetClientTests`, and `fxmenet-wavclient --engine`
  bit-identical with local processing for host buffers of 512, 128, 100 and
  random sizes offline, and 512 in real time with nothing concealed (round
  trip under 0.6 ms on loopback). First run showed a wavclient bug, not an
  engine one: a shorter last host buffer switched aligned streams to the
  larger latency at the end, and the output was realigned with that final
  latency; now whole buffers, latency taken after the first.

- 6f: out-of-process scan, cache, ABI and sha256 in the catalog, offline
  blocks, peer following: built and tested (committed). 6g (two machines)
  and 13 (mDNS) postponed.
- 7a (written): client audio-thread core, `BlockStream` and `ReplyRing`, and
  `FxmeNetClientTests` (962 checks, compiled and run standalone with g++,
  all pass). Found while writing it: the latency formula of protocol.md
  section 5.1 gave a reply no time when the host block is larger than N;
  amended to `D*N + B - N` (blocks multiples of N) or `D*N + B + N - 1`.
  Concealment holds the last sample and ramps it down (a lazily filled
  jitter buffer has nothing left to fade out when a reply is missing).
- 7b (written): `AudioLink` (one session's UDP audio, offline retries,
  statistics), `ClientEngine` (client-authoritative chain, connection thread
  with resume, rebuild and reconnection, hazard-pointer link for the audio
  thread, dry path when not connected or bypassed), `fxmenet-wavclient
  --engine [--host-block N] [--random-blocks]`. Checked: syntax with JUCE's
  warning flags and `-Wshadow`.

- 6e: telemetry, `PARAM_CHANGED` and discovery pass (committed).
- 6f (written, not yet built): out-of-process plugin scan with a cache,
  catalog sha256 and FxmeNet ABI, `setNonRealtime` from the offline flag,
  the client's UDP address followed across a resume. mDNS dropped from
  version 1 (protocol.md section 7), kept as optional item 13. Checked:
  syntax of every server source, configure.

- 6e (written, not yet built): server step 2. Telemetry pump and
  `PARAM_CHANGED` on a per-session timer (message thread), `ParamWatcher`
  with atomics only, discovery responder (broadcast) with the shared
  `Common/FxmeNet/control/Discovery.h`, options `--discovery-port` and
  `--no-discovery`. `fxmenet-wavclient` gains `--telemetry` and
  `--discover`. Checked: syntax with strict warnings.

- 6c step 1: `fxmenet-server` builds and runs (scan, listening, clean stop).
  Binary named `fxmenet-server` with `OUTPUT_NAME` (juce_add_console_app
  ignores `PRODUCT_NAME` for the file).
- 6d: `fxmenet-wavclient` builds and passes on loopback: offline runs
  bit-identical with local processing (Compressor, Compressor + Equalizer,
  Dede), real-time run without late or lost blocks. Not yet run between two
  machines.

- 6b: control channel tests pass (committed).
- 6c step 1 (written, not yet built): `fxmenet-server` in `Server/`:
  catalog scan, control handshake with resume, sessions, chain with
  lock-free snapshot swaps, per-session audio thread with ordering, offline
  hold, reply cache, `UdpPeer` sends. Checked: syntax of every source with
  strict warnings, configure. Protocol completed: acknowledgements (an empty
  frame of the request's type) and `STATE_GET`'s payload.

- 6a: protocol and transport tests pass (committed).
- 6b (written, not yet built): the control channel in
  `Common/FxmeNet/control/` (frames and binary payloads JUCE-free, JSON
  messages, `ControlConnection`), frame tests added to
  `FxmeNetProtocolTests`, new `FxmeNetControlTests` (JSON round trips, TCP
  exchange with a 1 MB state). Protocol amended: plugins are identified by
  JUCE's VST3 `uniqueId` (a 32-bit hash of the normalised class id) plus name
  and vendor, because JUCE's hosting API does not expose the 16-byte class
  id.

- Protocol reviewed, no changes. 6 (part, written, not yet built):
  `Common/` with the wire format (`Wire.h`), fragmentation
  (`Fragmentation.h`), `AudioTransport` and `UdpTransport`, and two test
  programs: `FxmeNetProtocolTests` (JUCE-free: round trips, malformed input,
  fragments in order, reversed, duplicated, interleaved, lost) and
  `FxmeNetTransportTests` (200 requests of 16 channels x 256 samples through
  loopback UDP to an echo thread, sample-exact; a wrong-token datagram
  ignored). Checked: syntax with strict warnings, configure.

- Protocol draft, `doc/protocol.md`: UDP audio (decided) with fragmentation,
  sequence-numbered blocks, a server reply cache and offline retries; TCP
  control with JSON and binary frames; latency in every audio reply;
  transport and MIDI in the request; channel masks; telemetry; discovery;
  reconnection with resume. Transport-neutral by rule, so a TCP audio
  transport stays about a day's work if tests call for it (section 11).

- 4: AudioGridder read (streamer, processor, audio worker, sandbox, message
  format). Findings in `doc/audiogridder-notes.md`: ideas adopted (latency in
  every reply, bypass delayed by the latency, sandbox with a direct port),
  practices avoided (blocking and allocating on the audio thread, raw-struct
  wire format), and the case for UDP with sequence-numbered retries. Its
  code is not reused.

- 5 (Phase 1 done): GainPan and the loopback harness built and run on
  Linux. The mirror's meters follow the source's and freeze with telemetry
  off, parameters follow both ways, schema `inL inR outL outR`, frames of
  104 bytes (8-byte header, 4 meter channels of 24). This also settles
  spike 2 on Linux: the VST3 side channel works. FxmeTools pin bumped to
  94e4e4d.

- 5a: `FxmeCoreNetTelemetryTests` pass (FxmeTools d8b0cff, "Additions for
  FxmeNet").
- 5b, 5c (written, not yet built): `Examples/GainPan` (the howto plugin, full
  shell, NetBridge with four meters) and `Tools/Loopback` (the harness). In
  FxmeTools: `fxme::net::queryNetPlugin()` and `UnknownCompatible` in
  `NetAbi.h`, so a host needs no VST3 SDK headers; `doc/fxmenet-howto.md`.
  Checked: syntax of GainPan (with a stand-in JuceHeader) and the harness
  with `-Wshadow -Wconversion -Wsign-conversion -Wfloat-equal`, clean; CMake
  configure.

- 5 (part, written, not yet built): FxmeTools telemetry. Core:
  `net/TelemetryCodec.h`, `net/TelemetryRegistry.h`, `net/NetAbi.h`, the
  `VuMeter` / `SpectrumTap` / `WaveformTap` additions, tests
  (`CoreNetTelemetryTests.cpp`). Module: `net/NetBridge.h`. Checked: syntax
  with `-Wconversion -Wsign-conversion -Wfloat-equal` (clean apart from three
  existing `VuMeter.h` lines), the core JUCE guard, and `NetBridge.h` against
  both JUCE 9.0.4 and 8.0.12. Logged in FxmeTools `doc/api-changes.md`.

- 1 (done): spike 1 with the settle step. AmbiRR2 bit-identical once the
  message loop runs (0 ms: 0.014 off; 1000 and 3000 ms: identical). In a
  `debian:stable-slim` container with only fontconfig and freetype (no X11,
  xcb or GL libraries), all four plugins load, process and reload
  bit-identical. The server needs no X11. Also found: JUCE's plugin
  identifier string hashes the file path, so it cannot match plugins across
  machines.

- Repository scaffold: root CMake (macOS settings before `project()`),
  `cmake/FxmeDeps.cmake` copied from FxmeFX, `cmake/Dependencies.cmake`
  pinning JUCE 9.0.4 and FxmeTools 1a767df, doc triplet. Configure checked:
  both dependencies resolve to the sibling clones, at their pins.
- 1 (first run): `Spikes/HeadlessHost` built and run on Compressor,
  Equalizer, SpectralDede and AmbiRR2 with no display. All load, process and
  round-trip their state; three bit-identical, AmbiRR2 not (its 30 Hz
  parameter timer never fired: the spike did not run the message loop).
  Results and consequences in `plan.md`, Phase 0. Then: settle step added
  (`settleMs`, message loop run after `prepareToPlay`), `-Wfloat-equal`
  warning fixed, binary name corrected in the docs.
- Plan written and reviewed (`plan.md`): tiers 0/1/2, shadow instances,
  telemetry, the case against just using AudioGridder, porting costs, channel
  masks. Decisions confirmed: VST3 only, client-authoritative state, 256
  parameters with learn mapping, own repo with sibling clones.
